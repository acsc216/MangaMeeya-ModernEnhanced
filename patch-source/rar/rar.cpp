/*
 * rar.spi - RAR archive plug-in for MangaMeeya (Susie 32-bit 00AM)
 *
 * Uses the official UnRAR library (RARLab) compiled into this DLL.
 * Supported: RAR 1.5 - 3.x (RAR4), RAR5, data/header encryption,
 * multi-volume archives, files larger than 4 GB (internal 64-bit).
 *
 * MangaMeeya passes fileInfo.position back as the "len" argument of
 * GetFile, so position is used as the entry id.
 *
 * Solid archives are decoded sequentially.  Every decoded entry is
 * written to %TEMP%\MangaMeeya_rar\<hash>\<id>.bin and reused later.
 * A forward request continues from the live UnRAR handle; a backward
 * request is served from the cache (or restarts from the beginning if
 * the cache entry is missing).
 *
 * Build (32-bit):
 *   cl /nologo /c /MT /O2 /EHsc /W0 /DRARDLL /DUNRAR /DSILENT /Iunrar unrar\*.cpp
 *   cl /nologo /c /MT /O2 /EHsc /W0 /Iunrar rar.cpp
 *   link /DLL /OUT:rar.spi /DEF:rar.def *.obj user32.lib crypt32.lib
 *
 * Environment:
 *   MMRAR_LOG=1            append diagnostics to %TEMP%\rarspi.log
 *   MMRAR_PASSWORD=...     preset password (automation/tests)
 *   MMRAR_REMEMBER_PASSWORD=0
 *   MMRAR_CACHE_MAX_MB=n   cache limit, default 5120 (0 = no cleanup)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <time.h>
#include <wincrypt.h>

#include <string>
#include <vector>

#include "unrar/dll.hpp"

#define ERR_UNKNOWN      2
#define ERR_BROKEN       3
#define ERR_MEMORY       4
#define ERR_READ         6
#define ERR_INTERNAL     8

#define ERR_PASSWORD    (-100)   /* internal only */
#define CACHE_SAMPLE    65536

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

struct ENTRY {
    std::wstring wname;
    std::string ansi;
    std::string utf8;
    ULONGLONG size;
    ULONGLONG packsize;
    DWORD crc;
    long mtime;
    int id;               /* 1-based */
    BOOL encrypted;
    BOOL solid;
};

struct SINK {
    BYTE* mem;
    SIZE_T mem_cap;
    SIZE_T mem_len;
    HANDLE file;          /* cache .tmp file, or INVALID_HANDLE_VALUE */
    ULONGLONG file_len;
    PROGRESS_CB cb;
    long ldata;
    ULONGLONG done;
    ULONGLONG total;
    ULONGLONG last_report;
    BOOL abort;
    BOOL overflow;
};

struct ARCHIVE {
    char key[MAX_PATH * 2];
    std::wstring path_w;
    std::string cache_hash;
    std::wstring cache_dir;
    BOOL is_mem;
    BOOL solid;
    BOOL enc_headers;
    BOOL any_encrypted;
    BOOL has_password;
    WCHAR password[512];
    BOOL remember_choice;
    BOOL pwd_cancel;          /* user cancelled the dialog recently */
    DWORD pwd_cancel_tick;
    std::vector<ENTRY> entries;

    /* live sequential decoder for solid archives */
    HANDLE seq_h;
    int seq_next;         /* next file ordinal (0-based) */
    SINK* sink;

    CRITICAL_SECTION cs;
    ARCHIVE* next;
};

static HMODULE g_self;
static CRITICAL_SECTION g_lock;
static BOOL g_lock_init;
static CRITICAL_SECTION g_pwd_lock;   /* serializes password dialogs */
static BOOL g_log_on;
static BOOL g_remember_password = TRUE;
static WCHAR g_env_password[512];
static BOOL g_has_env_password;
static int g_cache_max_mb = 5120;
static ARCHIVE* g_archives;

static void save_remember_pref(BOOL on);
static BOOL ask_password(ARCHIVE* a, BOOL again);
static BOOL pwd_cancel_recent(ARCHIVE* a);
static void pwd_cancel_set(ARCHIVE* a);

static const char g_pw_entropy[] = "MangaMeeya_rar_password";

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static void logf(const char* fmt, ...)
{
    char tmp[MAX_PATH], path[MAX_PATH];
    FILE* f;
    va_list ap;
    SYSTEMTIME st;

    if (!g_log_on) return;
    if (!GetTempPathA(MAX_PATH, tmp)) return;
    _snprintf(path, sizeof(path), "%srarspi.log", tmp);
    f = fopen(path, "a");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static ULONGLONG fnv1a_bytes(const BYTE* p, size_t n, ULONGLONG h)
{
    while (n--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

static ULONGLONG fnv1a_wstr(const WCHAR* s)
{
    ULONGLONG h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned short)*s++; h *= 1099511628211ULL; }
    return h;
}

static void hash_path_cache(const WCHAR* path, char* out)
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

static ULONGLONG mem_sample_hash(const BYTE* p, long n)
{
    ULONGLONG h = 1469598103934665603ULL;
    long head = n < CACHE_SAMPLE ? n : CACHE_SAMPLE;
    long tail = n < CACHE_SAMPLE ? 0 : CACHE_SAMPLE;
    h ^= (ULONGLONG)n; h *= 1099511628211ULL;
    h = fnv1a_bytes(p, (size_t)head, h);
    if (tail) h = fnv1a_bytes(p + n - tail, (size_t)tail, h);
    return h;
}

static int map_rar_error(int e)
{
    switch (e) {
    case ERAR_SUCCESS:            return 0;
    case ERAR_NO_MEMORY:          return ERR_MEMORY;
    case ERAR_BAD_DATA:           return ERR_BROKEN;
    case ERAR_BAD_ARCHIVE:        return ERR_UNKNOWN;
    case ERAR_UNKNOWN_FORMAT:     return ERR_UNKNOWN;
    case ERAR_EOPEN:              return ERR_READ;
    case ERAR_ECREATE:            return ERR_READ;
    case ERAR_ECLOSE:             return ERR_READ;
    case ERAR_EREAD:              return ERR_READ;
    case ERAR_EWRITE:             return ERR_READ;
    case ERAR_MISSING_PASSWORD:   return ERR_PASSWORD;
    case ERAR_BAD_PASSWORD:       return ERR_PASSWORD;
    default:                      return ERR_UNKNOWN;
    }
}

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
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
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
    if (q) q[1] = 0; else path[0] = 0;
    wcsncpy(dir, path, cap - 1);
    dir[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* password dialog (same approach as 7z.spi)                           */
/* ------------------------------------------------------------------ */

typedef struct { const WCHAR* text; WCHAR* value; int cap; BOOL remember; } PWDCTX;

/* The dialog is modal and password prompts are serialized per archive, so a
 * single active context is sufficient.  We keep it in a global instead of
 * relying on DWLP_USER, which has proven unreliable when the dialog is
 * created from a worker thread. */
static PWDCTX* g_pwd_ctx;

static BYTE* dlg_align(BYTE* p)
{
    while ((ULONG_PTR)p & 3) *p++ = 0;
    return p;
}

static BYTE* dlg_wstr(BYTE* p, const WCHAR* s)
{
    while (*s) { *(WORD*)p = (WORD)*s++; p += 2; }
    *(WORD*)p = 0; p += 2;
    return p;
}

static BYTE* dlg_item(BYTE* p, DWORD style, short x, short y, short cx, short cy,
                      WORD id, WORD cls, const WCHAR* text)
{
    DLGITEMTEMPLATE* it;
    p = dlg_align(p);              /* every item must start DWORD-aligned */
    it = (DLGITEMTEMPLATE*)p;
    it->style = style;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy;
    it->id = id;
    p = (BYTE*)(it + 1);
    *(WORD*)p = 0xFFFF; p += 2;
    *(WORD*)p = cls; p += 2;
    p = dlg_wstr(p, text);
    *(WORD*)p = 0; p += 2;
    return dlg_align(p);
}

static INT_PTR CALLBACK pwd_dlg_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PWDCTX* ctx = g_pwd_ctx;
    switch (msg) {
    case WM_INITDIALOG:
        ctx = (PWDCTX*)lParam;
        g_pwd_ctx = ctx;
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
            {
                int n = GetDlgItemTextW(hDlg, 1001, ctx->value, ctx->cap);
                ctx->remember = (IsDlgButtonChecked(hDlg, 1003) == BST_CHECKED);
                logf("IDOK n=%d len=%u first=%04x", n, (unsigned)wcslen(ctx->value),
                     (unsigned)(ctx->value[0] & 0xFFFF));
                EndDialog(hDlg, 1);
            }
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
    dlg->style = WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU |
                 DS_MODALFRAME | DS_CENTER;
    dlg->dwExtendedStyle = WS_EX_DLGMODALFRAME;
    dlg->cdit = 5;
    dlg->x = 10; dlg->y = 10; dlg->cx = 220; dlg->cy = 92;
    p = (BYTE*)(dlg + 1);
    *(WORD*)p = 0; p += 2;
    *(WORD*)p = 0; p += 2;
    p = dlg_wstr(p, L"RAR Password");
    p = dlg_align(p);

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
    logf("show_pwd: parent=%p ret=%Id err=%lu", parent, r, GetLastError());
    g_pwd_ctx = NULL;
    if (r == 1 && remember) *remember = ctx.remember;
    return r == 1;
}

static BOOL ask_password(ARCHIVE* a, BOOL again)
{
    WCHAR buf[512];
    HWND parent;
    BOOL remember;
    BOOL ok;

    EnterCriticalSection(&g_pwd_lock);

    if (a->has_password) {
        LeaveCriticalSection(&g_pwd_lock);
        return TRUE;
    }
    if (pwd_cancel_recent(a)) {
        LeaveCriticalSection(&g_pwd_lock);
        return FALSE;
    }

    logf("ask_password: again=%d env=%d has=%d", again, g_has_env_password, a->has_password);
    if (g_has_env_password) {
        wcsncpy(a->password, g_env_password, 511);
        a->password[511] = 0;
        a->has_password = TRUE;
        a->pwd_cancel = FALSE;
        a->remember_choice = g_remember_password;
        LeaveCriticalSection(&g_pwd_lock);
        return TRUE;
    }

    parent = GetActiveWindow();
    if (!parent) parent = GetForegroundWindow();
    if (again)
        MessageBoxW(parent,
                    L"\x5BC6\x7801\x9519\x8BEF\xFF0C\x8BF7\x91CD\x8BD5\x3002",
                    L"RAR", MB_OK | MB_ICONWARNING);

    buf[0] = 0;
    remember = a->remember_choice;
    ok = show_password_dialog(parent,
            L"\x538B\x7F29\x5305\x5DF2\x52A0\x5BC6\xFF0C\x8BF7\x8F93\x5165\x5BC6\x7801\xFF1A",
            buf, 512, &remember);
    if (!ok) {
        pwd_cancel_set(a);
        LeaveCriticalSection(&g_pwd_lock);
        return FALSE;
    }
    a->remember_choice = remember;
    g_remember_password = remember;
    save_remember_pref(remember);
    logf("ask_password got len=%u first=%04x", (unsigned)wcslen(buf),
         (unsigned)(buf[0] & 0xFFFF));
    wcsncpy(a->password, buf, 511);
    a->password[511] = 0;
    SecureZeroMemory(buf, sizeof(buf));
    a->has_password = TRUE;
    a->pwd_cancel = FALSE;
    LeaveCriticalSection(&g_pwd_lock);
    return TRUE;
}

static BOOL password_file_path(const WCHAR* archive, WCHAR* out, int cap)
{
    WCHAR appdata[MAX_PATH], dir[MAX_PATH * 2], hash[24];
    if (!archive || !archive[0]) return FALSE;
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return FALSE;
    _snwprintf(dir, MAX_PATH * 2, L"%s\\MangaMeeya_rar", appdata);
    CreateDirectoryW(dir, NULL);
    _snwprintf(dir, MAX_PATH * 2, L"%s\\MangaMeeya_rar\\passwords", appdata);
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
    out.pbData = NULL; out.cbData = 0;
    if (!CryptProtectData(&in, L"RAR password", &entropy, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        logf("password: CryptProtectData failed err=%lu", GetLastError());
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
        CloseHandle(h); return FALSE;
    }
    in.pbData = (BYTE*)malloc(size);
    if (!in.pbData) { CloseHandle(h); return FALSE; }
    if (!ReadFile(h, in.pbData, size, &got, NULL) || got != size) {
        free(in.pbData); CloseHandle(h); return FALSE;
    }
    CloseHandle(h);
    in.cbData = size;
    entropy.pbData = (BYTE*)g_pw_entropy;
    entropy.cbData = (DWORD)(sizeof(g_pw_entropy) - 1);
    decoded.pbData = NULL; decoded.cbData = 0;
    if (!CryptUnprotectData(&in, NULL, &entropy, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &decoded)) {
        free(in.pbData); return FALSE;
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

static void save_remember_pref(BOOL on)
{
    WCHAR appdata[MAX_PATH], path[MAX_PATH * 2];
    HANDLE h; DWORD wr;
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;
    _snwprintf(path, MAX_PATH * 2, L"%s\\MangaMeeya_rar", appdata);
    CreateDirectoryW(path, NULL);
    _snwprintf(path, MAX_PATH * 2, L"%s\\MangaMeeya_rar\\remember", appdata);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, on ? "1" : "0", 1, &wr, NULL);
        CloseHandle(h);
    }
}

static void load_remember_pref(void)
{
    WCHAR appdata[MAX_PATH], path[MAX_PATH * 2];
    HANDLE h; DWORD got = 0; char c = 0;
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;
    _snwprintf(path, MAX_PATH * 2, L"%s\\MangaMeeya_rar\\remember", appdata);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (ReadFile(h, &c, 1, &got, NULL) && got == 1 && (c == '0' || c == '1'))
        g_remember_password = (c == '1');
    CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* cache                                                               */
/* ------------------------------------------------------------------ */

static ULONGLONG path_size(const WCHAR* path)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    ULONGLONG total = 0;
    h = FindFirstFileW(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        WCHAR sub[MAX_PATH * 2];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        _snwprintf(sub, MAX_PATH * 2, L"%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += path_size(sub);
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
    _snwprintf(pat, MAX_PATH * 2, L"%s\\*", path);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            WCHAR sub[MAX_PATH * 2];
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            _snwprintf(sub, MAX_PATH * 2, L"%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                SetFileAttributesW(sub, FILE_ATTRIBUTE_NORMAL);
                delete_tree(sub);
                RemoveDirectoryW(sub);
            } else {
                SetFileAttributesW(sub, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(sub);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

typedef struct { ULONGLONG size; FILETIME last; WCHAR path[MAX_PATH * 2]; } CACHEITEM;

static int cacheitem_cmp(const void* a, const void* b)
{
    const CACHEITEM* x = (const CACHEITEM*)a;
    const CACHEITEM* y = (const CACHEITEM*)b;
    return CompareFileTime(&x->last, &y->last);
}

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

    if (g_cache_max_mb <= 0) return;
    if (!GetTempPathW(MAX_PATH, temp)) return;
    _snwprintf(root, MAX_PATH * 2, L"%sMangaMeeya_rar", temp);
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
            items = ni; cap = ncap;
        }
        items[n].size = size;
        items[n].last = fd.ftLastWriteTime;
        wcsncpy(items[n].path, path, MAX_PATH * 2 - 1);
        items[n].path[MAX_PATH * 2 - 1] = 0;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (n > 0 && total > (ULONGLONG)g_cache_max_mb * 1024 * 1024) {
        qsort(items, n, sizeof(CACHEITEM), cacheitem_cmp);
        for (i = 0; i < n && total > (ULONGLONG)g_cache_max_mb * 1024 * 1024; i++) {
            ULONGLONG sz = items[i].size;
            delete_tree(items[i].path);
            if (GetFileAttributesW(items[i].path) == INVALID_FILE_ATTRIBUTES)
                total -= sz;
        }
    }
    free(items);
}

static BOOL ensure_cache_dir(ARCHIVE* a)
{
    WCHAR temp[MAX_PATH], parent[MAX_PATH];
    DWORD attr;
    if (!GetTempPathW(MAX_PATH, temp)) return FALSE;
    _snwprintf(parent, MAX_PATH, L"%sMangaMeeya_rar", temp);
    CreateDirectoryW(parent, NULL);
    CreateDirectoryW(a->cache_dir.c_str(), NULL);
    attr = GetFileAttributesW(a->cache_dir.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void cache_path(ARCHIVE* a, int id, const WCHAR* ext, WCHAR* out, int cap)
{
    _snwprintf(out, cap, L"%s\\%d.%s", a->cache_dir.c_str(), id, ext);
}

static BOOL cache_complete(ARCHIVE* a, int idx)
{
    ENTRY& e = a->entries[idx];
    WCHAR path[MAX_PATH * 2];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    cache_path(a, e.id, L"bin", path, MAX_PATH * 2);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return FALSE;
    return ((ULONGLONG)fad.nFileSizeHigh << 32) + fad.nFileSizeLow == e.size;
}

static BOOL read_cache(ARCHIVE* a, int idx, BYTE** out, SIZE_T* out_size)
{
    ENTRY& e = a->entries[idx];
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD got = 0, size;
    BYTE* buf;

    if (e.size > 0x7FFFFFFFULL) return FALSE;
    if (!cache_complete(a, idx)) return FALSE;
    cache_path(a, e.id, L"bin", path, MAX_PATH * 2);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(h, NULL);
    buf = (BYTE*)LocalAlloc(LMEM_FIXED, size ? size : 1);
    if (!buf) { CloseHandle(h); return FALSE; }
    if (size && (!ReadFile(h, buf, size, &got, NULL) || got != size)) {
        CloseHandle(h); LocalFree(buf); return FALSE;
    }
    CloseHandle(h);
    *out = buf;
    *out_size = size;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* UnRAR callback and sinks                                            */
/* ------------------------------------------------------------------ */

static int CALLBACK rar_callback(UINT msg, LPARAM user, LPARAM p1, LPARAM p2)
{
    ARCHIVE* a = (ARCHIVE*)user;

    switch (msg) {
    case UCM_CHANGEVOLUMEW:
    case UCM_CHANGEVOLUME:
        /* Keep the volume name UnRAR computed; return 0 to let it open. */
        return 0;

    case UCM_NEEDPASSWORDW: {
        WCHAR* buf = (WCHAR*)p1;
        SIZE_T cap = (SIZE_T)p2;
        logf("UCM_NEEDPASSWORDW: cap=%u has=%d cancel=%d", (unsigned)cap,
             a->has_password, pwd_cancel_recent(a));
        if (cap == 0) return -1;
        if (pwd_cancel_recent(a)) return -1;
        a->pwd_cancel = FALSE;
        if (!a->has_password) {
            if (!ask_password(a, FALSE)) { pwd_cancel_set(a); return -1; }
        }
        wcsncpy(buf, a->password, cap - 1);
        buf[cap - 1] = 0;
        logf("pwd supplied len=%u first=%04x", (unsigned)wcslen(buf),
             (unsigned)(buf[0] & 0xFFFF));
        return 0;
    }
    case UCM_NEEDPASSWORD: {
        char* buf = (char*)p1;
        SIZE_T cap = (SIZE_T)p2;
        logf("UCM_NEEDPASSWORD: cap=%u has=%d cancel=%d", (unsigned)cap,
             a->has_password, pwd_cancel_recent(a));
        if (cap == 0) return -1;
        /* Do not ask twice: the wide callback above already handled it. */
        if (pwd_cancel_recent(a)) return -1;
        a->pwd_cancel = FALSE;
        if (!a->has_password) {
            if (!ask_password(a, FALSE)) { pwd_cancel_set(a); return -1; }
        }
        WideCharToMultiByte(CP_ACP, 0, a->password, -1, buf, (int)cap, NULL, NULL);
        buf[cap - 1] = 0;
        return 0;
    }
    case UCM_PROCESSDATA: {
        SINK* s = a->sink;
        SIZE_T n = (SIZE_T)p2;
        if (!s) return 0;
        if (s->mem) {
            if (s->mem_len + n > s->mem_cap) {
                s->overflow = TRUE;
                s->abort = TRUE;
                return -1;
            }
            memcpy(s->mem + s->mem_len, (const void*)p1, n);
            s->mem_len += n;
        }
        if (s->file != INVALID_HANDLE_VALUE && n) {
            DWORD wr = 0;
            if (!WriteFile(s->file, (const void*)p1, (DWORD)n, &wr, NULL) || wr != n) {
                s->abort = TRUE;
                return -1;
            }
            s->file_len += n;
        }
        s->done += n;
        if (s->cb && s->done - s->last_report >= 1024 * 1024) {
            int num = s->total > 0 ? (int)(s->done > s->total ? s->total : s->done) : 0;
            int den = s->total > 0x7FFFFFFFULL ? 0x7FFFFFFF : (int)s->total;
            s->last_report = s->done;
            if (s->cb(num, den, s->ldata) != 0) {
                s->abort = TRUE;
                return -1;
            }
        }
        return 0;
    }
    case UCM_LARGEDICT:
        return 1;
    }
    return 0;
}

static HANDLE open_rar(ARCHIVE* a, UINT mode, int* err, UINT* flags)
{
    RAROpenArchiveDataEx od;
    HANDLE h;
    ZeroMemory(&od, sizeof(od));
    od.ArcNameW = (wchar_t*)a->path_w.c_str();
    od.OpenMode = mode;
    od.Callback = rar_callback;
    od.UserData = (LPARAM)a;

    h = RAROpenArchiveEx(&od);
    if (flags) *flags = od.Flags;
    if (!h) {
        *err = map_rar_error((int)od.OpenResult);
        logf("open failed: result=%u err=%d", od.OpenResult, *err);
        return NULL;
    }
    a->enc_headers = (od.Flags & ROADF_ENCHEADERS) != 0;
    if (flags && (*flags & ROADF_SOLID)) a->solid = TRUE;
    return h;
}

static HANDLE open_rar_retry(ARCHIVE* a, UINT mode, int* err, UINT* flags)
{
    int tries;
    for (tries = 0; tries < 3; tries++) {
        HANDLE h;
        if (pwd_cancel_recent(a)) return NULL;
        h = open_rar(a, mode, err, flags);
        if (h) return h;
        if (*err != ERR_PASSWORD) return NULL;
        if (g_has_env_password) return NULL;
        a->has_password = FALSE;
        a->password[0] = 0;
        if (!ask_password(a, tries > 0)) {
            pwd_cancel_set(a);
            return NULL;
        }
    }
    return NULL;
}

static void close_seq(ARCHIVE* a)
{
    if (a->seq_h) {
        RARCloseArchive(a->seq_h);
        a->seq_h = NULL;
    }
    a->seq_next = 0;
    a->sink = NULL;
}

/* Suppress repeated prompts for the prefetch burst that follows a cancel.
 * A later explicit (re)open after the window asks again. */
static BOOL pwd_cancel_recent(ARCHIVE* a)
{
    return a->pwd_cancel && (GetTickCount() - a->pwd_cancel_tick) < 3000;
}

static void pwd_cancel_set(ARCHIVE* a)
{
    a->pwd_cancel = TRUE;
    a->pwd_cancel_tick = GetTickCount();
}

/* ------------------------------------------------------------------ */
/* listing                                                             */
/* ------------------------------------------------------------------ */

static int list_archive(ARCHIVE* a)
{
    HANDLE h;
    int err = 0;
    UINT flags = 0;
    int id = 0;

    a->entries.clear();
    a->any_encrypted = FALSE;
    h = open_rar_retry(a, RAR_OM_LIST, &err, &flags);
    if (!h) return err;
    a->solid = (flags & ROADF_SOLID) != 0;
    a->enc_headers = (flags & ROADF_ENCHEADERS) != 0;

    for (;;) {
        RARHeaderDataEx hd;
        int r;
        ZeroMemory(&hd, sizeof(hd));
        r = RARReadHeaderEx(h, &hd);
        if (r == ERAR_END_ARCHIVE) break;
        if (r != 0) {
            logf("list: header error %d", r);
            RARCloseArchive(h);
            return map_rar_error(r);
        }
        if (!(hd.Flags & RHDF_DIRECTORY) && !(hd.Flags & RHDF_SPLITBEFORE)) {
            ENTRY e;
            e.wname = hd.FileNameW;
            e.size = ((ULONGLONG)hd.UnpSizeHigh << 32) | hd.UnpSize;
            e.packsize = ((ULONGLONG)hd.PackSizeHigh << 32) | hd.PackSize;
            e.crc = hd.FileCRC;
            e.encrypted = (hd.Flags & RHDF_ENCRYPTED) != 0;
            e.solid = (hd.Flags & RHDF_SOLID) != 0;
            e.id = ++id;
            e.mtime = 0;
            {
                FILETIME ft;
                BOOL have = FALSE;
                if (hd.MtimeLow || hd.MtimeHigh) {
                    ft.dwLowDateTime = hd.MtimeLow;
                    ft.dwHighDateTime = hd.MtimeHigh;
                    have = TRUE;
                } else if (hd.FileTime) {
                    WORD d = (WORD)(hd.FileTime & 0xFFFF);
                    WORD t = (WORD)(hd.FileTime >> 16);
                    have = DosDateTimeToFileTime(d, t, &ft);
                }
                if (have) {
                    FILETIME local;
                    SYSTEMTIME st;
                    if (FileTimeToLocalFileTime(&ft, &local) &&
                        FileTimeToSystemTime(&local, &st)) {
                        struct tm tm;
                        ZeroMemory(&tm, sizeof(tm));
                        tm.tm_year = st.wYear - 1900;
                        tm.tm_mon = st.wMonth - 1;
                        tm.tm_mday = st.wDay;
                        tm.tm_hour = st.wHour;
                        tm.tm_min = st.wMinute;
                        tm.tm_sec = st.wSecond;
                        e.mtime = (long)mktime(&tm);
                    }
                }
            }
            {
                int n = WideCharToMultiByte(CP_UTF8, 0, e.wname.c_str(), -1,
                                            NULL, 0, NULL, NULL);
                if (n > 1) {
                    e.utf8.resize(n - 1);
                    WideCharToMultiByte(CP_UTF8, 0, e.wname.c_str(), -1,
                                        &e.utf8[0], n, NULL, NULL);
                }
                n = WideCharToMultiByte(CP_ACP, 0, e.wname.c_str(), -1,
                                        NULL, 0, NULL, NULL);
                if (n > 1) {
                    e.ansi.resize(n - 1);
                    WideCharToMultiByte(CP_ACP, 0, e.wname.c_str(), -1,
                                        &e.ansi[0], n, NULL, NULL);
                } else {
                    e.ansi = e.utf8;
                }
            }
            if (e.encrypted) a->any_encrypted = TRUE;
            a->entries.push_back(e);
        }
        r = RARProcessFile(h, RAR_SKIP, NULL, NULL);
        if (r != 0) {
            logf("list: skip error %d", r);
            RARCloseArchive(h);
            return map_rar_error(r);
        }
    }
    RARCloseArchive(h);
    logf("listed %d entries, solid=%d enc=%d", (int)a->entries.size(), a->solid, a->any_encrypted);
    return 0;
}

/* ------------------------------------------------------------------ */
/* extraction                                                          */
/* ------------------------------------------------------------------ */

static void report_progress(PROGRESS_CB cb, long ldata, ULONGLONG done, ULONGLONG total)
{
    if (!cb) return;
    cb(done > 0x7FFFFFFFULL ? 0x7FFFFFFF : (int)done,
       total > 0x7FFFFFFFULL ? 0x7FFFFFFF : (int)total, ldata);
}

/* Decode the current header to memory (and optionally to a cache file).
 * Returns 0 on success, a Susie error code on failure. */
static int decode_current(ARCHIVE* a, HANDLE h, ENTRY* e, BYTE* mem,
                          PROGRESS_CB cb, long ldata, ULONGLONG total,
                          BOOL cache_it)
{
    SINK sink;
    WCHAR tmp[MAX_PATH * 2], bin[MAX_PATH * 2];
    BOOL have_tmp = FALSE;
    int r;

    ZeroMemory(&sink, sizeof(sink));
    sink.mem = mem;
    sink.mem_cap = (SIZE_T)(e->size ? e->size : 1);
    sink.file = INVALID_HANDLE_VALUE;
    sink.cb = cb;
    sink.ldata = ldata;
    sink.total = total;

    if (cache_it && ensure_cache_dir(a)) {
        cache_path(a, e->id, L"tmp", tmp, MAX_PATH * 2);
        sink.file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
        if (sink.file != INVALID_HANDLE_VALUE)
            have_tmp = TRUE;
    }

    a->sink = &sink;
    r = RARProcessFile(h, RAR_TEST, NULL, NULL);
    a->sink = NULL;

    if (have_tmp) {
        CloseHandle(sink.file);
        if (r == 0 && sink.file_len == e->size) {
            cache_path(a, e->id, L"bin", bin, MAX_PATH * 2);
            MoveFileExW(tmp, bin, MOVEFILE_REPLACE_EXISTING);
        } else {
            DeleteFileW(tmp);
        }
    }

    if (r != 0) {
        if (sink.abort) return 1;
        return map_rar_error(r);
    }
    if (sink.overflow || sink.mem_len != (SIZE_T)e->size)
        return ERR_BROKEN;
    return 0;
}

/* Skip the current solid header, still advancing the dictionary. */
static int skip_current(ARCHIVE* a, HANDLE h, int* err)
{
    int r = RARProcessFile(h, RAR_SKIP, NULL, NULL);
    if (r != 0) {
        *err = map_rar_error(r);
        return -1;
    }
    return 0;
}

static int extract_solid(ARCHIVE* a, int idx, BYTE** out, SIZE_T* out_size,
                         PROGRESS_CB cb, long ldata)
{
    ENTRY& target = a->entries[idx];
    BYTE* mem;
    ULONGLONG total = 0;
    ULONGLONG done = 0;
    int i, rc = 0;

    *out = NULL;
    *out_size = 0;

    /* A cached entry is enough for a backward request. */
    if (!a->seq_h || idx < a->seq_next) {
        if (read_cache(a, idx, out, out_size)) {
            report_progress(cb, ldata, target.size, target.size);
            return 0;
        }
    }
    if (a->seq_h && idx < a->seq_next)
        close_seq(a);
    if (!a->seq_h) {
        int err = 0;
        UINT flags = 0;
        a->seq_h = open_rar_retry(a, RAR_OM_EXTRACT, &err, &flags);
        if (!a->seq_h) return err;
        a->seq_next = 0;
    }

    mem = (BYTE*)LocalAlloc(LMEM_FIXED, target.size ? (SIZE_T)target.size : 1);
    if (!mem) return ERR_MEMORY;

    for (i = a->seq_next; i <= idx && i < (int)a->entries.size(); i++)
        total += a->entries[i].size;
    if (total == 0) total = 1;

    for (;;) {
        RARHeaderDataEx hd;
        int r, ordinal;
        ENTRY* cur;

        ZeroMemory(&hd, sizeof(hd));
        r = RARReadHeaderEx(a->seq_h, &hd);
        if (r == ERAR_END_ARCHIVE) { rc = ERR_BROKEN; break; }
        if (r != 0) { rc = map_rar_error(r); break; }
        if (hd.Flags & (RHDF_DIRECTORY | RHDF_SPLITBEFORE)) {
            if (skip_current(a, a->seq_h, &rc) != 0) break;
            continue;
        }
        ordinal = a->seq_next;
        if (ordinal >= (int)a->entries.size()) { rc = ERR_BROKEN; break; }
        cur = &a->entries[ordinal];
        if (hd.FileNameW[0] && cur->wname != hd.FileNameW) {
            logf("solid: name mismatch at %d", ordinal);
            rc = ERR_BROKEN;
            break;
        }

        if (ordinal < idx) {
            /* Cache every decoded entry so backward jumps are cheap. */
            int drc;
            if (cache_complete(a, ordinal)) {
                if (skip_current(a, a->seq_h, &rc) != 0) break;
                done += cur->size;
            } else {
                drc = decode_current(a, a->seq_h, cur, NULL, cb, ldata, total, TRUE);
                if (drc != 0) { rc = drc; break; }
                done += cur->size;
            }
            a->seq_next++;
            report_progress(cb, ldata, done, total);
            continue;
        }

        /* target */
        rc = decode_current(a, a->seq_h, &target, mem, cb, ldata, total, TRUE);
        if (rc == 0) {
            a->seq_next++;
            *out = mem;
            *out_size = (SIZE_T)target.size;
            report_progress(cb, ldata, total, total);
            return 0;
        }
        break;
    }

    close_seq(a);
    LocalFree(mem);
    return rc;
}

static int extract_nonsolid(ARCHIVE* a, int idx, BYTE** out, SIZE_T* out_size,
                            PROGRESS_CB cb, long ldata)
{
    ENTRY& target = a->entries[idx];
    HANDLE h;
    int err = 0;
    UINT flags = 0;
    int ordinal = 0;
    BYTE* mem;
    int rc = 0;

    *out = NULL;
    *out_size = 0;
    mem = (BYTE*)LocalAlloc(LMEM_FIXED, target.size ? (SIZE_T)target.size : 1);
    if (!mem) return ERR_MEMORY;

    h = open_rar_retry(a, RAR_OM_EXTRACT, &err, &flags);
    if (!h) { LocalFree(mem); return err; }

    for (;;) {
        RARHeaderDataEx hd;
        int r;
        ZeroMemory(&hd, sizeof(hd));
        r = RARReadHeaderEx(h, &hd);
        if (r == ERAR_END_ARCHIVE) { rc = ERR_BROKEN; break; }
        if (r != 0) { rc = map_rar_error(r); break; }
        if (hd.Flags & (RHDF_DIRECTORY | RHDF_SPLITBEFORE)) {
            if (skip_current(a, h, &rc) != 0) break;
            continue;
        }
        if (ordinal < idx) {
            r = RARProcessFile(h, RAR_SKIP, NULL, NULL);
            if (r != 0) { rc = map_rar_error(r); break; }
            ordinal++;
            continue;
        }
        rc = decode_current(a, h, &target, mem, cb, ldata, target.size, FALSE);
        if (rc == 0) {
            report_progress(cb, ldata, target.size, target.size);
        }
        break;
    }
    RARCloseArchive(h);
    if (rc != 0) { LocalFree(mem); return rc; }
    *out = mem;
    *out_size = (SIZE_T)target.size;
    return 0;
}

static int extract_entry(ARCHIVE* a, int idx, BYTE** out, SIZE_T* out_size,
                         PROGRESS_CB cb, long ldata)
{
    ENTRY& e = a->entries[idx];
    int rc = 0;
    int tries;

    if (e.size > 0x7FFFFFFFULL) {
        logf("entry %d too large: %I64u", e.id, e.size);
        return ERR_MEMORY;
    }

    /* After the user cancels, the prefetch burst that follows must not pop
     * another dialog for every page.  A later explicit open asks again. */
    if (pwd_cancel_recent(a))
        return ERR_UNKNOWN;

    for (tries = 0; tries < 3; tries++) {
        if (pwd_cancel_recent(a)) { rc = ERR_UNKNOWN; break; }
        if (a->solid)
            rc = extract_solid(a, idx, out, out_size, cb, ldata);
        else
            rc = extract_nonsolid(a, idx, out, out_size, cb, ldata);
        if (rc != ERR_PASSWORD || g_has_env_password)
            break;
        /* The password supplied during extraction was rejected. */
        close_seq(a);
        a->has_password = FALSE;
        a->password[0] = 0;
        if (!ask_password(a, tries > 0)) {
            pwd_cancel_set(a);
            rc = ERR_UNKNOWN;
            break;
        }
    }

    if (rc == 0 && e.encrypted && a->has_password && !a->is_mem)
        save_password(a->path_w.c_str(), a->password, a->remember_choice);
    return rc;
}

/* ------------------------------------------------------------------ */
/* archive context                                                     */
/* ------------------------------------------------------------------ */

static void free_archive(ARCHIVE* a)
{
    ARCHIVE** p;
    if (!a) return;
    for (p = &g_archives; *p; p = &(*p)->next)
        if (*p == a) { *p = a->next; break; }
    close_seq(a);
    DeleteCriticalSection(&a->cs);
    delete a;
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
        if (!src) { *err = TRUE; return NULL; }
        if (!resolve_ansi_path(src, wpath, MAX_PATH * 2)) {
            if (!MultiByteToWideChar(CP_ACP, 0, src, -1, wpath, MAX_PATH * 2)) {
                *err = TRUE; return NULL;
            }
        }
        _snprintf(key, sizeof(key), "P:%016I64x", fnv1a_wstr(wpath));
    }

    for (a = g_archives; a; a = a->next)
        if (!strcmp(a->key, key)) return a;

    a = new (std::nothrow) ARCHIVE();
    if (!a) { *err = TRUE; return NULL; }
    strcpy_s(a->key, sizeof(a->key), key);
    InitializeCriticalSection(&a->cs);
    a->is_mem = mem;
    a->remember_choice = g_remember_password;
    a->seq_h = NULL;
    a->seq_next = 0;
    a->sink = NULL;

    if (mem) {
        WCHAR temp[MAX_PATH], tpath[MAX_PATH];
        char hash[24];
        ULONGLONG mh;
        HANDLE h;
        DWORD wr = 0;
        if (!mem_size_valid || len <= 0) { *err = TRUE; free_archive(a); return NULL; }
        mh = mem_sample_hash((const BYTE*)src, len);
        _snprintf(hash, sizeof(hash), "%016I64x", mh);
        if (!GetTempPathW(MAX_PATH, temp)) { *err = TRUE; free_archive(a); return NULL; }
        _snwprintf(tpath, MAX_PATH, L"%sMangaMeeya_rar", temp);
        CreateDirectoryW(tpath, NULL);
        _snwprintf(tpath, MAX_PATH, L"%sMangaMeeya_rar\\in_%hs.rar", temp, hash);
        h = CreateFileW(tpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { *err = TRUE; free_archive(a); return NULL; }
        WriteFile(h, src, (DWORD)len, &wr, NULL);
        CloseHandle(h);
        a->path_w = tpath;
        a->cache_hash = hash;
    } else {
        a->path_w = wpath;
        {
            char hash[24];
            hash_path_cache(wpath, hash);
            a->cache_hash = hash;
        }
    }
    {
        WCHAR temp[MAX_PATH], wh[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, temp)) { *err = TRUE; free_archive(a); return NULL; }
        a->cache_dir = temp;
        a->cache_dir += L"MangaMeeya_rar\\";
        wh[0] = 0;
        if (a->cache_hash.size() < MAX_PATH)
            MultiByteToWideChar(CP_ACP, 0, a->cache_hash.c_str(), -1, wh, MAX_PATH);
        a->cache_dir += wh;
    }

    if (g_has_env_password) {
        wcsncpy(a->password, g_env_password, 511);
        a->password[511] = 0;
        a->has_password = TRUE;
    } else if (!a->is_mem && g_remember_password) {
        if (load_password(a->path_w.c_str(), a->password, 512))
            a->has_password = TRUE;
    }

    if (list_archive(a) != 0) {
        *err = TRUE;
        free_archive(a);
        return NULL;
    }
    if (!a->is_mem && a->has_password && (a->enc_headers || a->any_encrypted))
        save_password(a->path_w.c_str(), a->password, a->remember_choice);

    a->next = g_archives;
    g_archives = a;
    cache_cleanup(a->cache_dir.c_str(), a->path_w.c_str());
    return a;
}

/* ------------------------------------------------------------------ */
/* Susie 00AM exports                                                  */
/* ------------------------------------------------------------------ */

static int copy_info(const char* s, LPSTR buf, int buflen)
{
    int n;
    if (!buf || buflen <= 0) return 0;
    n = (int)strlen(s);
    if (n >= buflen) n = buflen - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return n;
}

extern "C" __declspec(dllexport) int WINAPI GetPluginInfo(int infono, LPSTR buf, int buflen)
{
    switch (infono) {
    case 0: return copy_info("00AM", buf, buflen);
    case 1: return copy_info("RAR Archive Plug-in v1.0 (UnRAR 7.2.2)", buf, buflen);
    case 2: return copy_info("*.RAR;*.CBR", buf, buflen);
    case 3: return copy_info("RAR Archive", buf, buflen);
    default: return 0;
    }
}

static BOOL rar_sig(const BYTE* p)
{
    return p[0] == 0x52 && p[1] == 0x61 && p[2] == 0x72 && p[3] == 0x21 &&
           p[4] == 0x1A && p[5] == 0x07 &&
           (p[6] == 0x00 || (p[6] == 0x01 && p[7] == 0x00));
}

extern "C" __declspec(dllexport) int WINAPI IsSupported(LPSTR filename, DWORD dw)
{
    BYTE hdr[8] = {0};
    if (HIWORD(dw)) {
        memcpy(hdr, (const void*)dw, 8);
    } else {
        HANDLE h = (HANDLE)dw;
        DWORD got = 0, pos;
        if (!h || h == INVALID_HANDLE_VALUE) return 0;
        pos = SetFilePointer(h, 0, NULL, FILE_CURRENT);
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        if (!ReadFile(h, hdr, 8, &got, NULL) || got < 8) {
            SetFilePointer(h, pos, NULL, FILE_BEGIN);
            return 0;
        }
        SetFilePointer(h, pos, NULL, FILE_BEGIN);
        (void)filename;
    }
    return rar_sig(hdr) ? 1 : 0;
}

extern "C" __declspec(dllexport) int WINAPI GetArchiveInfo(LPSTR buf, long len, UINT flag, HLOCAL* lphInf)
{
    ARCHIVE* a;
    BOOL err;
    HLOCAL h;
    FILEINFO* fi;
    int i;

    if (!lphInf) return ERR_INTERNAL;
    *lphInf = NULL;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = TRUE; }
    EnterCriticalSection(&g_lock);
    a = get_archive(buf, len, flag, TRUE, &err);
    if (a) a->pwd_cancel = FALSE;   /* this is a (re)open: ask again */
    if (!a || err || a->entries.empty()) {
        LeaveCriticalSection(&g_lock);
        return ERR_UNKNOWN;
    }
    h = LocalAlloc(LMEM_FIXED, sizeof(FILEINFO) * (a->entries.size() + 1));
    if (!h) { LeaveCriticalSection(&g_lock); return ERR_MEMORY; }
    fi = (FILEINFO*)h;
    ZeroMemory(fi, sizeof(FILEINFO) * (a->entries.size() + 1));
    for (i = 0; i < (int)a->entries.size(); i++) {
        ENTRY& e = a->entries[i];
        const char* name = e.utf8.c_str();
        const char* slash1 = strrchr(name, '/');
        const char* slash2 = strrchr(name, '\\');
        const char* slash = slash1 > slash2 ? slash1 : slash2;
        int plen = slash ? (int)(slash - name) : 0;
        const char* leaf = slash ? slash + 1 : name;
        if (plen > 199) plen = 199;
        _snprintf((char*)fi[i].method, 8, "RAR");
        fi[i].position = e.id;
        fi[i].compsize = e.packsize > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (unsigned long)e.packsize;
        fi[i].filesize = e.size > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (unsigned long)e.size;
        fi[i].timestamp = e.mtime;
        if (plen > 0) { memcpy(fi[i].path, name, plen); fi[i].path[plen] = 0; }
        lstrcpynA(fi[i].filename, leaf, 200);
        fi[i].crc = e.crc;
    }
    *lphInf = h;
    LeaveCriticalSection(&g_lock);
    return 0;
}

static BOOL entry_name_eq(ENTRY* e, const char* name)
{
    if (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE,
                       e->utf8.c_str(), -1, name, -1) == CSTR_EQUAL)
        return TRUE;
    if (CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE,
                       e->ansi.c_str(), -1, name, -1) == CSTR_EQUAL)
        return TRUE;
    return FALSE;
}

extern "C" __declspec(dllexport) int WINAPI GetFileInfo(LPSTR buf, long len, LPSTR filename,
                                                        UINT flag, FILEINFO* info)
{
    ARCHIVE* a;
    BOOL err;
    int i;
    if (!info || !filename) return ERR_INTERNAL;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = TRUE; }
    EnterCriticalSection(&g_lock);
    a = get_archive(buf, len, flag, TRUE, &err);
    if (!a || err) { LeaveCriticalSection(&g_lock); return ERR_UNKNOWN; }
    for (i = 0; i < (int)a->entries.size(); i++) {
        ENTRY& e = a->entries[i];
        if (entry_name_eq(&e, filename)) {
            ZeroMemory(info, sizeof(*info));
            _snprintf((char*)info->method, 8, "RAR");
            info->position = e.id;
            info->compsize = e.packsize > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (unsigned long)e.packsize;
            info->filesize = e.size > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (unsigned long)e.size;
            info->timestamp = e.mtime;
            lstrcpynA(info->filename, filename, 200);
            info->crc = e.crc;
            LeaveCriticalSection(&g_lock);
            return 0;
        }
    }
    LeaveCriticalSection(&g_lock);
    return ERR_UNKNOWN;
}

extern "C" __declspec(dllexport) int WINAPI GetFile(LPSTR src, long len, LPSTR dest,
                                                    UINT flag, FARPROC cb, long lData)
{
    ARCHIVE* a;
    BOOL err;
    ENTRY* e = NULL;
    BYTE* data = NULL;
    SIZE_T size = 0;
    int rc;
    int idx = -1;
    int i;
    PROGRESS_CB pcb = (PROGRESS_CB)cb;

    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = TRUE; }
    EnterCriticalSection(&g_lock);
    a = get_archive(src, len, flag, FALSE, &err);
    if (!a || err) { LeaveCriticalSection(&g_lock); return ERR_UNKNOWN; }
    for (i = 0; i < (int)a->entries.size(); i++)
        if (a->entries[i].id == (int)len) { e = &a->entries[i]; idx = i; break; }
    LeaveCriticalSection(&g_lock);
    if (!e) { logf("GetFile: no id %ld", len); return ERR_UNKNOWN; }

    EnterCriticalSection(&a->cs);
    rc = extract_entry(a, idx, &data, &size, pcb, lData);
    LeaveCriticalSection(&a->cs);

    if (rc != 0) {
        logf("GetFile id=%d failed rc=%d", e->id, rc);
        return rc;
    }
    logf("GetFile id=%d ok size=%I64u flag=%08x", e->id, (ULONGLONG)size, flag);
    if ((flag & 0x700) == 0x100) {
        if (!dest) { LocalFree(data); return ERR_INTERNAL; }
        *(HLOCAL*)dest = (HLOCAL)data;
        return 0;
    }
    if (!dest) { LocalFree(data); return ERR_INTERNAL; }
    {
        WCHAR wdest[MAX_PATH * 2], wpath[MAX_PATH * 2];
        const WCHAR* leaf = e->wname.c_str();
        const WCHAR* q;
        HANDLE h;
        DWORD wr = 0;
        for (q = e->wname.c_str(); *q; q++)
            if (*q == L'\\' || *q == L'/') leaf = q + 1;
        if (!MultiByteToWideChar(CP_ACP, 0, dest, -1, wdest, MAX_PATH * 2)) {
            LocalFree(data);
            return ERR_READ;
        }
        _snwprintf(wpath, MAX_PATH * 2, L"%s\\%s", wdest, leaf);
        h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { LocalFree(data); return ERR_READ; }
        if (size) WriteFile(h, data, (DWORD)size, &wr, NULL);
        CloseHandle(h);
        LocalFree(data);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

static void read_config(void)
{
    WCHAR dir[MAX_PATH], ini[MAX_PATH * 2];
    WCHAR appini[MAX_PATH * 2];
    get_plugin_dir(dir, MAX_PATH);
    _snwprintf(ini, MAX_PATH * 2, L"%srar.ini", dir);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(appini, MAX_PATH * 2, L"%sMangaMeeya.ini", dir);
        if (GetFileAttributesW(appini) != INVALID_FILE_ATTRIBUTES)
            wcscpy(ini, appini);
        else
            ini[0] = 0;
    }
    if (ini[0]) {
        int n = GetPrivateProfileIntW(L"rar", L"CacheMaxMB", -1, ini);
        if (n >= 0) g_cache_max_mb = n;
        n = GetPrivateProfileIntW(L"rar", L"Log", -1, ini);
        if (n >= 0) g_log_on = n != 0;
        n = GetPrivateProfileIntW(L"rar", L"RememberPassword", -1, ini);
        if (n >= 0) g_remember_password = n != 0;
    }
}

BOOL WINAPI DllMain(HMODULE hm, DWORD reason, LPVOID p)
{
    (void)p;
    if (reason == DLL_PROCESS_ATTACH) {
        char v[64];
        DWORD n;
        g_self = hm;
        DisableThreadLibraryCalls(hm);
        InitializeCriticalSection(&g_pwd_lock);
        read_config();
        load_remember_pref();
        /* Environment variables take precedence over rar.ini/MangaMeeya.ini. */
        n = GetEnvironmentVariableA("MMRAR_LOG", v, sizeof(v));
        if (n > 0) g_log_on = (v[0] == '1');
        n = GetEnvironmentVariableA("MMRAR_CACHE_MAX_MB", v, sizeof(v));
        if (n > 0) g_cache_max_mb = atoi(v);
        n = GetEnvironmentVariableA("MMRAR_REMEMBER_PASSWORD", v, sizeof(v));
        if (n > 0 && v[0] == '0') g_remember_password = FALSE;
        n = GetEnvironmentVariableW(L"MMRAR_PASSWORD", g_env_password, 512);
        if (n > 0) g_has_env_password = TRUE;
        logf("--- rar.spi attached (UnRAR 7.2.2) log=%d ---", g_log_on);
    } else if (reason == DLL_PROCESS_DETACH) {
        ARCHIVE* a = g_archives;
        while (a) {
            ARCHIVE* n = a->next;
            close_seq(a);
            a = n;
        }
        g_archives = NULL;
    }
    return TRUE;
}
