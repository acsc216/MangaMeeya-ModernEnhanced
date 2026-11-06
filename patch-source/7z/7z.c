/*
 * 7z.spi - 7-Zip archive plug-in for MangaMeeya (Susie 32-bit 00AM)
 *
 * The plug-in runs 7za.exe (x86) next to this DLL.
 *
 * File input  : the archive is opened directly by 7za.exe.
 * Memory input: the image is written once to %TEMP%\MangaMeeya_7z.
 *
 * Non-solid archives are extracted entry by entry through stdout.
 *
 * Solid archives are decoded into a per-archive cache under
 * %TEMP%\MangaMeeya_7z\<hash>.  A solid 7z archive is a sequence of
 * independent "blocks" (folders); 7-Zip stores the block index of every
 * file and prints it in `l -slt` output ("Blocks = N" in the header and
 * "Block = k" per file).  LZMA2 decoding is sequential *within* one block,
 * but different blocks are independent, so one 7za.exe process is started
 * per block and blocks are decoded in parallel on multiple cores.
 *
 * MangaMeeya reads pages one at a time, so GetFile also serves a page as
 * soon as its block has produced it - the first page appears without
 * waiting for the whole archive to be extracted.  The remainder keeps
 * decoding in the background while the reader displays pages.
 *
 * MangaMeeya passes fileInfo.position back as the "len" argument of
 * GetFile, so position is used as the entry identifier.
 *
 * Build (32-bit):
 *   cl /nologo /LD /MT /O2 /W0 7z.c /link /DEF:7z.def /OUT:7z.spi
 *
 * Install with 7za.exe and 7-Zip's License.txt in SusiePlugin\.
 *
 * Environment:
 *   MM7Z_LOG=1      append diagnostics to %TEMP%\7zspi.log
 *   MM7Z_THREADS=n  parallel block decoders (1..16, default = CPU count, max 8)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <time.h>

#define SEVENZ_SIG_OK(p) ((p)[0]==0x37 && (p)[1]==0x7A && (p)[2]==0xBC && \
                          (p)[3]==0xAF && (p)[4]==0x27 && (p)[5]==0x1C)

#define MAX_WORKERS     16
#define ERR_UNKNOWN      2
#define ERR_BROKEN       3
#define ERR_MEMORY       4

typedef int (__stdcall *PROGRESS_CB)(int, int, long);

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

typedef struct {
    char*  name_utf8;
    WCHAR* name_w;
    ULONGLONG size;
    ULONGLONG packsize;
    DWORD  crc;
    long   mtime;
    int    is_dir;
    int    block;          /* solid block index, -1 if unknown */
    ULONG  id;
    char   method[8];
} ENTRY;

struct ARCHIVE;

typedef struct WORKER {
    struct ARCHIVE* a;
    int    index;
    int*   blocks;
    int    nblocks;
    HANDLE thread;
    BOOL   failed;
} WORKER;

typedef struct ARCHIVE {
    char        key[MAX_PATH * 2];
    WCHAR       archive_w[MAX_PATH * 2];
    BOOL        solid;
    int         block_count;      /* number of solid blocks we will use */
    ENTRY*      entries;
    int         count;
    WCHAR       cache_w[MAX_PATH * 2];
    char        cache_hash[24];

    CRITICAL_SECTION cs;
    HANDLE      done_event;       /* manual reset: extraction attempt finished */
    volatile LONG cancel;
    BOOL        extract_started;
    BOOL        worker_failed;
    volatile LONG extract_failed;
    volatile LONG extract_cancelled;
    int         workers_left;
    WORKER*     workers;
    int         worker_count;
    HANDLE      procs[MAX_WORKERS];

    ULONGLONG*  block_bytes;
    volatile LONG* block_pct;
    int         block_slots;
    ULONGLONG   total_bytes;
    BOOL        cache_ready;

    struct ARCHIVE* next;
} ARCHIVE;

typedef struct {
    volatile LONG*    cancel;
    volatile LONG*    pct;
    CRITICAL_SECTION* cs;
    HANDLE*           proc_slot;
} RUNCANCEL;

static HINSTANCE g_self;
static CRITICAL_SECTION g_lock;
static BOOL g_lock_init;
static ARCHIVE* g_archives;
static WCHAR g_7za[MAX_PATH];
static BOOL g_log;
static int  g_threads;
static HANDLE g_job;
static volatile LONG g_list_seq;
static DWORD g_crc_table[256];

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void mm_logf(const char* fmt, ...)
{
    char buf[2048], path[MAX_PATH];
    va_list ap; FILE* f; DWORD n;
    if (!g_log) return;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap); buf[sizeof(buf)-1] = 0;
    n = GetTempPathA(MAX_PATH, path);
    if (!n) return;
    lstrcatA(path, "7zspi.log");
    f = fopen(path, "a");
    if (f) { fprintf(f, "[%lu] %s\r\n", GetTickCount(), buf); fclose(f); }
}

static void log_wide(const char* prefix, const WCHAR* w)
{
    char a[2048]; int n;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a), NULL, NULL);
    if (n <= 0) a[0] = 0; else a[sizeof(a)-1] = 0;
    mm_logf("%s%s", prefix, a);
}

static int copy_info(const char* s, LPSTR buf, int buflen)
{
    int n = lstrlenA(s);
    if (buflen <= 0) return 0;
    if (n > buflen - 1) n = buflen - 1;
    memcpy(buf, s, n); buf[n] = 0; return n;
}

/* Compare archive names stored as UTF-8 byte strings.  Do *not* use
 * lstrcmpiA/CompareStringA here: under CP936 (and other DBCS code pages)
 * the UTF-8 bytes are mistaken for double-byte characters, so e.g.
 * "第1巻" and "第2巻" compare equal (the 1/2 byte is swallowed as a trail
 * byte).  Byte-wise ASCII folding is correct for UTF-8 and treats '/' and
 * '\\' as the same separator. */
static BOOL name_eq(const char* a, const char* b)
{
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return FALSE;
        if (!ca) return TRUE;
    }
}

static ULONGLONG fnv1a_bytes(const void* p, size_t n, ULONGLONG h)
{
    const unsigned char* s = (const unsigned char*)p;
    while (n--) { h ^= *s++; h *= 1099511628211ULL; }
    return h;
}

static ULONGLONG fnv1a_wstr(const WCHAR* s)
{
    ULONGLONG h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned)(*s & 0xff); h *= 1099511628211ULL;
        h ^= (unsigned)((*s >> 8) & 0xff); h *= 1099511628211ULL;
    }
    return h;
}

static void hash_path_cache(const WCHAR* path, char* out /* >=17 */)
{
    ULONGLONG h = fnv1a_wstr(path);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        ULONGLONG mt = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) |
                        fad.ftLastWriteTime.dwLowDateTime;
        h ^= sz; h *= 1099511628211ULL;
        h ^= mt; h *= 1099511628211ULL;
    }
    _snprintf(out, 17, "%016I64x", h);
}

/* ------------------------------------------------------------------ */
/* CRC32                                                               */
/* ------------------------------------------------------------------ */

static void crc32_init(void)
{
    DWORD i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
}

static DWORD crc32_buf(const BYTE* p, DWORD n)
{
    DWORD c = 0xFFFFFFFFUL;
    while (n--) c = g_crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

/* ------------------------------------------------------------------ */
/* path helpers                                                        */
/* ------------------------------------------------------------------ */

static BOOL is_sep(char c) { return c == '\\' || c == '/'; }

static int ansi_charlen(const char* s)
{
    if (s[0] && s[1] && IsDBCSLeadByteEx(CP_ACP, (BYTE)(unsigned char)s[0]))
        return 2;
    return 1;
}

static BOOL name_matches_lossy(const char* comp, const WCHAR* wide)
{
    char buf[2048]; int n;
    n = WideCharToMultiByte(CP_ACP, 0, wide, -1, buf, sizeof(buf), NULL, NULL);
    if (n > 0 && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE,
                                comp, -1, buf, -1) == CSTR_EQUAL)
        return TRUE;
    n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, buf,
                            sizeof(buf), NULL, NULL);
    if (n > 0 && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE,
                                comp, -1, buf, -1) == CSTR_EQUAL)
        return TRUE;
    return FALSE;
}

/* Recover the real wide path from a possibly lossy ANSI path. */
static BOOL resolve_ansi_path(const char* ansi, WCHAR* out, int cap)
{
    char full[MAX_PATH * 4];
    const char* p;
    WCHAR cur[2048];
    int curlen = 0;
    DWORD n;

    if (!ansi || !ansi[0] || cap < 8) return FALSE;
    n = GetFullPathNameA(ansi, sizeof(full), full, NULL);
    p = (n > 0 && n < sizeof(full)) ? full : ansi;

    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        cur[0] = p[0]; cur[1] = L':'; cur[2] = L'\\'; cur[3] = 0; curlen = 3;
        p += 2;
        while (is_sep(*p)) p++;
    } else if (p[0] == '\\' && p[1] == '\\') {
        const char* q = p + 2;
        const char* s1 = q;
        int l1, l2, w1, w2, pos;
        const char* s2;
        while (*q && !is_sep(*q)) q += ansi_charlen(q);
        l1 = (int)(q - s1);
        if (!*q) return FALSE;
        s2 = q + 1; q = s2;
        while (*q && !is_sep(*q)) q += ansi_charlen(q);
        l2 = (int)(q - s2);
        if (l1 <= 0 || l2 <= 0) return FALSE;
        cur[0] = L'\\'; cur[1] = L'\\';
        w1 = MultiByteToWideChar(CP_ACP, 0, s1, l1, cur + 2, 1000);
        if (w1 <= 0) return FALSE;
        pos = 2 + w1; cur[pos++] = L'\\';
        w2 = MultiByteToWideChar(CP_ACP, 0, s2, l2, cur + pos, 1000);
        if (w2 <= 0) return FALSE;
        pos += w2; cur[pos++] = L'\\'; cur[pos] = 0; curlen = pos;
        p = q;
        while (is_sep(*p)) p++;
    } else {
        return FALSE;
    }

    while (*p) {
        const char* e;
        int clen, base, wl, need;
        char comp[1024];
        WCHAR wc[1024], cand[2048], pat[2048];
        WIN32_FIND_DATAW fd; HANDLE h; BOOL found = FALSE;

        while (is_sep(*p)) p++;
        if (!*p) break;
        e = p;
        while (*e && !is_sep(*e)) e += ansi_charlen(e);
        clen = (int)(e - p);
        if (clen <= 0 || clen >= (int)sizeof(comp)) return FALSE;
        memcpy(comp, p, clen); comp[clen] = 0; p = e;

        base = curlen;
        if (base > 0 && cur[base - 1] != L'\\') {
            cur[base++] = L'\\'; cur[base] = 0;
        }
        wl = MultiByteToWideChar(CP_ACP, 0, comp, -1, wc, 1024);
        if (wl <= 0) return FALSE;
        need = base + wl;
        if (need > 2048) return FALSE;
        memcpy(cand, cur, base * sizeof(WCHAR));
        memcpy(cand + base, wc, wl * sizeof(WCHAR));
        if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
            memcpy(cur, cand, need * sizeof(WCHAR));
            curlen = base + wl - 1;
            continue;
        }
        if (base + 2 > 2048) return FALSE;
        memcpy(pat, cur, base * sizeof(WCHAR));
        pat[base] = L'*'; pat[base + 1] = 0;
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return FALSE;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (name_matches_lossy(comp, fd.cFileName)) {
                int l = (int)wcslen(fd.cFileName);
                if (base + l + 1 > 2048) break;
                memcpy(cur, cand, base * sizeof(WCHAR));
                memcpy(cur + base, fd.cFileName, (l + 1) * sizeof(WCHAR));
                curlen = base + l; found = TRUE; break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!found) return FALSE;
    }
    if (curlen + 1 > cap) return FALSE;
    memcpy(out, cur, (curlen + 1) * sizeof(WCHAR));
    return TRUE;
}

static void get_plugin_dir(WCHAR* dir, int cap)
{
    WCHAR path[MAX_PATH]; WCHAR* q;
    GetModuleFileNameW(g_self, path, MAX_PATH);
    q = wcsrchr(path, L'\\');
    if (q) q[1] = 0;
    wcsncpy(dir, path, cap - 1); dir[cap - 1] = 0;
}

static void find_7za(void)
{
    WCHAR dir[MAX_PATH], cand[MAX_PATH];
    if (g_7za[0]) return;
    dir[0] = 0;
    get_plugin_dir(dir, MAX_PATH);
    _snwprintf(cand, MAX_PATH, L"%s7za.exe", dir);
    if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
        wcscpy(g_7za, cand); return;
    }
    wcscpy(g_7za, L"7za.exe"); /* PATH */
}

/* Build "<cache>\<archive relative path>", rejecting absolute paths,
 * drive/ADS colons and ".." components. */
static BOOL build_cache_path(const ARCHIVE* a, const WCHAR* name, WCHAR* out, int cap)
{
    int o, i;

    if (!name || !name[0]) return FALSE;
    if (name[0] == L'\\' || name[0] == L'/') return FALSE;
    o = (int)wcslen(a->cache_w);
    if (o + 2 >= cap) return FALSE;
    memcpy(out, a->cache_w, o * sizeof(WCHAR));
    out[o++] = L'\\';

    i = 0;
    while (name[i]) {
        int start;
        while (name[i] == L'\\' || name[i] == L'/') i++;
        if (!name[i]) break;
        start = o;
        while (name[i] && name[i] != L'\\' && name[i] != L'/') {
            if (name[i] == L':') return FALSE;
            if (o + 1 >= cap) return FALSE;
            out[o++] = name[i++];
        }
        if (o - start == 2 && out[start] == L'.' && out[start + 1] == L'.')
            return FALSE;
        if (o == start) continue;
        if (name[i]) {
            if (o + 1 >= cap) return FALSE;
            out[o++] = L'\\';
        }
    }
    out[o] = 0;
    return o > (int)wcslen(a->cache_w) + 1;
}

/* ------------------------------------------------------------------ */
/* run 7za.exe                                                         */
/* ------------------------------------------------------------------ */

static DWORD run_7za(const WCHAR* cmdline, BYTE** out, DWORD* out_size,
                     BOOL capture, RUNCANCEL* rc)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE rpipe = NULL, wpipe = NULL, nul = INVALID_HANDLE_VALUE;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BYTE* buf = NULL;
    DWORD cap = 0, len = 0, got = 0;
    BOOL ok;
    DWORD exitcode = (DWORD)-1;
    int last_pct = -1;
    BOOL killed = FALSE;

    if (out) *out = NULL;
    if (out_size) *out_size = 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0))
        return (DWORD)-1;
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : NULL;

    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessW(NULL, (LPWSTR)cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wpipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rpipe);
        return (DWORD)-1;
    }
    CloseHandle(pi.hThread);
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);

    if (rc && rc->proc_slot) {
        EnterCriticalSection(rc->cs);
        *rc->proc_slot = pi.hProcess;
        LeaveCriticalSection(rc->cs);
    }

    for (;;) {
        BYTE chunk[65536];
        if (!killed && rc && rc->cancel && *rc->cancel) {
            TerminateProcess(pi.hProcess, 1);
            killed = TRUE;
        }
        if (!ReadFile(rpipe, chunk, sizeof(chunk), &got, NULL) || got == 0)
            break;
        if (rc && rc->pct) {
            DWORD q;
            for (q = 0; q < got; q++) {
                if (chunk[q] == '%') {
                    int v = 0, k = (int)q - 1, mul = 1, digs = 0;
                    while (k >= 0 && digs < 3 && chunk[k] >= '0' && chunk[k] <= '9') {
                        v += (chunk[k] - '0') * mul; mul *= 10; k--; digs++;
                    }
                    if (digs > 0 && v >= 0 && v <= 100 && v != last_pct) {
                        last_pct = v;
                        InterlockedExchange(rc->pct, v);
                    }
                }
            }
        }
        if (capture) {
            if (len + got + 1 > cap) {
                DWORD ncap = cap ? cap * 2 : 262144;
                BYTE* nb;
                while (ncap < len + got + 1) ncap *= 2;
                nb = (BYTE*)realloc(buf, ncap);
                if (!nb) {
                    free(buf); buf = NULL;
                    TerminateProcess(pi.hProcess, 1);
                    break;
                }
                buf = nb; cap = ncap;
            }
            memcpy(buf + len, chunk, got);
            len += got;
        }
    }
    CloseHandle(rpipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitcode);

    if (rc && rc->proc_slot) {
        EnterCriticalSection(rc->cs);
        *rc->proc_slot = NULL;
        LeaveCriticalSection(rc->cs);
    }
    CloseHandle(pi.hProcess);

    if (capture && buf) {
        buf[len] = 0;
        *out = buf;
        if (out_size) *out_size = len;
    } else {
        free(buf);
    }
    return exitcode;
}

/* ------------------------------------------------------------------ */
/* listing                                                             */
/* ------------------------------------------------------------------ */

static void free_entries(ARCHIVE* a)
{
    int i;
    if (!a->entries) return;
    for (i = 0; i < a->count; i++) {
        free(a->entries[i].name_utf8);
        free(a->entries[i].name_w);
    }
    free(a->entries);
    a->entries = NULL;
    a->count = 0;
}

static void add_entry(ARCHIVE* a, ENTRY* e)
{
    ENTRY* ne = (ENTRY*)realloc(a->entries, sizeof(ENTRY) * (a->count + 1));
    if (!ne) {
        free(e->name_utf8); free(e->name_w);
        return;
    }
    a->entries = ne;
    a->entries[a->count] = *e;
    a->count++;
}

static void flush_entry(ARCHIVE* a, ENTRY* cur, int* have)
{
    if (!*have) return;
    if (!cur->is_dir) add_entry(a, cur);
    else { free(cur->name_utf8); free(cur->name_w); }
    ZeroMemory(cur, sizeof(*cur));
    cur->block = -1;
    *have = 0;
}

static void parse_listing(const char* text, ARCHIVE* a)
{
    const char* p = text;
    const char* sep = NULL;
    ENTRY cur;
    int have = 0;

    ZeroMemory(&cur, sizeof(cur));
    cur.block = -1;

    /* header section, up to the line of dashes */
    while (*p) {
        const char* eol = p;
        int ll;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        ll = (int)(eol - p);

        if (ll >= 10 && !strncmp(p, "----------", 10)) {
            sep = p;
            p = eol;
            while (*p == '\r' || *p == '\n') p++;
            break;
        }
        if (!a->solid && ll > 8 && !strncmp(p, "Solid = ", 8) && p[8] == '+')
            a->solid = TRUE;
        else if (ll > 9 && !strncmp(p, "Blocks = ", 9))
            a->block_count = atoi(p + 9);

        p = eol;
        while (*p == '\r' || *p == '\n') p++;
    }
    if (!sep) return;

    while (*p) {
        const char* eol = p;
        int ll;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        ll = (int)(eol - p);

        if (ll == 0) {
            flush_entry(a, &cur, &have);
            p = eol;
            while (*p == '\r' || *p == '\n') p++;
            continue;
        }

#define KEYIS(k) (ll > (int)strlen(k) && !strncmp(p, k, strlen(k)) && p[strlen(k)] == ' ')
        if (KEYIS("Path =")) {
            const char* v = p + 7;
            int vl = ll - 7, wl;
            flush_entry(a, &cur, &have);
            have = 1;
            cur.name_utf8 = (char*)malloc(vl + 1);
            if (cur.name_utf8) { memcpy(cur.name_utf8, v, vl); cur.name_utf8[vl] = 0; }
            wl = MultiByteToWideChar(CP_UTF8, 0, v, vl, NULL, 0);
            if (wl > 0) {
                cur.name_w = (WCHAR*)malloc((wl + 1) * sizeof(WCHAR));
                if (cur.name_w) {
                    MultiByteToWideChar(CP_UTF8, 0, v, vl, cur.name_w, wl);
                    cur.name_w[wl] = 0;
                }
            }
        } else if (KEYIS("Size =")) {
            cur.size = _strtoui64(p + 7, NULL, 10);
        } else if (KEYIS("Packed Size =")) {
            cur.packsize = _strtoui64(p + 14, NULL, 10);
        } else if (KEYIS("CRC =")) {
            cur.crc = (DWORD)strtoul(p + 6, NULL, 16);
        } else if (KEYIS("Block =")) {
            cur.block = atoi(p + 8);
        } else if (KEYIS("Attributes =")) {
            if (p[13] == 'D' || p[13] == 'd') cur.is_dir = 1;
        } else if (KEYIS("Method =")) {
            int vl = ll - 9; if (vl > 7) vl = 7;
            if (vl > 0) memcpy(cur.method, p + 9, vl);
        } else if (KEYIS("Modified =")) {
            int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
            if (sscanf(p + 11, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &hh, &mi, &ss) == 6) {
                struct tm tmv; time_t tt;
                ZeroMemory(&tmv, sizeof(tmv));
                tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
                tmv.tm_hour = hh; tmv.tm_min = mi; tmv.tm_sec = ss;
                tt = mktime(&tmv);
                cur.mtime = (long)tt;
            }
        }
#undef KEYIS
        p = eol;
        while (*p == '\r' || *p == '\n') p++;
    }
    flush_entry(a, &cur, &have);

    {
        int i;
        for (i = 0; i < a->count; i++) a->entries[i].id = (ULONG)(i + 1);
    }
}

static void prepare_blocks(ARCHIVE* a)
{
    int i, k;
    BOOL have_blocks = TRUE;

    for (i = 0; i < a->count; i++)
        if (a->entries[i].block < 0) have_blocks = FALSE;

    if (!a->solid) { a->block_count = 1; have_blocks = FALSE; }
    if (a->block_count < 1) { a->block_count = 1; have_blocks = FALSE; }
    if (a->block_count > a->count) { a->block_count = 1; have_blocks = FALSE; }
    if (a->block_count > 1 && !have_blocks) a->block_count = 1;
    if (a->block_count <= 1)
        for (i = 0; i < a->count; i++) a->entries[i].block = 0;

    a->block_slots = a->block_count > 0 ? a->block_count : 1;
    a->block_bytes = (ULONGLONG*)calloc(a->block_slots, sizeof(ULONGLONG));
    a->block_pct = (volatile LONG*)calloc(a->block_slots, sizeof(LONG));
    a->total_bytes = 0;
    if (a->block_bytes && a->block_pct) {
        for (i = 0; i < a->count; i++) {
            k = a->entries[i].block;
            if (k < 0 || k >= a->block_slots) k = 0;
            a->block_bytes[k] += a->entries[i].size;
            a->total_bytes += a->entries[i].size;
        }
    } else {
        a->block_slots = 0;
    }
}

static BOOL list_archive(ARCHIVE* a)
{
    WCHAR cmd[32768];
    BYTE* out = NULL; DWORD size = 0; DWORD rc;
    if (!g_7za[0]) find_7za();
    _snwprintf(cmd, 32768, L"\"%s\" l -slt -sccUTF-8 -y \"%s\"", g_7za, a->archive_w);
    log_wide("list: ", cmd);
    rc = run_7za(cmd, &out, &size, TRUE, NULL);
    if (rc != 0 || !out) { free(out); mm_logf("list rc=%lu", rc); return FALSE; }
    parse_listing((const char*)out, a);
    free(out);
    prepare_blocks(a);
    mm_logf("listed %d entries solid=%d blocks=%d", a->count, a->solid, a->block_count);
    return a->count > 0;
}

/* ------------------------------------------------------------------ */
/* cache and list files                                                */
/* ------------------------------------------------------------------ */

static BOOL ensure_cache_dir(ARCHIVE* a)
{
    WCHAR temp[MAX_PATH], parent[MAX_PATH];
    DWORD attr;
    if (!GetTempPathW(MAX_PATH, temp)) return FALSE;
    _snwprintf(parent, MAX_PATH, L"%sMangaMeeya_7z", temp);
    CreateDirectoryW(parent, NULL);
    CreateDirectoryW(a->cache_w, NULL);
    attr = GetFileAttributesW(a->cache_w);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL cache_marker_exists(ARCHIVE* a)
{
    WCHAR marker[MAX_PATH * 2];
    _snwprintf(marker, MAX_PATH * 2, L"%s\\.done", a->cache_w);
    return GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}

static void write_cache_marker(ARCHIVE* a)
{
    WCHAR marker[MAX_PATH * 2];
    HANDLE h;
    _snwprintf(marker, MAX_PATH * 2, L"%s\\.done", a->cache_w);
    h = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_HIDDEN, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static BOOL make_list_path(ARCHIVE* a, WCHAR* out, int cap)
{
    WCHAR temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return FALSE;
    _snwprintf(out, cap, L"%sMangaMeeya_7z\\.mm7z_%hs_%lu_%ld.lst", temp,
               a->cache_hash, GetCurrentThreadId(),
               InterlockedIncrement(&g_list_seq));
    return TRUE;
}

static BOOL write_list_line(HANDLE h, const char* s, int len)
{
    DWORD wr = 0;
    if (len < 0) len = (int)strlen(s);
    if (!WriteFile(h, s, (DWORD)len, &wr, NULL) || wr != (DWORD)len) return FALSE;
    if (!WriteFile(h, "\r\n", 2, &wr, NULL) || wr != 2) return FALSE;
    return TRUE;
}

static BOOL write_block_list(ARCHIVE* a, int block, const WCHAR* path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    int i; BOOL ok = TRUE;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    for (i = 0; i < a->count; i++) {
        ENTRY* e = &a->entries[i];
        if (e->block != block || e->is_dir) continue;
        if (!e->name_utf8 || !write_list_line(h, e->name_utf8, -1)) { ok = FALSE; break; }
    }
    CloseHandle(h);
    if (!ok) DeleteFileW(path);
    return ok;
}

static BOOL write_entry_list(ENTRY* e, const WCHAR* path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = e->name_utf8 && write_list_line(h, e->name_utf8, -1);
    CloseHandle(h);
    if (!ok) DeleteFileW(path);
    return ok;
}

/* ------------------------------------------------------------------ */
/* extraction workers                                                  */
/* ------------------------------------------------------------------ */

static int worker_limit(void)
{
    SYSTEM_INFO si;
    if (g_threads > 0) return g_threads;
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors < 1) return 1;
    if (si.dwNumberOfProcessors > 8) return 8;
    return (int)si.dwNumberOfProcessors;
}

static void cancel_extract(ARCHIVE* a)
{
    int i;
    EnterCriticalSection(&a->cs);
    InterlockedExchange(&a->cancel, 1);
    for (i = 0; i < MAX_WORKERS; i++)
        if (a->procs[i]) TerminateProcess(a->procs[i], 1);
    LeaveCriticalSection(&a->cs);
}

static unsigned __stdcall worker_proc(void* param)
{
    WORKER* w = (WORKER*)param;
    ARCHIVE* a = w->a;
    int i;

    for (i = 0; i < w->nblocks; i++) {
        int b = w->blocks[i];
        WCHAR listpath[MAX_PATH * 2];
        WCHAR cmd[32768];
        RUNCANCEL rc;
        DWORD r;

        if (InterlockedCompareExchange(&a->cancel, 0, 0))
            break;

        listpath[0] = 0;
        if (a->block_count > 1) {
            if (!make_list_path(a, listpath, MAX_PATH * 2) ||
                !write_block_list(a, b, listpath)) {
                w->failed = TRUE;
                break;
            }
            _snwprintf(cmd, 32768,
                       L"\"%s\" x -y -bsp1 -sccUTF-8 -scsUTF-8 -o\"%s\" \"%s\" @\"%s\"",
                       g_7za, a->cache_w, a->archive_w, listpath);
        } else {
            _snwprintf(cmd, 32768,
                       L"\"%s\" x -y -bsp1 -sccUTF-8 -o\"%s\" \"%s\"",
                       g_7za, a->cache_w, a->archive_w);
        }

        InterlockedExchange(&a->block_pct[b], 0);
        ZeroMemory(&rc, sizeof(rc));
        rc.cancel = &a->cancel;
        rc.pct = &a->block_pct[b];
        rc.cs = &a->cs;
        rc.proc_slot = &a->procs[w->index];

        mm_logf("worker %d block %d start", w->index, b);
        r = run_7za(cmd, NULL, NULL, FALSE, &rc);
        if (listpath[0]) DeleteFileW(listpath);
        if (InterlockedCompareExchange(&a->cancel, 0, 0))
            break;
        if (r != 0) {
            mm_logf("worker %d block %d rc=%lu", w->index, b, r);
            w->failed = TRUE;
            break;
        }
        InterlockedExchange(&a->block_pct[b], 100);
        mm_logf("worker %d block %d done", w->index, b);
    }

    EnterCriticalSection(&a->cs);
    if (w->failed) {
        int j;
        a->worker_failed = TRUE;
        InterlockedExchange(&a->extract_failed, 1);
        InterlockedExchange(&a->cancel, 1);
        for (j = 0; j < MAX_WORKERS; j++)
            if (a->procs[j]) TerminateProcess(a->procs[j], 1);
    }
    a->workers_left--;
    if (a->workers_left == 0) {
        if (!a->worker_failed && !InterlockedCompareExchange(&a->cancel, 0, 0)) {
            write_cache_marker(a);
            a->cache_ready = TRUE;
            mm_logf("extraction complete");
        } else if (!a->worker_failed) {
            InterlockedExchange(&a->extract_cancelled, 1);
            mm_logf("extraction cancelled");
        }
        SetEvent(a->done_event);
    }
    LeaveCriticalSection(&a->cs);
    return 0;
}

static void free_workers(ARCHIVE* a)
{
    int i;
    if (!a->workers) return;
    for (i = 0; i < a->worker_count; i++) {
        if (a->workers[i].thread) CloseHandle(a->workers[i].thread);
        free(a->workers[i].blocks);
    }
    free(a->workers);
    a->workers = NULL;
    a->worker_count = 0;
}

/* Start the background extraction, or restart a cancelled/failed one.
 * Returns FALSE only when a new attempt could not be started at all. */
static BOOL ensure_extract_started(ARCHIVE* a)
{
    int i, k, threads, wcount, slots;

    EnterCriticalSection(&a->cs);

    if (a->cache_ready) { LeaveCriticalSection(&a->cs); return TRUE; }
    if (!a->block_bytes || !a->block_pct || a->block_slots <= 0) {
        InterlockedExchange(&a->extract_failed, 1);
        SetEvent(a->done_event);
        LeaveCriticalSection(&a->cs);
        return FALSE;
    }

    if (a->extract_started && a->workers_left > 0) {
        if (!InterlockedCompareExchange(&a->cancel, 0, 0)) {
            LeaveCriticalSection(&a->cs);   /* extraction in progress */
            return TRUE;
        }
        /* A previous call cancelled or failed; let the workers wind down so
         * this call can start a fresh attempt instead of failing at once. */
        LeaveCriticalSection(&a->cs);
        WaitForSingleObject(a->done_event, 15000);
        EnterCriticalSection(&a->cs);
        if (a->workers_left > 0) { LeaveCriticalSection(&a->cs); return TRUE; }
    }

    if (a->extract_started) {
        if (InterlockedCompareExchange(&a->extract_failed, 0, 0)) {
            /* hard failure: do not retry on every GetFile */
            LeaveCriticalSection(&a->cs);
            return TRUE;
        }
        /* previous attempt was cancelled - restart */
        free_workers(a);
        a->extract_started = FALSE;
        a->worker_failed = FALSE;
        InterlockedExchange(&a->extract_failed, 0);
        InterlockedExchange(&a->extract_cancelled, 0);
        InterlockedExchange(&a->cancel, 0);
        ResetEvent(a->done_event);
        for (i = 0; i < MAX_WORKERS; i++) a->procs[i] = NULL;
        for (k = 0; k < a->block_slots; k++) a->block_pct[k] = 0;
    }

    if (!ensure_cache_dir(a)) {
        InterlockedExchange(&a->extract_failed, 1);
        SetEvent(a->done_event);
        LeaveCriticalSection(&a->cs);
        return FALSE;
    }
    if (cache_marker_exists(a)) {
        a->cache_ready = TRUE;
        LeaveCriticalSection(&a->cs);
        return TRUE;
    }

    slots = a->block_slots > 0 ? a->block_slots : 1;
    threads = worker_limit();
    wcount = slots < threads ? slots : threads;
    if (wcount < 1) wcount = 1;
    if (wcount > MAX_WORKERS) wcount = MAX_WORKERS;

    a->workers = (WORKER*)calloc(wcount, sizeof(WORKER));
    if (!a->workers) {
        InterlockedExchange(&a->extract_failed, 1);
        SetEvent(a->done_event);
        LeaveCriticalSection(&a->cs);
        return FALSE;
    }
    a->worker_count = wcount;
    for (i = 0; i < MAX_WORKERS; i++) a->procs[i] = NULL;
    for (k = 0; k < a->block_slots; k++) a->block_pct[k] = 0;

    {
        int valid = 0;
        for (i = 0; i < wcount; i++) {
            WORKER* w = &a->workers[i];
            int n = 0, b, per = (slots + wcount - 1) / wcount + 1;
            w->a = a;
            w->index = i;
            w->blocks = (int*)malloc(sizeof(int) * per);
            if (!w->blocks) continue;
            for (b = i; b < slots; b += wcount) w->blocks[n++] = b;
            w->nblocks = n;
            valid++;
        }
        if (valid == 0) {
            a->worker_failed = TRUE;
            InterlockedExchange(&a->extract_failed, 1);
            SetEvent(a->done_event);
            LeaveCriticalSection(&a->cs);
            return FALSE;
        }
        a->workers_left = valid;
    }

    if (!a->done_event)
        a->done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->extract_started = TRUE;
    for (i = 0; i < wcount; i++) {
        WORKER* w = &a->workers[i];
        if (w->nblocks <= 0) continue;
        w->thread = (HANDLE)_beginthreadex(NULL, 0, worker_proc, w, 0, NULL);
        if (!w->thread) {
            w->failed = TRUE;
            a->worker_failed = TRUE;
            InterlockedExchange(&a->extract_failed, 1);
            a->workers_left--;
        }
    }
    if (a->workers_left == 0) {
        SetEvent(a->done_event);
        LeaveCriticalSection(&a->cs);
        return FALSE;
    }
    mm_logf("extract start: %d workers for %d block(s)", wcount, a->block_count);
    LeaveCriticalSection(&a->cs);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* reading cache entries                                               */
/* ------------------------------------------------------------------ */

enum { CACHE_NOT_READY, CACHE_OK, CACHE_ERROR };

static int read_cache_entry(ARCHIVE* a, ENTRY* e, BYTE** out, DWORD* out_size)
{
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    LARGE_INTEGER sz;
    BYTE* buf;
    DWORD got = 0;

    if (!build_cache_path(a, e->name_w, path, MAX_PATH * 2))
        return CACHE_ERROR;
    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return CACHE_NOT_READY;
    if (!GetFileSizeEx(h, &sz) || (ULONGLONG)sz.QuadPart != e->size) {
        CloseHandle(h);
        return CACHE_NOT_READY;
    }
    if (e->size > 0x3FFFFFFFULL) { /* 32-bit LocalAlloc */
        CloseHandle(h);
        return CACHE_ERROR;
    }
    buf = (BYTE*)LocalAlloc(LMEM_FIXED, (SIZE_T)(e->size ? e->size : 1));
    if (!buf) { CloseHandle(h); return CACHE_ERROR; }
    if (e->size &&
        (!ReadFile(h, buf, (DWORD)e->size, &got, NULL) || got != (DWORD)e->size)) {
        LocalFree(buf);
        CloseHandle(h);
        return CACHE_NOT_READY;
    }
    CloseHandle(h);
    if (e->crc && crc32_buf(buf, (DWORD)e->size) != e->crc) {
        LocalFree(buf);
        return CACHE_NOT_READY; /* extraction will report a CRC failure */
    }
    *out = buf;
    *out_size = (DWORD)e->size;
    return CACHE_OK;
}

static int archive_pct(ARCHIVE* a)
{
    int k;
    ULONGLONG done = 0;

    if (a->total_bytes == 0) {
        int fin = 0;
        for (k = 0; k < a->block_slots; k++)
            if (a->block_pct[k] >= 100) fin++;
        return a->block_slots ? fin * 100 / a->block_slots : 0;
    }
    for (k = 0; k < a->block_slots; k++) {
        LONG p = a->block_pct[k];
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        done += a->block_bytes[k] * (ULONGLONG)p / 100;
    }
    return (int)(done * 100 / a->total_bytes);
}

static int wait_for_entry(ARCHIVE* a, ENTRY* e, PROGRESS_CB cb, long lData,
                          BYTE** out, DWORD* out_size)
{
    DWORD last_cb;
    DWORD t0;

    /* Susie 00AM GetFile: the first call has nNum == 0, and a *non-zero*
     * return value aborts the operation (the image-filter API uses the
     * opposite convention). */
    if (cb && cb(0, 1, lData) != 0) {
        cancel_extract(a);
        return 1;
    }
    last_cb = t0 = GetTickCount();

    for (;;) {
        int r = read_cache_entry(a, e, out, out_size);
        if (r == CACHE_OK) {
            mm_logf("served id=%lu in %lu ms (pct=%d)", e->id,
                    GetTickCount() - t0, archive_pct(a));
            if (cb) cb(1, 1, lData);
            return 0;
        }
        if (r == CACHE_ERROR) return ERR_MEMORY;
        if (InterlockedCompareExchange(&a->extract_failed, 0, 0)) return ERR_BROKEN;
        if (InterlockedCompareExchange(&a->extract_cancelled, 0, 0)) return 1;

        if (WaitForSingleObject(a->done_event, 0) == WAIT_OBJECT_0) {
            /* extraction finished successfully but this entry is absent/bad */
            r = read_cache_entry(a, e, out, out_size);
            if (r == CACHE_OK) { if (cb) cb(1, 1, lData); return 0; }
            return ERR_BROKEN;
        }
        if (cb) {
            DWORD now = GetTickCount();
            if (now - last_cb >= 80) {
                last_cb = now;
                if (cb(archive_pct(a), 100, lData) != 0) {
                    cancel_extract(a);
                    return 1;
                }
            }
        }
        Sleep(20);
    }
}

/* Non-solid archives: extract one entry to memory through stdout. */
static BYTE* extract_direct(ARCHIVE* a, ENTRY* e, DWORD* out_size, int* err)
{
    WCHAR listpath[MAX_PATH * 2];
    WCHAR cmd[32768];
    BYTE* out = NULL;
    DWORD size = 0, rc;

    *err = ERR_BROKEN;
    if (!ensure_cache_dir(a)) { *err = ERR_MEMORY; return NULL; }
    if (!make_list_path(a, listpath, MAX_PATH * 2) || !write_entry_list(e, listpath))
        return NULL;
    _snwprintf(cmd, 32768, L"\"%s\" x -so -y -scsUTF-8 \"%s\" @\"%s\"",
               g_7za, a->archive_w, listpath);
    rc = run_7za(cmd, &out, &size, TRUE, NULL);
    DeleteFileW(listpath);
    if (rc != 0 || !out) {
        free(out);
        mm_logf("extract-one failed rc=%lu", rc);
        return NULL;
    }
    {
        BYTE* lb = (BYTE*)LocalAlloc(LMEM_FIXED, size ? size : 1);
        if (!lb) { free(out); *err = ERR_MEMORY; return NULL; }
        memcpy(lb, out, size);
        free(out);
        *out_size = size;
        *err = 0;
        return lb;
    }
}

/* ------------------------------------------------------------------ */
/* archive context                                                     */
/* ------------------------------------------------------------------ */

/* Hash enough of a memory image to notice buffer reuse without reading
 * the whole (possibly large) archive on every GetFile call. */
static ULONGLONG mem_sample_hash(const BYTE* p, long n)
{
    ULONGLONG h = 1469598103934665603ULL;
    long head = n < 65536 ? n : 65536;
    long tail = n < 65536 ? 0 : 65536;
    h ^= (ULONGLONG)n; h *= 1099511628211ULL;
    h = fnv1a_bytes(p, (size_t)head, h);
    if (tail) h = fnv1a_bytes(p + n - tail, (size_t)tail, h);
    return h;
}

static ARCHIVE* get_archive(LPCSTR src, long len, UINT flag, BOOL mem_size_valid, BOOL* err)
{
    char key[MAX_PATH * 2];
    ARCHIVE* a;
    WCHAR wpath[MAX_PATH * 2];
    BOOL mem = (flag & 7) == 1;

    *err = FALSE;
    if (mem) {
        if (!src) { *err = TRUE; return NULL; }
        _snprintf(key, sizeof(key), "M:%p", (void*)src);
    } else {
        if (!resolve_ansi_path(src, wpath, MAX_PATH * 2)) {
            if (!MultiByteToWideChar(CP_ACP, 0, src ? src : "", -1, wpath, MAX_PATH * 2)) {
                *err = TRUE; return NULL;
            }
        }
        _snprintf(key, sizeof(key), "P:%016I64x", fnv1a_wstr(wpath));
    }

    for (a = g_archives; a; a = a->next)
        if (!strcmp(a->key, key)) return a;

    a = (ARCHIVE*)calloc(1, sizeof(ARCHIVE));
    if (!a) { *err = TRUE; return NULL; }
    lstrcpynA(a->key, key, sizeof(a->key));
    InitializeCriticalSection(&a->cs);
    a->done_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (mem) {
        WCHAR temp[MAX_PATH], tpath[MAX_PATH];
        char hash[24];
        ULONGLONG mh;
        HANDLE h;
        DWORD wr = 0;
        if (!mem_size_valid || len <= 0) { *err = TRUE; return a; }
        mh = mem_sample_hash((const BYTE*)src, len);
        _snprintf(hash, sizeof(hash), "%016I64x", mh);
        if (!GetTempPathW(MAX_PATH, temp)) { *err = TRUE; return a; }
        _snwprintf(tpath, MAX_PATH, L"%sMangaMeeya_7z", temp);
        CreateDirectoryW(tpath, NULL);
        _snwprintf(tpath, MAX_PATH, L"%sMangaMeeya_7z\\in_%hs.7z", temp, hash);
        h = CreateFileW(tpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { *err = TRUE; return a; }
        WriteFile(h, src, (DWORD)len, &wr, NULL);
        CloseHandle(h);
        wcscpy(a->archive_w, tpath);
        lstrcpynA(a->cache_hash, hash, sizeof(a->cache_hash));
    } else {
        wcscpy(a->archive_w, wpath);
        hash_path_cache(a->archive_w, a->cache_hash);
    }
    {
        WCHAR temp[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, temp)) { *err = TRUE; return a; }
        _snwprintf(a->cache_w, MAX_PATH * 2, L"%sMangaMeeya_7z\\%hs", temp, a->cache_hash);
    }

    a->next = g_archives;
    g_archives = a;
    if (!list_archive(a)) { *err = TRUE; }
    return a;
}

static ENTRY* find_entry(ARCHIVE* a, ULONG id)
{
    int i;
    for (i = 0; i < a->count; i++)
        if (a->entries[i].id == id) return &a->entries[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Susie 00AM exports                                                  */
/* ------------------------------------------------------------------ */

__declspec(dllexport) int WINAPI GetPluginInfo(int infono, LPSTR buf, int buflen)
{
    switch (infono) {
    case 0: return copy_info("00AM", buf, buflen);
    case 1: return copy_info("7z Archive Plug-in v1.1 (parallel solid blocks)", buf, buflen);
    case 2: return copy_info("*.7Z", buf, buflen);
    case 3: return copy_info("7z Archive", buf, buflen);
    default: return 0;
    }
}

__declspec(dllexport) int WINAPI IsSupported(LPSTR filename, DWORD dw)
{
    BYTE hdr[8]; DWORD got = 0;
    if (HIWORD(dw)) {
        memcpy(hdr, (BYTE*)dw, 6);
        return SEVENZ_SIG_OK(hdr);
    } else {
        HANDLE h = (HANDLE)dw; DWORD pos;
        if (h == 0 || h == INVALID_HANDLE_VALUE) return 0;
        pos = SetFilePointer(h, 0, NULL, FILE_CURRENT);
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        if (!ReadFile(h, hdr, 6, &got, NULL) || got < 6) {
            SetFilePointer(h, pos, NULL, FILE_BEGIN); return 0;
        }
        SetFilePointer(h, pos, NULL, FILE_BEGIN);
        (void)filename;
        return SEVENZ_SIG_OK(hdr);
    }
}

__declspec(dllexport) int WINAPI GetArchiveInfo(LPSTR buf, long len, UINT flag, HLOCAL* lphInf)
{
    ARCHIVE* a; BOOL err; FILEINFO* fi; HLOCAL h; int i;
    if (!lphInf) return 8;
    *lphInf = NULL;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = TRUE; }
    EnterCriticalSection(&g_lock);
    a = get_archive(buf, len, flag, TRUE, &err);
    if (!a || err || a->count <= 0) {
        LeaveCriticalSection(&g_lock);
        return ERR_UNKNOWN;
    }
    h = LocalAlloc(LMEM_FIXED, sizeof(FILEINFO) * (a->count + 1));
    if (!h) { LeaveCriticalSection(&g_lock); return ERR_MEMORY; }
    fi = (FILEINFO*)h;
    ZeroMemory(fi, sizeof(FILEINFO) * (a->count + 1));
    for (i = 0; i < a->count; i++) {
        ENTRY* e = &a->entries[i];
        char* name = e->name_utf8 ? e->name_utf8 : (char*)"";
        char* slash1 = strrchr(name, '/');
        char* slash2 = strrchr(name, '\\');
        char* slash = slash1 > slash2 ? slash1 : slash2;
        int plen = slash ? (int)(slash - name) : 0;
        const char* leaf = slash ? slash + 1 : name;
        if (plen > 199) plen = 199;
        {
            const char* mm = e->method[0] ? e->method : "7z";
            int ml = (int)strlen(mm); if (ml > 7) ml = 7;
            memcpy(fi[i].method, mm, ml);
        }
        fi[i].position = e->id;
        fi[i].compsize = (unsigned long)e->packsize;
        fi[i].filesize = (unsigned long)e->size;
        fi[i].timestamp = e->mtime;
        if (plen > 0) { memcpy(fi[i].path, name, plen); fi[i].path[plen] = 0; }
        lstrcpynA(fi[i].filename, leaf, 200);
        fi[i].crc = e->crc;
    }
    *lphInf = h;
    LeaveCriticalSection(&g_lock);
    return 0;
}

__declspec(dllexport) int WINAPI GetFileInfo(LPSTR buf, long len, LPSTR filename, UINT flag, FILEINFO* info)
{
    ARCHIVE* a; BOOL err; int i;
    if (!info) return 8;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = TRUE; }
    if (!filename) return ERR_UNKNOWN;
    EnterCriticalSection(&g_lock);
    a = get_archive(buf, len, flag, TRUE, &err);
    if (!a || err) { LeaveCriticalSection(&g_lock); return ERR_UNKNOWN; }
    for (i = 0; i < a->count; i++) {
        ENTRY* e = &a->entries[i];
        if (e->name_utf8 && name_eq(e->name_utf8, filename)) {
            ZeroMemory(info, sizeof(*info));
            {
                const char* mm = e->method[0] ? e->method : "7z";
                int ml = (int)strlen(mm); if (ml > 7) ml = 7;
                memcpy(info->method, mm, ml);
            }
            info->position = e->id;
            info->compsize = (unsigned long)e->packsize;
            info->filesize = (unsigned long)e->size;
            info->timestamp = e->mtime;
            lstrcpynA(info->filename, filename, 200);
            info->crc = e->crc;
            LeaveCriticalSection(&g_lock);
            return 0;
        }
    }
    LeaveCriticalSection(&g_lock);
    return ERR_UNKNOWN;
}

__declspec(dllexport) int WINAPI GetFile(LPSTR src, long len, LPSTR dest, UINT flag,
                                         FARPROC cb, long lData)
{
    ARCHIVE* a; BOOL err; ENTRY* e;
    PROGRESS_CB pcb = (PROGRESS_CB)cb;
    BYTE* data = NULL;
    DWORD size = 0;
    int rc = 0;

    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = TRUE; }
    EnterCriticalSection(&g_lock);
    a = get_archive(src, len, flag, FALSE, &err);
    LeaveCriticalSection(&g_lock);
    if (!a || err) { mm_logf("GetFile: no archive"); return ERR_UNKNOWN; }

    e = find_entry(a, (ULONG)len);
    if (!e) { mm_logf("GetFile: no entry id=%ld", len); return ERR_UNKNOWN; }
    if (!e->name_w) return ERR_BROKEN;
    mm_logf("GetFile id=%ld flag=%08x dest=%p name=%s", len, flag, (void*)dest,
            e->name_utf8 ? e->name_utf8 : "?");

    if (a->solid) {
        if (!ensure_extract_started(a)) return ERR_MEMORY;
        rc = wait_for_entry(a, e, pcb, lData, &data, &size);
        if (rc) return rc;
    } else {
        if (pcb && pcb(0, 1, lData) != 0) return 1;
        data = extract_direct(a, e, &size, &rc);
        if (!data) return rc;
        if (pcb) pcb(1, 1, lData);
    }

    if ((flag & 0x700) == 0x100) {
        if (!dest) { LocalFree(data); return 8; }
        *(HLOCAL*)dest = (HLOCAL)data;
        return 0;
    }

    if (!dest) { LocalFree(data); return 8; }
    {
        WCHAR wdest[MAX_PATH * 2], wpath[MAX_PATH * 2];
        const WCHAR* leaf = e->name_w;
        const WCHAR* q;
        HANDLE h;
        DWORD wr = 0;
        for (q = e->name_w; *q; q++)
            if (*q == L'\\' || *q == L'/') leaf = q + 1;
        if (!MultiByteToWideChar(CP_ACP, 0, dest, -1, wdest, MAX_PATH * 2)) {
            LocalFree(data);
            return 6;
        }
        _snwprintf(wpath, MAX_PATH * 2, L"%s\\%s", wdest, leaf);
        h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            mm_logf("GetFile disk-out create failed err=%lu", GetLastError());
            LocalFree(data);
            return 6;
        }
        if (size) WriteFile(h, data, size, &wr, NULL);
        CloseHandle(h);
        mm_logf("GetFile disk-out wrote %lu bytes", wr);
        LocalFree(data);
    }
    return 0;
}

BOOL WINAPI DllMain(HMODULE hm, DWORD reason, LPVOID p)
{
    (void)p;
    if (reason == DLL_PROCESS_ATTACH) {
        char v[16];
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        g_self = hm;
        InitializeCriticalSection(&g_lock);
        g_lock_init = TRUE;
        DisableThreadLibraryCalls(hm);
        crc32_init();
        if (GetEnvironmentVariableA("MM7Z_LOG", v, sizeof(v)) > 0 && v[0] == '1')
            g_log = TRUE;
        if (GetEnvironmentVariableA("MM7Z_THREADS", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n < 1) n = 1;
            if (n > MAX_WORKERS) n = MAX_WORKERS;
            g_threads = n;
        }
        g_job = CreateJobObjectW(NULL, NULL);
        if (g_job) {
            ZeroMemory(&li, sizeof(li));
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation,
                                         &li, sizeof(li))) {
                CloseHandle(g_job);
                g_job = NULL;
            }
        }
        find_7za();
        log_wide("--- 7z attached, 7za=", g_7za);
    }
    return TRUE;
}
