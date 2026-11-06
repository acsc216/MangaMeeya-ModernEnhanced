/*
 * test_7zspi.c - console test harness for 7z.spi (32-bit)
 *
 * Usage:
 *   test_7zspi.exe <path-to-7z.spi> <archive.7z> [options]
 *
 * Options:
 *   --mem      also test the memory-image input path
 *   --cancel   call GetFile with a callback that cancels immediately
 *   --quiet    do not print every entry
 *
 * Build (x86):
 *   cl /nologo /MT /O2 /W0 test_7zspi.c /Fe:test_7zspi.exe
 *
 * The DLL looks for 7za.exe next to itself.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char method[8];
    unsigned long position;
    unsigned long compsize;
    unsigned long filesize;
    long timestamp;
    char path[200];
    char filename[200];
    unsigned long crc;
} FILEINFO;

typedef int (WINAPI *PFN_GETPLUGININFO)(int, LPSTR, int);
typedef int (WINAPI *PFN_ISSUPPORTED)(LPSTR, DWORD);
typedef int (WINAPI *PFN_GETARCHIVEINFO)(LPSTR, long, UINT, HLOCAL*);
typedef int (WINAPI *PFN_GETFILEINFO)(LPSTR, long, LPSTR, UINT, FILEINFO*);
typedef int (__stdcall *PROGRESS_CB)(int, int, long);
typedef int (WINAPI *PFN_GETFILE)(LPSTR, long, LPSTR, UINT, PROGRESS_CB, long);

static int g_quiet;
static int g_cb_calls;

static int __stdcall progress_cb(int num, int denom, long data)
{
    (void)data;
    g_cb_calls++;
    if (!g_quiet && (num == 0 || num == denom || g_cb_calls % 20 == 0))
        printf("    progress %d/%d\n", num, denom);
    return 0;  /* Susie 00AM GetFile: zero = continue, non-zero = abort */
}

static int __stdcall cancel_cb(int num, int denom, long data)
{
    (void)denom; (void)data;
    g_cb_calls++;
    printf("    cancel_cb #%d (%d)\n", g_cb_calls, num);
    return 1;  /* non-zero cancels a 00AM GetFile */
}

static DWORD crc_table[256];
static void crc_init(void)
{
    DWORD i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
}
static DWORD crc32_of(const BYTE* p, DWORD n)
{
    DWORD c = 0xFFFFFFFFUL;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static int read_whole_file(const char* path, BYTE** out, DWORD* out_size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    BYTE* buf;
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    buf = (BYTE*)malloc(size ? size : 1);
    if (!buf) { CloseHandle(h); return 0; }
    if (size && (!ReadFile(h, buf, size, &got, NULL) || got != size)) {
        free(buf); CloseHandle(h); return 0;
    }
    CloseHandle(h);
    *out = buf; *out_size = size;
    return 1;
}

static int test_file_mode(PFN_GETARCHIVEINFO getinfo, PFN_GETFILEINFO getfileinfo,
                          PFN_GETFILE getfile, const char* archive)
{
    HLOCAL h = NULL;
    FILEINFO* fi;
    int n = 0, i, fails = 0;
    DWORD t0 = GetTickCount();
    DWORD t_info = 0, t_file = 0, t_crc = 0, t_scan = 0;
    DWORD t_first = 0, t_slowest = 0;

    if (getinfo((LPSTR)archive, 0, 0, &h) != 0 || !h) {
        printf("FAIL GetArchiveInfo(%s)\n", archive);
        return 1;
    }
    fi = (FILEINFO*)h;
    while (fi[n].method[0]) n++;
    printf("GetArchiveInfo: %d entries\n", n);

    for (i = 0; i < n; i++) {
        char full[512];
        FILEINFO one;
        HLOCAL out = NULL;
        int rc;
        DWORD t_one = GetTickCount();

        if (fi[i].path[0])
            _snprintf(full, sizeof(full), "%s\\%s", fi[i].path, fi[i].filename);
        else
            _snprintf(full, sizeof(full), "%s", fi[i].filename);
        full[sizeof(full) - 1] = 0;

        {
            DWORD t = GetTickCount();
            if (getfileinfo((LPSTR)archive, 0, full, 0, &one) != 0) {
                printf("  FAIL GetFileInfo %s\n", full);
                fails++;
            } else if (one.position != fi[i].position || one.filesize != fi[i].filesize) {
                printf("  FAIL GetFileInfo mismatch %s: want pos=%lu size=%lu, got pos=%lu size=%lu\n",
                       full, fi[i].position, fi[i].filesize, one.position, one.filesize);
                fails++;
            }
            t_info += GetTickCount() - t;
        }

        {
            DWORD t = GetTickCount();
            rc = getfile((LPSTR)archive, (long)fi[i].position, (LPSTR)&out, 0x100,
                         progress_cb, 0);
            t = GetTickCount() - t;
            t_file += t;
            if (i == 0) t_first = t;
            if (t > t_slowest) t_slowest = t;
            if (!g_quiet || i < 3 || i == n - 1)
                printf("  page %d/%d: GetFile %lu ms\n", i + 1, n, t);
        }
        if (rc != 0 || !out) {
            printf("  FAIL GetFile %s rc=%d\n", full, rc);
            fails++;
            continue;
        }
        if ((DWORD)GlobalSize(out) != fi[i].filesize) {
            /* LocalAlloc sizes may be rounded; compare CRC instead */
        }
        {
            DWORD t = GetTickCount();
            if (fi[i].crc && crc32_of((const BYTE*)out, fi[i].filesize) != fi[i].crc) {
                printf("  FAIL CRC %s\n", full);
                fails++;
            }
            t_crc += GetTickCount() - t;
        }
        if (!g_quiet)
            printf("  ok %-10lu %-8lu %s\n", fi[i].filesize, fi[i].position, full);
        LocalFree(out);

        if (i == 0) {
            char dir[MAX_PATH];
            WCHAR wdir[MAX_PATH], wleaf[MAX_PATH], wpath[MAX_PATH];
            DWORD n = GetTempPathA(MAX_PATH, dir);
            int rc2;
            if (n && n + 32 < MAX_PATH) {
                strcat(dir, "mm7ztest_out");
                CreateDirectoryA(dir, NULL);
                MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH);
                MultiByteToWideChar(CP_UTF8, 0, fi[i].filename, -1, wleaf, MAX_PATH);
                _snwprintf(wpath, MAX_PATH, L"%s\\%s", wdir, wleaf);
                DeleteFileW(wpath);
                rc2 = getfile((LPSTR)archive, (long)fi[i].position, dir, 0, NULL, 0);
                if (rc2 != 0) {
                    printf("  FAIL disk-output GetFile rc=%d\n", rc2);
                    fails++;
                } else if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) {
                    printf("  FAIL disk-output file missing\n");
                    fails++;
                } else {
                    printf("  disk-output ok: %s\\<leaf>\n", dir);
                }
                DeleteFileW(wpath);
            }
        }
    }
    LocalFree(h);
    printf("file mode: %d entries, %d failures, %lu ms (info=%lu file=%lu crc=%lu)\n",
           n, fails, GetTickCount() - t0, t_info, t_file, t_crc);
    printf("  first page %lu ms, slowest page %lu ms\n", t_first, t_slowest);
    return fails ? 1 : 0;
}

static int test_mem_mode(PFN_GETARCHIVEINFO getinfo, PFN_GETFILE getfile,
                         const char* archive)
{
    BYTE* img; DWORD size;
    HLOCAL h = NULL;
    FILEINFO* fi;
    int n = 0, i, fails = 0;

    if (!read_whole_file(archive, &img, &size)) {
        printf("FAIL reading archive into memory\n");
        return 1;
    }
    if (getinfo((LPSTR)img, (long)size, 1, &h) != 0 || !h) {
        printf("FAIL GetArchiveInfo(memory)\n");
        free(img);
        return 1;
    }
    fi = (FILEINFO*)h;
    while (fi[n].method[0]) n++;
    printf("memory mode: %d entries\n", n);

    for (i = 0; i < n; i++) {
        HLOCAL out = NULL;
        int rc = getfile((LPSTR)img, (long)fi[i].position, (LPSTR)&out, 0x101,
                         progress_cb, 0);
        if (rc != 0 || !out) {
            printf("  FAIL GetFile(memory) #%d rc=%d\n", i, rc);
            fails++;
            continue;
        }
        if (fi[i].crc && crc32_of((const BYTE*)out, fi[i].filesize) != fi[i].crc) {
            printf("  FAIL CRC(memory) #%d\n", i);
            fails++;
        }
        LocalFree(out);
    }
    LocalFree(h);
    free(img);
    printf("memory mode: %d entries, %d failures\n", n, fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    HMODULE dll;
    PFN_GETPLUGININFO getplugininfo;
    PFN_ISSUPPORTED issupported;
    PFN_GETARCHIVEINFO getinfo;
    PFN_GETFILEINFO getfileinfo;
    PFN_GETFILE getfile;
    char buf[256];
    int i, want_mem = 0, want_cancel = 0, want_cancel_only = 0;
    const char* dllpath;
    const char* archive;
    HANDLE fh;
    int rc;

    if (argc < 3) {
        printf("usage: %s <7z.spi> <archive.7z> [--mem] [--cancel] [--quiet]\n", argv[0]);
        return 2;
    }
    dllpath = argv[1];
    archive = argv[2];
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--mem")) want_mem = 1;
        else if (!strcmp(argv[i], "--cancel")) want_cancel = 1;
        else if (!strcmp(argv[i], "--cancel-only")) { want_cancel = 1; want_cancel_only = 1; }
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
    }

    crc_init();
    SetEnvironmentVariableA("MM7Z_LOG", "1");

    dll = LoadLibraryA(dllpath);
    if (!dll) { printf("FAIL LoadLibrary %s (%lu)\n", dllpath, GetLastError()); return 1; }
    getplugininfo = (PFN_GETPLUGININFO)GetProcAddress(dll, "GetPluginInfo");
    issupported = (PFN_ISSUPPORTED)GetProcAddress(dll, "IsSupported");
    getinfo = (PFN_GETARCHIVEINFO)GetProcAddress(dll, "GetArchiveInfo");
    getfileinfo = (PFN_GETFILEINFO)GetProcAddress(dll, "GetFileInfo");
    getfile = (PFN_GETFILE)GetProcAddress(dll, "GetFile");
    if (!getplugininfo || !issupported || !getinfo || !getfileinfo || !getfile) {
        printf("FAIL exports missing\n");
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    getplugininfo(0, buf, sizeof(buf));
    printf("plugin api: %s\n", buf);
    getplugininfo(1, buf, sizeof(buf));
    printf("plugin: %s\n", buf);

    fh = CreateFileA(archive, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) { printf("FAIL open %s\n", archive); return 1; }
    rc = issupported((LPSTR)archive, (DWORD)fh);
    CloseHandle(fh);
    if (!rc) { printf("FAIL IsSupported\n"); return 1; }
    printf("IsSupported: ok\n");

    /* Run the cancel test first so the archive is not already cached. */
    if (want_cancel) {
        HLOCAL h = NULL;
        FILEINFO* fi;
        int n = 0;
        HLOCAL out = NULL;
        printf("cancel test...\n");
        if (getinfo((LPSTR)archive, 0, 0, &h) == 0 && h) {
            fi = (FILEINFO*)h;
            while (fi[n].method[0]) n++;
            g_cb_calls = 0;
            rc = getfile((LPSTR)archive, (long)fi[n > 1 ? n - 1 : 0].position,
                         (LPSTR)&out, 0x100, cancel_cb, 0);
            printf("cancel test: rc=%d calls=%d\n", rc, g_cb_calls);
            if (rc != 1) { printf("FAIL expected cancel rc=1\n"); rc = 1; }
            else rc = 0;
            if (out) LocalFree(out);
            LocalFree(h);
        } else {
            printf("FAIL cancel test setup\n");
            rc = 1;
        }
        if (rc) { FreeLibrary(dll); printf("RESULT: FAIL\n"); return rc; }
        if (want_cancel_only) {
            FreeLibrary(dll);
            printf("RESULT: PASS\n");
            return 0;
        }
    }

    rc = test_file_mode(getinfo, getfileinfo, getfile, archive);
    if (want_mem) rc |= test_mem_mode(getinfo, getfile, archive);

    FreeLibrary(dll);
    printf(rc ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return rc;
}
