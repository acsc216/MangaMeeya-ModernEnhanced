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
 * soon as it has been produced.  A single solid block is decoded on demand
 * through one `7za x -so` stream: decoding pauses at "requested page +
 * read-ahead window" instead of extracting the whole archive up front.
 * Multi-block archives still use one process per block (parallel eager
 * extraction, which is cheap because the blocks are independent).
 *
 * MangaMeeya passes fileInfo.position back as the "len" argument of
 * GetFile, so position is used as the entry identifier.
 *
 * Build (32-bit):
 *   cl /nologo /LD /MT /O2 /W0 7z.c /link /DEF:7z.def user32.lib crypt32.lib /OUT:7z.spi
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
#include <wincrypt.h>

#define SEVENZ_SIG_OK(p) ((p)[0]==0x37 && (p)[1]==0x7A && (p)[2]==0xBC && \
                          (p)[3]==0xAF && (p)[4]==0x27 && (p)[5]==0x1C)

#define MAX_WORKERS     16
#define ERR_UNKNOWN      2
#define ERR_BROKEN       3
#define ERR_MEMORY       4

#define STREAM_CHUNK    (256 * 1024)
#define STREAM_PREFETCH (64ULL * 1024 * 1024)

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
    int    encrypted;
    int    block;          /* solid block index, -1 if unknown */
    ULONGLONG offset;      /* start offset in the archive's unpacked stream */
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
    BOOL        is_mem;
    BOOL        solid;
    BOOL        encrypted;        /* at least one entry or the header is encrypted */
    BOOL        has_password;
    BOOL        remember_choice;  /* dialog checkbox */
    WCHAR       password[512];
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

    /* Single solid block: decode through one `7za x -so` stream on demand. */
    BOOL        stream_mode;
    BOOL        memcache;         /* keep decoded pages in the OS cache only */
    HANDLE*     pins;             /* DELETE_ON_CLOSE handles that keep them */
    BOOL        stream_started;
    HANDLE      stream_thread;
    HANDLE      stream_wake;      /* auto-reset: frontier raised / cancel */
    volatile LONG stream_mb;      /* decoded position, MiB */
    ULONGLONG   want_until;       /* decode at least up to this stream offset */

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
static BOOL g_memcache;
static BOOL g_remember_password = TRUE;
static WCHAR g_env_password[512];
static BOOL g_has_env_password;
static CRITICAL_SECTION g_logcs;
static BOOL g_logcs_init;
static int  g_threads;
static ULONGLONG g_prefetch = STREAM_PREFETCH;
static ULONGLONG g_cache_max = 5120ULL * 1024 * 1024;   /* 5 GB */
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
    if (g_logcs_init) EnterCriticalSection(&g_logcs);
    f = fopen(path, "a");
    if (f) { fprintf(f, "[%lu] %s\r\n", GetTickCount(), buf); fclose(f); }
    if (g_logcs_init) LeaveCriticalSection(&g_logcs);
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
/* archive passwords                                                   */
/* ------------------------------------------------------------------ */

static const char g_pw_entropy[] = "MangaMeeya_7z_password";

/* Build " -p\"<password>\"" (quotes inside the password are escaped). */
static int password_switch(WCHAR* out, int cap, const WCHAR* pwd)
{
    int n = 0;
    if (!pwd || !pwd[0] || cap < 8) return 0;
    out[n++] = L' '; out[n++] = L'-'; out[n++] = L'p'; out[n++] = L'"';
    for (; *pwd && n < cap - 3; pwd++) {
        if (*pwd == L'"') out[n++] = L'\\';
        out[n++] = *pwd;
    }
    out[n++] = L'"';
    out[n] = 0;
    return n;
}

typedef struct {
    const WCHAR* text;
    WCHAR* value;
    int cap;
    BOOL remember;
} PWDCTX;

/* Per-user checkbox preference (%APPDATA%\MangaMeeya_7z\remember). */
static BOOL remember_pref_path(WCHAR* out, int cap)
{
    WCHAR appdata[MAX_PATH], dir[MAX_PATH * 2];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return FALSE;
    _snwprintf(dir, MAX_PATH * 2, L"%s\\MangaMeeya_7z", appdata);
    CreateDirectoryW(dir, NULL);
    _snwprintf(out, cap, L"%s\\remember", dir);
    return TRUE;
}

static void save_remember_pref(BOOL on)
{
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD wr = 0;
    if (!remember_pref_path(path, MAX_PATH * 2)) return;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_HIDDEN, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, on ? "1" : "0", 1, &wr, NULL);
        CloseHandle(h);
    }
}

static void load_remember_pref(void)
{
    WCHAR path[MAX_PATH * 2];
    char c = 0;
    HANDLE h;
    DWORD got = 0;
    if (!remember_pref_path(path, MAX_PATH * 2)) return;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, &c, 1, &got, NULL) || got != 1) c = 0;
    CloseHandle(h);
    if (c == '0' || c == '1') g_remember_password = (c == '1');
}

static BYTE* dlg_align(BYTE* p)
{
    return (BYTE*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
}

static BYTE* dlg_wstr(BYTE* p, const WCHAR* s)
{
    while (*s) { *(WCHAR*)p = *s++; p += 2; }
    *(WCHAR*)p = 0;
    return p + 2;
}

static BYTE* dlg_item(BYTE* p, DWORD style, WORD x, WORD y, WORD cx, WORD cy,
                      WORD id, WORD cls, const WCHAR* text)
{
    DLGITEMTEMPLATE* it;
    p = dlg_align(p);
    it = (DLGITEMTEMPLATE*)p;
    it->style = style;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy;
    it->id = id;
    p += sizeof(DLGITEMTEMPLATE);
    *(WORD*)p = 0xFFFF; p += 2;
    *(WORD*)p = cls; p += 2;
    p = dlg_wstr(p, text);
    *(WORD*)p = 0; p += 2;
    return p;
}

static INT_PTR CALLBACK pwd_dlg_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PWDCTX* ctx = (PWDCTX*)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        ctx = (PWDCTX*)lParam;
        SetWindowLongPtrW(hDlg, DWLP_USER, lParam);
        if (ctx->text) SetDlgItemTextW(hDlg, 1002, ctx->text);
        SendDlgItemMessageW(hDlg, 1001, EM_SETLIMITTEXT, (WPARAM)(ctx->cap - 1), 0);
        if (ctx->value && ctx->value[0]) SetDlgItemTextW(hDlg, 1001, ctx->value);
        CheckDlgButton(hDlg, 1003, ctx->remember ? BST_CHECKED : BST_UNCHECKED);
        {
            HWND owner = GetParent(hDlg);
            RECT r, pr;
            if (owner && GetWindowRect(hDlg, &r) && GetWindowRect(owner, &pr)) {
                int w = r.right - r.left, h = r.bottom - r.top;
                SetWindowPos(hDlg, NULL,
                             pr.left + (pr.right - pr.left - w) / 2,
                             pr.top + (pr.bottom - pr.top - h) / 2,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
        }
        SetFocus(GetDlgItem(hDlg, 1001));
        return FALSE;

    case WM_COMMAND:
        if (!ctx) break;
        switch (LOWORD(wParam)) {
        case IDOK:
            GetDlgItemTextW(hDlg, 1001, ctx->value, ctx->cap);
            ctx->remember = (IsDlgButtonChecked(hDlg, 1003) == BST_CHECKED);
            EndDialog(hDlg, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, 0);
        return TRUE;
    }
    return FALSE;
}

static BOOL show_password_dialog(HWND parent, const WCHAR* text, WCHAR* value,
                                 int cap, BOOL* remember)
{
    enum { IDC_EDIT = 1001, IDC_TEXT = 1002, IDC_CHECK = 1003 };
    BYTE buf[2048];
    BYTE* p;
    DLGTEMPLATE* dlg;
    PWDCTX ctx;
    INT_PTR r;

    ZeroMemory(buf, sizeof(buf));
    dlg = (DLGTEMPLATE*)buf;
    dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    dlg->dwExtendedStyle = WS_EX_DLGMODALFRAME;
    dlg->cdit = 5;
    dlg->x = 10; dlg->y = 10; dlg->cx = 220; dlg->cy = 92;
    p = (BYTE*)(dlg + 1);
    *(WORD*)p = 0; p += 2;      /* menu */
    *(WORD*)p = 0; p += 2;      /* dialog class */
    p = dlg_wstr(p, L"7z Password");

    p = dlg_item(p, WS_CHILD | WS_VISIBLE | SS_LEFT,
                 7, 7, 206, 18, IDC_TEXT, 0x0082, L"");
    p = dlg_item(p, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                    ES_PASSWORD | ES_AUTOHSCROLL,
                 7, 27, 206, 14, IDC_EDIT, 0x0081, L"");
    p = dlg_item(p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                 7, 46, 160, 12, IDC_CHECK, 0x0080,
                 L"\x8BB0\x4F4F\x5BC6\x7801 / Remember");
    p = dlg_item(p, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                 92, 68, 50, 14, IDOK, 0x0080, L"OK");
    p = dlg_item(p, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                 148, 68, 50, 14, IDCANCEL, 0x0080, L"Cancel");

    ctx.text = text;
    ctx.value = value;
    ctx.cap = cap;
    ctx.remember = remember ? *remember : TRUE;
    if (!parent) parent = GetActiveWindow();
    if (!parent) parent = GetForegroundWindow();
    r = DialogBoxIndirectParamW(g_self, (LPCDLGTEMPLATEW)buf, parent,
                                pwd_dlg_proc, (LPARAM)&ctx);
    if (r == 1 && remember) *remember = ctx.remember;
    return r == 1;
}

/* Prompt for a password.  MM7Z_PASSWORD, when set, bypasses the dialog
 * (used for automation/tests). */
static BOOL ask_password(ARCHIVE* a, BOOL again)
{
    WCHAR buf[512];
    HWND parent;
    BOOL remember;

    if (g_has_env_password) {
        wcsncpy(a->password, g_env_password, 511);
        a->password[511] = 0;
        a->has_password = TRUE;
        a->remember_choice = g_remember_password;
        return TRUE;
    }

    parent = GetActiveWindow();
    if (!parent) parent = GetForegroundWindow();
    if (again)
        MessageBoxW(parent,
                    L"\x5BC6\x7801\x9519\x8BEF\xFF0C\x8BF7\x91CD\x8BD5\x3002",
                    L"7z", MB_OK | MB_ICONWARNING);

    buf[0] = 0;
    remember = a->remember_choice;
    if (!show_password_dialog(parent,
            L"\x538B\x7F29\x5305\x5DF2\x52A0\x5BC6\xFF0C\x8BF7\x8F93\x5165\x5BC6\x7801\xFF1A",
            buf, 512, &remember))
        return FALSE;
    a->remember_choice = remember;
    g_remember_password = remember; /* applies to later archives too */
    save_remember_pref(remember);   /* remember the checkbox choice */
    wcsncpy(a->password, buf, 511);
    a->password[511] = 0;
    SecureZeroMemory(buf, sizeof(buf));
    a->has_password = TRUE;
    return TRUE;
}

static BOOL password_file_path(const WCHAR* archive, WCHAR* out, int cap)
{
    WCHAR appdata[MAX_PATH], dir[MAX_PATH * 2], hash[24];

    if (!archive || !archive[0]) return FALSE;
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return FALSE;
    _snwprintf(dir, MAX_PATH * 2, L"%s\\MangaMeeya_7z", appdata);
    CreateDirectoryW(dir, NULL);
    _snwprintf(dir, MAX_PATH * 2, L"%s\\MangaMeeya_7z\\passwords", appdata);
    CreateDirectoryW(dir, NULL);
    _snwprintf(hash, 24, L"%016I64x", fnv1a_wstr(archive));
    _snwprintf(out, cap, L"%s\\%s.pw", dir, hash);
    return TRUE;
}

static void delete_password(const WCHAR* archive)
{
    WCHAR path[MAX_PATH * 2];
    if (password_file_path(archive, path, MAX_PATH * 2))
        DeleteFileW(path);
}

/* Persist with DPAPI (current-user scope), never plaintext. */
static void save_password(const WCHAR* archive, const WCHAR* pwd, BOOL remember)
{
    DATA_BLOB in, out, entropy;
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD wr = 0;

    if (!remember) { delete_password(archive); return; }
    if (!pwd || !pwd[0]) return;
    if (!password_file_path(archive, path, MAX_PATH * 2)) return;

    in.pbData = (BYTE*)pwd;
    in.cbData = (DWORD)((wcslen(pwd) + 1) * sizeof(WCHAR));
    entropy.pbData = (BYTE*)g_pw_entropy;
    entropy.cbData = (DWORD)(sizeof(g_pw_entropy) - 1);
    out.pbData = NULL;
    out.cbData = 0;
    if (!CryptProtectData(&in, L"7z password", &entropy, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        mm_logf("password: CryptProtectData failed err=%lu", GetLastError());
        return;
    }
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_HIDDEN, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, out.pbData, out.cbData, &wr, NULL);
        CloseHandle(h);
    }
    LocalFree(out.pbData);
}

static BOOL load_password(const WCHAR* archive, WCHAR* out, int cap)
{
    DATA_BLOB in, decoded, entropy;
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD got = 0, size;

    if (!g_remember_password || !out || cap <= 0) return FALSE;
    out[0] = 0;
    if (!password_file_path(archive, path, MAX_PATH * 2)) return FALSE;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > (1u << 20)) {
        CloseHandle(h);
        return FALSE;
    }
    in.pbData = (BYTE*)malloc(size);
    if (!in.pbData) { CloseHandle(h); return FALSE; }
    if (!ReadFile(h, in.pbData, size, &got, NULL) || got != size) {
        free(in.pbData);
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);
    in.cbData = size;
    entropy.pbData = (BYTE*)g_pw_entropy;
    entropy.cbData = (DWORD)(sizeof(g_pw_entropy) - 1);
    decoded.pbData = NULL;
    decoded.cbData = 0;
    if (!CryptUnprotectData(&in, NULL, &entropy, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &decoded)) {
        free(in.pbData);
        return FALSE;
    }
    free(in.pbData);
    if (decoded.cbData >= sizeof(WCHAR) && (decoded.cbData % sizeof(WCHAR)) == 0) {
        int n = (int)(decoded.cbData / sizeof(WCHAR));
        if (n > cap - 1) n = cap - 1;
        memcpy(out, decoded.pbData, n * sizeof(WCHAR));
        out[n] = 0;
    }
    LocalFree(decoded.pbData);
    return out[0] != 0;
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

/* Parse a hex value within one listing line.  _strtoul would skip the
 * newline and consume digits from the next field (an empty "CRC =" line is
 * followed by "Encrypted = -", whose 'E' parsed as 0x0E). */
static DWORD parse_hex_n(const char* p, int len)
{
    DWORD v = 0;
    int i;
    for (i = 0; i < len; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else break;
        v = (v << 4) | (DWORD)d;
    }
    return v;
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
            cur.crc = parse_hex_n(p + 6, ll - 6);
        } else if (KEYIS("Encrypted =")) {
            if (p[12] == '+') { cur.encrypted = 1; a->encrypted = TRUE; }
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
    ULONGLONG off = 0;

    for (i = 0; i < a->count; i++) {
        a->entries[i].offset = off;
        off += a->entries[i].size;
    }

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

    /* One solid block cannot be decoded in parallel and may be very large;
     * decode it on demand instead of extracting the whole archive. */
    a->stream_mode = a->solid && a->block_count <= 1 && a->count > 0;

    /* Optional session-only memory cache: decoded pages live in the Windows
     * cache manager (temporary + delete-on-close files).  The data never
     * occupies this 32-bit process's address space and is deleted when the
     * process exits; it is not reused across sessions. */
    if (g_memcache && a->stream_mode && a->count > 0 && a->count <= 20000) {
        a->pins = (HANDLE*)calloc(a->count, sizeof(HANDLE));
        if (a->pins) a->memcache = TRUE;
    }
}

/* 0 = ok, 1 = password required/wrong, -1 = other failure */
static int list_archive(ARCHIVE* a)
{
    WCHAR cmd[32768], pwd[600];
    BYTE* out = NULL; DWORD size = 0; DWORD rc;
    int r;

    if (!g_7za[0]) find_7za();
    pwd[0] = 0;
    if (a->has_password) password_switch(pwd, 600, a->password);
    _snwprintf(cmd, 32768, L"\"%s\" l -slt -sccUTF-8 -y%s \"%s\"",
               g_7za, pwd, a->archive_w);
    log_wide("list: ", cmd);
    rc = run_7za(cmd, &out, &size, TRUE, NULL);
    if (rc != 0 || !out) {
        r = (out && (strstr((const char*)out, "Enter password") ||
                     strstr((const char*)out, "Wrong password") ||
                     strstr((const char*)out, "Cannot open encrypted archive")))
            ? 1 : -1;
        mm_logf("list rc=%lu r=%d", rc, r);
        free(out);
        return r;
    }
    free_entries(a);
    a->solid = FALSE;
    a->block_count = 0;
    a->encrypted = FALSE;
    parse_listing((const char*)out, a);
    free(out);
    prepare_blocks(a);
    mm_logf("listed %d entries solid=%d blocks=%d enc=%d",
            a->count, a->solid, a->block_count, a->encrypted);
    return a->count > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* cache and list files                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* cache size management (default cap: 5 GB)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    WCHAR path[MAX_PATH * 2];
    ULONGLONG size;
    FILETIME last;
} CACHEITEM;

static ULONGLONG path_size(const WCHAR* path)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    WCHAR pat[MAX_PATH * 2];
    ULONGLONG total = 0;

    _snwprintf(pat, MAX_PATH * 2, L"%s\\*", path);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
            return ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        return 0;
    }
    do {
        WCHAR child[MAX_PATH * 2];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        _snwprintf(child, MAX_PATH * 2, L"%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += path_size(child);
        else
            total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static void delete_tree(const WCHAR* path)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    WCHAR pat[MAX_PATH * 2];
    DWORD attr = GetFileAttributesW(path);

    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { DeleteFileW(path); return; }

    _snwprintf(pat, MAX_PATH * 2, L"%s\\*", path);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            WCHAR child[MAX_PATH * 2];
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf(child, MAX_PATH * 2, L"%s\\%s", path, fd.cFileName);
            delete_tree(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

static int cacheitem_cmp(const void* va, const void* vb)
{
    const CACHEITEM* a = (const CACHEITEM*)va;
    const CACHEITEM* b = (const CACHEITEM*)vb;
    if (a->last.dwHighDateTime != b->last.dwHighDateTime)
        return a->last.dwHighDateTime < b->last.dwHighDateTime ? -1 : 1;
    if (a->last.dwLowDateTime != b->last.dwLowDateTime)
        return a->last.dwLowDateTime < b->last.dwLowDateTime ? -1 : 1;
    return 0;
}

/* Keep the total cache under g_cache_max.  keep1 is the active archive's
 * cache directory (never deleted); keep2 is its input file (only relevant
 * for memory-image temp copies, which also live under the cache root).
 * Entries touched in the last 10 minutes are left alone so a concurrently
 * running MangaMeeya instance is not disturbed. */
static void cache_cleanup(const WCHAR* keep1, const WCHAR* keep2)
{
    WCHAR temp[MAX_PATH], root[MAX_PATH * 2], pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    CACHEITEM* items = NULL;
    int n = 0, cap = 0, i;
    ULONGLONG total = 0;
    FILETIME now;
    ULARGE_INTEGER nowu;

    if (!g_cache_max) return;
    if (!GetTempPathW(MAX_PATH, temp)) return;
    _snwprintf(root, MAX_PATH * 2, L"%sMangaMeeya_7z", temp);
    _snwprintf(pat, MAX_PATH * 2, L"%s\\*", root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    GetSystemTimeAsFileTime(&now);
    nowu.LowPart = now.dwLowDateTime;
    nowu.HighPart = now.dwHighDateTime;

    do {
        WCHAR path[MAX_PATH * 2];
        ULARGE_INTEGER lu;
        ULONGLONG size;

        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        _snwprintf(path, MAX_PATH * 2, L"%s\\%s", root, fd.cFileName);

        if ((keep1 && !_wcsicmp(path, keep1)) || (keep2 && !_wcsicmp(path, keep2))) {
            total += path_size(path);
            continue;
        }

        lu.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        lu.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (nowu.QuadPart - lu.QuadPart < 10ULL * 60 * 10000000) {
            total += path_size(path);
            continue;
        }

        size = path_size(path);
        total += size;
        if (n == cap) {
            int ncap = cap ? cap * 2 : 32;
            CACHEITEM* ni = (CACHEITEM*)realloc(items, sizeof(CACHEITEM) * ncap);
            if (!ni) break;
            items = ni;
            cap = ncap;
        }
        items[n].size = size;
        items[n].last = fd.ftLastWriteTime;
        wcsncpy(items[n].path, path, MAX_PATH * 2 - 1);
        items[n].path[MAX_PATH * 2 - 1] = 0;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (n > 0 && total > g_cache_max) {
        qsort(items, n, sizeof(CACHEITEM), cacheitem_cmp);
        mm_logf("cache cleanup: %d old item(s), total %I64u, cap %I64u",
                n, total, g_cache_max);
        for (i = 0; i < n && total > g_cache_max; i++) {
            ULONGLONG sz = items[i].size;
            delete_tree(items[i].path);
            if (GetFileAttributesW(items[i].path) == INVALID_FILE_ATTRIBUTES) {
                total -= sz;
                mm_logf("cache cleanup: removed %I64u bytes", sz);
            } else {
                mm_logf("cache cleanup: item in use, skipped");
            }
        }
        mm_logf("cache cleanup: total now %I64u", total);
    }
    free(items);
}

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

static BOOL write_all_list(ARCHIVE* a, const WCHAR* path)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    int i; BOOL ok = TRUE;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    for (i = 0; i < a->count; i++) {
        ENTRY* e = &a->entries[i];
        if (e->is_dir) continue;
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

/* Test the password against the earliest encrypted entry.  Header
 * encryption is already proven by a successful listing. */
static BOOL validate_password(ARCHIVE* a)
{
    WCHAR pwd[600], cmd[32768], listpath[MAX_PATH * 2];
    BYTE* out = NULL;
    DWORD size = 0, rc;
    int i, pick = -1;

    if (!a->has_password) return FALSE;
    for (i = 0; i < a->count; i++) {
        ENTRY* e = &a->entries[i];
        if (e->encrypted && (pick < 0 || e->offset < a->entries[pick].offset))
            pick = i;
    }
    if (pick < 0) return TRUE;

    pwd[0] = 0;
    password_switch(pwd, 600, a->password);
    if (!make_list_path(a, listpath, MAX_PATH * 2) ||
        !write_entry_list(&a->entries[pick], listpath))
        return TRUE;   /* cannot test - do not block opening */

    _snwprintf(cmd, 32768, L"\"%s\" t -y -scsUTF-8%s \"%s\" @\"%s\"",
               g_7za, pwd, a->archive_w, listpath);
    rc = run_7za(cmd, &out, &size, TRUE, NULL);
    DeleteFileW(listpath);
    free(out);
    mm_logf("password validation rc=%lu entry=%s", rc,
            a->entries[pick].name_utf8 ? a->entries[pick].name_utf8 : "?");
    return rc == 0;
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
    if (a->stream_wake) SetEvent(a->stream_wake);
    LeaveCriticalSection(&a->cs);
}

/* Open the cache file for a streamed entry.  When the file already exists
 * with the expected size it is reused; the entry is still decoded but the
 * data is discarded. */
static void create_parent_dirs(const WCHAR* path)
{
    WCHAR tmp[MAX_PATH * 2];
    int i, n = (int)wcslen(path);

    if (n <= 3 || n >= MAX_PATH * 2) return;
    wcscpy(tmp, path);
    for (i = 3; i < n; i++) {
        if (tmp[i] == L'\\') {
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = L'\\';
        }
    }
}

static HANDLE open_stream_entry(ARCHIVE* a, int idx, ENTRY* e, BOOL* skip, BOOL* fail)
{
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    LARGE_INTEGER sz;

    *skip = FALSE;
    *fail = FALSE;
    if (!build_cache_path(a, e->name_w, path, MAX_PATH * 2)) {
        mm_logf("stream: bad cache path for %s", e->name_utf8 ? e->name_utf8 : "?");
        *skip = TRUE;
        return INVALID_HANDLE_VALUE;
    }

    if (a->memcache) {
        /* Temporary + delete-on-close: the cache manager disables the lazy
         * writer, so the data stays in RAM (spilling only under memory
         * pressure) and the file disappears when the process exits.  The
         * handle is kept open in a->pins to keep the file alive. */
        create_parent_dirs(path);
        h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            mm_logf("memcache: create failed err=%lu path=%hs", GetLastError(),
                    e->name_utf8 ? e->name_utf8 : "?");
            *skip = TRUE;
            *fail = TRUE;
            return INVALID_HANDLE_VALUE;
        }
        if (a->pins && idx >= 0 && idx < a->count) a->pins[idx] = h;
        return h;
    }

    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(h, &sz) && (ULONGLONG)sz.QuadPart == e->size) {
            CloseHandle(h);
            *skip = TRUE;
            return INVALID_HANDLE_VALUE;
        }
        CloseHandle(h);
    }
    create_parent_dirs(path);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        mm_logf("stream: create failed err=%lu path=%hs", GetLastError(),
                e->name_utf8 ? e->name_utf8 : "?");
        *skip = TRUE;
        *fail = TRUE;
    }
    return h;
}

/* Single solid block: one `7za x -so` process produces the archive's
 * unpacked stream in archive order.  We split it by the sizes from the
 * listing and write the entries to the cache.  Decoding stops (the pipe
 * provides natural back-pressure) once everything up to want_until has
 * been produced, so opening a 2GB solid archive does not decode it all. */
static unsigned __stdcall stream_reader(void* param)
{
    ARCHIVE* a = (ARCHIVE*)param;
    WCHAR listpath[MAX_PATH * 2];
    WCHAR cmd[32768];
    WCHAR pwd[600];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rpipe = NULL, wpipe = NULL, nul = INVALID_HANDLE_VALUE;
    HANDLE out = INVALID_HANDLE_VALUE;
    BYTE* chunk = NULL;
    DWORD rc = (DWORD)-1;
    ULONGLONG pos = 0;
    int idx = 0;
    BOOL opened = FALSE, skip = FALSE, fail = FALSE, cancelled = FALSE;
    BOOL pause_logged = FALSE;
    DWORD got;
    LONG last_mb = -1;

    listpath[0] = 0;
    ZeroMemory(&pi, sizeof(pi));
    chunk = (BYTE*)malloc(STREAM_CHUNK);
    if (!chunk || !make_list_path(a, listpath, MAX_PATH * 2) ||
        !write_all_list(a, listpath)) {
        fail = TRUE;
        goto done;
    }
    pwd[0] = 0;
    if (a->has_password) password_switch(pwd, 600, a->password);
    _snwprintf(cmd, 32768, L"\"%s\" x -so -y%s -scsUTF-8 \"%s\" @\"%s\"",
               g_7za, pwd, a->archive_w, listpath);
    log_wide("stream: ", cmd);

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 1 << 20)) { fail = TRUE; goto done; }
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) { fail = TRUE; goto done; }
    mm_logf("stream: pipe ok");

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = nul;      /* never mix diagnostics into the data stream */
    si.hStdInput = nul;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        mm_logf("stream: CreateProcess failed err=%lu", GetLastError());
        fail = TRUE;
        goto done;
    }
    mm_logf("stream: process started pid=%lu", pi.dwProcessId);
    CloseHandle(wpipe); wpipe = NULL;
    CloseHandle(nul); nul = INVALID_HANDLE_VALUE;
    CloseHandle(pi.hThread);
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    EnterCriticalSection(&a->cs);
    a->procs[0] = pi.hProcess;
    LeaveCriticalSection(&a->cs);

    for (;;) {
        DWORD off;

        if (a->cancel) { cancelled = TRUE; break; }
        if (idx >= a->count || pos >= a->total_bytes)
            break;   /* everything decoded - drain EOF and finish */

        /* Wait at the current frontier; getfile raises want_until. */
        {
            ULONGLONG want;
            EnterCriticalSection(&a->cs);
            want = a->want_until;
            LeaveCriticalSection(&a->cs);
            if (pos >= want) {
                if (!pause_logged) {
                    pause_logged = TRUE;
                    mm_logf("stream: paused pos=%I64u want=%I64u", pos, want);
                }
                WaitForSingleObject(a->stream_wake, 200);
                continue;
            }
            pause_logged = FALSE;
        }

        if (!ReadFile(rpipe, chunk, STREAM_CHUNK, &got, NULL) || got == 0) {
            mm_logf("stream: eof got=%lu err=%lu", got, GetLastError());
            break;
        }

        off = 0;
        while (off < got) {
            ENTRY* e;
            ULONGLONG remain;
            DWORD n;

            if (idx >= a->count) { off = got; break; }
            e = &a->entries[idx];
            remain = e->size - (pos - e->offset);

            if (!opened) {
                out = open_stream_entry(a, idx, e, &skip, &fail);
                opened = TRUE;
                if (e->size == 0) {
                    if (out != INVALID_HANDLE_VALUE && !a->memcache) CloseHandle(out);
                    out = INVALID_HANDLE_VALUE;
                    idx++; opened = FALSE;
                    continue;
                }
            }

            n = (DWORD)((ULONGLONG)(got - off) < remain ? (got - off) : remain);
            if (out != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                if (!WriteFile(out, chunk + off, n, &wr, NULL) || wr != n)
                    fail = TRUE;
            }
            off += n;
            pos += n;
            if (pos >= e->offset + e->size) {
                if (out != INVALID_HANDLE_VALUE && !a->memcache) CloseHandle(out);
                out = INVALID_HANDLE_VALUE;
                idx++;
                opened = FALSE;
            }
        }

        {
            LONG mb = (LONG)(pos >> 20);
            if (mb != last_mb) {
                last_mb = mb;
                InterlockedExchange(&a->stream_mb, mb);
            }
        }
    }

    /* Drain the rest (normally empty); lets 7za exit cleanly. */
    if (!cancelled && rpipe) {
        while (ReadFile(rpipe, chunk, STREAM_CHUNK, &got, NULL) && got > 0)
            ;
    }
    CloseHandle(rpipe); rpipe = NULL;
    mm_logf("stream: waiting for process, pos=%I64u idx=%d/%d", pos, idx, a->count);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    mm_logf("stream: process exit rc=%lu", rc);
    EnterCriticalSection(&a->cs);
    a->procs[0] = NULL;
    LeaveCriticalSection(&a->cs);
    CloseHandle(pi.hProcess);
    pi.hProcess = NULL;

    /* Materialize trailing empty files (they produce no stream bytes). */
    while (idx < a->count && a->entries[idx].size == 0) {
        ENTRY* e = &a->entries[idx];
        out = open_stream_entry(a, idx, e, &skip, &fail);
        if (out != INVALID_HANDLE_VALUE && !a->memcache) CloseHandle(out);
        out = INVALID_HANDLE_VALUE;
        idx++;
    }

done:
    if (out != INVALID_HANDLE_VALUE && !a->memcache) CloseHandle(out);
    out = INVALID_HANDLE_VALUE;
    if (rpipe) CloseHandle(rpipe);
    if (wpipe) CloseHandle(wpipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (listpath[0]) DeleteFileW(listpath);
    free(chunk);

    EnterCriticalSection(&a->cs);
    if (cancelled || a->cancel) {
        InterlockedExchange(&a->extract_cancelled, 1);
        mm_logf("stream cancelled at %I64u", pos);
    } else if (fail || rc != 0 || idx < a->count) {
        a->worker_failed = TRUE;
        InterlockedExchange(&a->extract_failed, 1);
        InterlockedExchange(&a->cancel, 1);
        mm_logf("stream failed rc=%lu idx=%d/%d fail=%d", rc, idx, a->count, fail);
    } else {
        if (!a->memcache) write_cache_marker(a);
        a->cache_ready = TRUE;
        mm_logf("stream complete: %I64u bytes", pos);
    }
    SetEvent(a->done_event);
    LeaveCriticalSection(&a->cs);
    return 0;
}

static BOOL ensure_stream_started(ARCHIVE* a)
{
    int i;

    if (a->cache_ready) return TRUE;

    EnterCriticalSection(&a->cs);

    if (a->stream_started) {
        if (!a->extract_failed && !a->extract_cancelled &&
            !InterlockedCompareExchange(&a->cancel, 0, 0)) {
            LeaveCriticalSection(&a->cs);   /* decoding / paused */
            return TRUE;
        }
        LeaveCriticalSection(&a->cs);
        if (a->stream_thread) WaitForSingleObject(a->stream_thread, 15000);
        EnterCriticalSection(&a->cs);
        if (a->cache_ready) { LeaveCriticalSection(&a->cs); return TRUE; }
        if (a->extract_failed) { LeaveCriticalSection(&a->cs); return TRUE; }
        /* cancelled by the caller - restart from the beginning */
        if (a->stream_thread) {
            CloseHandle(a->stream_thread);
            a->stream_thread = NULL;
        }
        a->stream_started = FALSE;
        a->worker_failed = FALSE;
        InterlockedExchange(&a->extract_failed, 0);
        InterlockedExchange(&a->extract_cancelled, 0);
        InterlockedExchange(&a->cancel, 0);
        InterlockedExchange(&a->stream_mb, 0);
        a->want_until = 0;
        a->procs[0] = NULL;
        ResetEvent(a->done_event);
        if (a->pins) {
            for (i = 0; i < a->count; i++)
                if (a->pins[i]) { CloseHandle(a->pins[i]); a->pins[i] = NULL; }
        }
    }

    if (!ensure_cache_dir(a)) {
        InterlockedExchange(&a->extract_failed, 1);
        SetEvent(a->done_event);
        LeaveCriticalSection(&a->cs);
        return FALSE;
    }
    if (!a->memcache && cache_marker_exists(a)) {
        a->cache_ready = TRUE;
        LeaveCriticalSection(&a->cs);
        return TRUE;
    }

    if (!a->stream_wake)
        a->stream_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!a->stream_wake) {
        InterlockedExchange(&a->extract_failed, 1);
        SetEvent(a->done_event);
        LeaveCriticalSection(&a->cs);
        return FALSE;
    }

    a->want_until = 0;
    InterlockedExchange(&a->stream_mb, 0);
    a->stream_started = TRUE;
    a->stream_thread = (HANDLE)_beginthreadex(NULL, 0, stream_reader, a, 0, NULL);
    if (!a->stream_thread) {
        a->stream_started = FALSE;
        InterlockedExchange(&a->extract_failed, 1);
        SetEvent(a->done_event);
        LeaveCriticalSection(&a->cs);
        return FALSE;
    }
    LeaveCriticalSection(&a->cs);
    return TRUE;
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
        WCHAR pwd[600];
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
            pwd[0] = 0;
            if (a->has_password) password_switch(pwd, 600, a->password);
            _snwprintf(cmd, 32768,
                       L"\"%s\" x -y%s -bsp1 -sccUTF-8 -scsUTF-8 -o\"%s\" \"%s\" @\"%s\"",
                       g_7za, pwd, a->cache_w, a->archive_w, listpath);
        } else {
            pwd[0] = 0;
            if (a->has_password) password_switch(pwd, 600, a->password);
            _snwprintf(cmd, 32768,
                       L"\"%s\" x -y%s -bsp1 -sccUTF-8 -o\"%s\" \"%s\"",
                       g_7za, pwd, a->cache_w, a->archive_w);
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

    if (a->stream_mode)
        return ensure_stream_started(a);

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
    if (h == INVALID_HANDLE_VALUE) {
        mm_logf("read id=%lu open failed err=%lu size=%I64u", e->id, GetLastError(), e->size);
        return CACHE_NOT_READY;
    }
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return CACHE_NOT_READY;
    }
    if ((ULONGLONG)sz.QuadPart != e->size) {
        mm_logf("read id=%lu size=%I64d want=%I64u", e->id,
                (LONGLONG)sz.QuadPart, e->size);
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
        mm_logf("read id=%lu crc mismatch got=%08lx want=%08lx",
                e->id, crc32_buf(buf, (DWORD)e->size), e->crc);
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
        int r;

        if (a->stream_mode) {
            /* Ask the decoder to go at least through this entry plus a
             * read-ahead window.  It stays paused otherwise. */
            ULONGLONG need = e->offset + e->size;
            if (need < a->total_bytes) need += g_prefetch;
            if (need > a->total_bytes) need = a->total_bytes;
            EnterCriticalSection(&a->cs);
            if (need > a->want_until) {
                a->want_until = need;
                if (a->stream_wake) SetEvent(a->stream_wake);
            }
            LeaveCriticalSection(&a->cs);
        }

        r = read_cache_entry(a, e, out, out_size);
        if (r == CACHE_OK) {
            mm_logf("served id=%lu in %lu ms (pct=%d)", e->id,
                    GetTickCount() - t0, archive_pct(a));
            if (cb) cb(1, 1, lData);
            return 0;
        }
        if (r == CACHE_ERROR) {
            mm_logf("wait id=%lu: cache error", e->id);
            return ERR_MEMORY;
        }
        if (InterlockedCompareExchange(&a->extract_failed, 0, 0)) {
            mm_logf("wait id=%lu: extraction failed", e->id);
            return ERR_BROKEN;
        }
        if (InterlockedCompareExchange(&a->extract_cancelled, 0, 0)) return 1;

        if (WaitForSingleObject(a->done_event, 0) == WAIT_OBJECT_0) {
            /* extraction finished successfully but this entry is absent/bad */
            r = read_cache_entry(a, e, out, out_size);
            if (r == CACHE_OK) { if (cb) cb(1, 1, lData); return 0; }
            mm_logf("wait id=%lu: done but read=%d", e->id, r);
            return ERR_BROKEN;
        }
        if (cb) {
            DWORD now = GetTickCount();
            if (now - last_cb >= 80) {
                int pct;
                last_cb = now;
                if (a->stream_mode) {
                    ULONGLONG end = e->offset + e->size;
                    ULONGLONG pos = (ULONGLONG)(ULONG)InterlockedCompareExchange(
                                        &a->stream_mb, 0, 0) << 20;
                    pct = end ? (int)(pos * 100 / end) : 99;
                    if (pct > 99) pct = 99;
                    if (pct < 0) pct = 0;
                } else {
                    pct = archive_pct(a);
                }
                if (cb(pct, 100, lData) != 0) {
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
    WCHAR pwd[600];
    BYTE* out = NULL;
    DWORD size = 0, rc;

    *err = ERR_BROKEN;
    if (!ensure_cache_dir(a)) { *err = ERR_MEMORY; return NULL; }
    if (!make_list_path(a, listpath, MAX_PATH * 2) || !write_entry_list(e, listpath))
        return NULL;
    pwd[0] = 0;
    if (a->has_password) password_switch(pwd, 600, a->password);
    _snwprintf(cmd, 32768, L"\"%s\" x -so -y%s -scsUTF-8 \"%s\" @\"%s\"",
               g_7za, pwd, a->archive_w, listpath);
    rc = run_7za(cmd, &out, &size, TRUE, NULL);
    DeleteFileW(listpath);
    if (rc != 0) {
        free(out);
        mm_logf("extract-one failed rc=%lu", rc);
        return NULL;
    }
    {
        BYTE* lb = (BYTE*)LocalAlloc(LMEM_FIXED, size ? size : 1);
        if (!lb) { free(out); *err = ERR_MEMORY; return NULL; }
        if (size && out) memcpy(lb, out, size);
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

static void free_archive(ARCHIVE* a)
{
    ARCHIVE** p;
    int i;

    if (!a) return;
    for (p = &g_archives; *p; p = &(*p)->next)
        if (*p == a) { *p = a->next; break; }
    free_entries(a);
    if (a->workers) {
        for (i = 0; i < a->worker_count; i++) {
            if (a->workers[i].thread) CloseHandle(a->workers[i].thread);
            free(a->workers[i].blocks);
        }
        free(a->workers);
    }
    if (a->pins) {
        for (i = 0; i < a->count; i++)
            if (a->pins[i]) CloseHandle(a->pins[i]);
        free(a->pins);
    }
    if (a->stream_thread) CloseHandle(a->stream_thread);
    if (a->stream_wake) CloseHandle(a->stream_wake);
    if (a->done_event) CloseHandle(a->done_event);
    free(a->block_bytes);
    free(a->block_pct);
    DeleteCriticalSection(&a->cs);
    free(a);
}

/* Obtain and check a password after the archive has been listed.  Header
 * encryption was already proven by the listing; data-only encryption is
 * tested by extracting the earliest encrypted entry. */
static BOOL acquire_valid_password(ARCHIVE* a, BOOL header_encrypted)
{
    int tries;

    if (a->has_password && !a->encrypted) return TRUE;
    if (header_encrypted) return TRUE;
    if (!a->encrypted) return TRUE;

    for (tries = 0; tries < 3; tries++) {
        if (!a->has_password) {
            if (g_has_env_password) return FALSE;
            if (!ask_password(a, tries > 0)) return FALSE;
        }
        if (validate_password(a)) return TRUE;
        a->has_password = FALSE;
        a->password[0] = 0;
        if (g_has_env_password) return FALSE;
    }
    return FALSE;
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
    a->is_mem = mem;
    a->remember_choice = g_remember_password;

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

    if (g_has_env_password) {
        wcsncpy(a->password, g_env_password, 511);
        a->password[511] = 0;
        a->has_password = TRUE;
    } else if (!a->is_mem && g_remember_password) {
        if (load_password(a->archive_w, a->password, 512))
            a->has_password = TRUE;
    }

    a->next = g_archives;
    g_archives = a;

    {
        int tries, r = -1;
        BOOL header_encrypted = FALSE;

        for (tries = 0; tries < 3; tries++) {
            r = list_archive(a);
            if (r == 0) break;
            if (r != 1) break;
            header_encrypted = TRUE;
            if (g_has_env_password) { r = -1; break; }
            {
                BOOL had = a->has_password;
                a->has_password = FALSE;
                a->password[0] = 0;
                if (!ask_password(a, had || tries > 0)) { r = -1; break; }
            }
        }
        if (r != 0) {
            *err = TRUE;
            free_archive(a);
            return NULL;
        }
        if (!acquire_valid_password(a, header_encrypted)) {
            *err = TRUE;
            free_archive(a);
            return NULL;
        }
        if (!a->is_mem && (a->encrypted || header_encrypted))
            save_password(a->archive_w, a->password, a->remember_choice);
        if (a->memcache) {
            int n = (int)wcslen(a->cache_w);
            if (n + 5 < MAX_PATH * 2) wcscat(a->cache_w, L"_mem");
        }
        cache_cleanup(a->cache_w, a->archive_w);
    }
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
    case 1: return copy_info("7z Archive Plug-in v1.6 (on-demand / parallel / password)", buf, buflen);
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
        InitializeCriticalSection(&g_logcs);
        g_logcs_init = TRUE;
        DisableThreadLibraryCalls(hm);
        crc32_init();

        /* Configuration file: SusiePlugin\7z.ini (preferred), or the [7z]
         * section of MangaMeeya.ini next to the application.  Environment
         * variables, read below, override these values. */
        {
            WCHAR ini[MAX_PATH], appini[MAX_PATH * 2];
            get_plugin_dir(ini, MAX_PATH);
            _snwprintf(appini, MAX_PATH * 2, L"%s..\\MangaMeeya.ini", ini);
            if (wcslen(ini) + 8 < MAX_PATH) wcscat(ini, L"7z.ini");
            if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
                if (GetFileAttributesW(appini) != INVALID_FILE_ATTRIBUTES) {
                    wcsncpy(ini, appini, MAX_PATH - 1);
                    ini[MAX_PATH - 1] = 0;
                } else {
                    ini[0] = 0;
                }
            }
            if (ini[0]) {
                int n;
                n = (int)GetPrivateProfileIntW(L"7z", L"CacheMaxMB", (UINT)-1, ini);
                if (n >= 0 && n <= 1024 * 1024)     /* 0 = no cleanup */
                    g_cache_max = (ULONGLONG)n * 1024ULL * 1024ULL;
                n = (int)GetPrivateProfileIntW(L"7z", L"PrefetchMB", (UINT)-1, ini);
                if (n >= 0 && n <= 4096)
                    g_prefetch = (ULONGLONG)n * 1024ULL * 1024ULL;
                n = (int)GetPrivateProfileIntW(L"7z", L"Threads", (UINT)-1, ini);
                if (n >= 0 && n <= MAX_WORKERS)
                    g_threads = n;
                n = (int)GetPrivateProfileIntW(L"7z", L"MemCache", (UINT)-1, ini);
                if (n >= 0)
                    g_memcache = (n != 0);
                n = (int)GetPrivateProfileIntW(L"7z", L"Log", (UINT)-1, ini);
                if (n > 0)
                    g_log = TRUE;
                n = (int)GetPrivateProfileIntW(L"7z", L"RememberPassword", (UINT)-1, ini);
                if (n >= 0)
                    g_remember_password = (n != 0);
            }
        }
        load_remember_pref();   /* last dialog choice overrides the ini */

        if (GetEnvironmentVariableA("MM7Z_LOG", v, sizeof(v)) > 0 && v[0] == '1')
            g_log = TRUE;
        if (GetEnvironmentVariableA("MM7Z_THREADS", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n < 1) n = 1;
            if (n > MAX_WORKERS) n = MAX_WORKERS;
            g_threads = n;
        }
        if (GetEnvironmentVariableA("MM7Z_PREFETCH", v, sizeof(v)) > 0) {
            int n = atoi(v);   /* MiB */
            if (n >= 0 && n <= 4096)
                g_prefetch = (ULONGLONG)n * 1024ULL * 1024ULL;
        }
        if (GetEnvironmentVariableA("MM7Z_MEMCACHE", v, sizeof(v)) > 0 && v[0] == '1')
            g_memcache = TRUE;
        if (GetEnvironmentVariableA("MM7Z_CACHE_MAX_MB", v, sizeof(v)) > 0) {
            int n = atoi(v);
            if (n >= 0 && n <= 1024 * 1024)   /* 0 = no cleanup */
                g_cache_max = (ULONGLONG)n * 1024ULL * 1024ULL;
        }
        if (GetEnvironmentVariableA("MM7Z_REMEMBER_PASSWORD", v, sizeof(v)) > 0 && v[0] == '0')
            g_remember_password = FALSE;
        if (GetEnvironmentVariableW(L"MM7Z_PASSWORD", g_env_password, 512) > 0) {
            g_env_password[511] = 0;
            g_has_env_password = TRUE;
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
