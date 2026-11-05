/*
 * mmunicode.dll - ANSI -> Unicode filename compatibility shim for MangaMeeya
 *
 * Background
 * ----------
 * MangaMeeya is a pure ANSI (A-suffix) Win32 program.  When the active ANSI
 * code page cannot represent a file name character (for example U+30FB
 * KATAKANA MIDDLE DOT "・" under CP936 / GBK), every A API receives '?'
 * instead of the character.  CreateFileA then fails with
 * ERROR_INVALID_NAME (123), so books whose path contains such characters
 * cannot be opened even though the file exists.
 *
 * How it works
 * ------------
 * The DLL is loaded through an import added to MangaMeeya.exe (see
 * ../patch_mangameeya_unicode.py).  In DllMain it patches the IAT of the main
 * executable, and LdrRegisterDllNotification does the same for arc.dll and
 * the Susie image plugins when they are loaded.  The hooked A APIs first call
 * the original function.  If it fails and the path contains '?', the shim
 * reconstructs the real Unicode path by walking the path component by
 * component: for each lossy component it enumerates the parent directory and
 * compares the ANSI (lossy) form of every entry with the requested name, then
 * retries through the corresponding W API.  No cache or persistent state is
 * kept and nothing outside the process is modified.
 *
 * Because MangaMeeya's internal strings remain ANSI, the UI still displays
 * '?' for these characters; only file access is fixed.
 *
 * Build (32-bit; the build needs an x86 toolchain):
 *   cl /nologo /LD /MT /O2 /W0 mmunicode.c /link /OUT:mmunicode.dll
 * The result imports only KERNEL32.dll (static CRT), so it can be copied
 * next to MangaMeeya.exe without any runtime.
 *
 * Set the environment variable MMU_LOG=1 to write a diagnostic log to
 * %TEMP%\mmu.log.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#ifndef IMAGE_ORDINAL_FLAG
#define IMAGE_ORDINAL_FLAG 0x80000000
#endif

static HMODULE g_self = NULL;
static BOOL    g_log  = FALSE;

static void logf(const char* fmt, ...);

/* ------------------------------------------------------------------ */
/* path helpers                                                        */
/* ------------------------------------------------------------------ */

static BOOL is_sep(char c) { return c == '\\' || c == '/'; }

/* Length in bytes of one character in the ANSI path.  Must be used whenever
   scanning an ANSI path: in DBCS code pages (e.g. CP936) a double-byte
   character's trail byte can be 0x5C ('\'), which is not a separator. */
static int ansi_charlen(const char* s)
{
    if (s[0] && s[1] && IsDBCSLeadByteEx(CP_ACP, (BYTE)(unsigned char)s[0]))
        return 2;
    return 1;
}

static BOOL path_has_lossy(const char* s)
{
    return s != NULL && strchr(s, '?') != NULL;
}

static BOOL name_matches_lossy(const char* comp, const WCHAR* wide)
{
    char buf[2048];
    int n;
    BOOL used = FALSE;

    n = WideCharToMultiByte(CP_ACP, 0, wide, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (n > 0 && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE,
                                comp, -1, buf, -1) == CSTR_EQUAL)
        return TRUE;

    n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, buf,
                            (int)sizeof(buf), NULL, &used);
    if (n > 0 && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE,
                                comp, -1, buf, -1) == CSTR_EQUAL)
        return TRUE;

    return FALSE;
}

/*
 * Reconstruct a real wide path from a lossy ANSI path.
 * Walks the path component by component from the root; each component that
 * cannot be opened directly is matched against the directory listing by its
 * lossy ANSI conversion.
 */
static BOOL resolve_ansi_path(const char* ansi, WCHAR* out, int cap)
{
    char full[MAX_PATH * 4];
    const char* p;
    DWORD n;

    WCHAR cur[2048];
    int curlen = 0;

    if (!ansi || !ansi[0] || cap < 8)
        return FALSE;

    n = GetFullPathNameA(ansi, (DWORD)sizeof(full), full, NULL);
    if (n > 0 && n < (DWORD)sizeof(full))
        p = full;
    else
        p = ansi;

    cur[0] = 0;

    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        cur[0] = p[0];
        cur[1] = L':';
        cur[2] = L'\\';
        cur[3] = 0;
        curlen = 3;
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
        s2 = q + 1;
        q = s2;
        while (*q && !is_sep(*q)) q += ansi_charlen(q);
        l2 = (int)(q - s2);
        if (l1 <= 0 || l2 <= 0) return FALSE;

        cur[0] = L'\\'; cur[1] = L'\\';
        w1 = MultiByteToWideChar(CP_ACP, 0, s1, l1, cur + 2, 1000);
        if (w1 <= 0) return FALSE;
        pos = 2 + w1;
        cur[pos++] = L'\\';
        w2 = MultiByteToWideChar(CP_ACP, 0, s2, l2, cur + pos, 1000);
        if (w2 <= 0) return FALSE;
        pos += w2;
        cur[pos++] = L'\\';
        cur[pos] = 0;
        curlen = pos;
        p = q;
        while (is_sep(*p)) p++;
    } else {
        return FALSE;
    }

    while (*p) {
        const char* e;
        int clen, base, wl, need, plen;
        char comp[1024];
        WCHAR wc[1024];
        WCHAR cand[2048];
        WCHAR pat[2048];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        BOOL found = FALSE;

        while (is_sep(*p)) p++;
        if (!*p) break;

        e = p;
        while (*e && !is_sep(*e)) e += ansi_charlen(e);
        clen = (int)(e - p);
        if (clen <= 0 || clen >= (int)sizeof(comp)) return FALSE;
        memcpy(comp, p, clen);
        comp[clen] = 0;
        p = e;

        base = curlen;
        if (base > 0 && cur[base - 1] != L'\\') {
            if (base + 1 >= (int)(sizeof(cur) / sizeof(cur[0]))) return FALSE;
            cur[base++] = L'\\';
            cur[base] = 0;
        }

        wl = MultiByteToWideChar(CP_ACP, 0, comp, -1, wc, 1024);
        if (wl <= 0) return FALSE;

        need = base + wl;
        if (need > (int)(sizeof(cand) / sizeof(cand[0]))) return FALSE;
        memcpy(cand, cur, base * sizeof(WCHAR));
        memcpy(cand + base, wc, wl * sizeof(WCHAR));

        if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
            memcpy(cur, cand, need * sizeof(WCHAR));
            curlen = base + wl - 1;
            continue;
        }

        plen = base;
        if (plen + 2 > (int)(sizeof(pat) / sizeof(pat[0]))) return FALSE;
        memcpy(pat, cur, plen * sizeof(WCHAR));
        pat[plen] = L'*';
        pat[plen + 1] = 0;

        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return FALSE;

        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (name_matches_lossy(comp, fd.cFileName)) {
                int l = (int)wcslen(fd.cFileName);
                if (base + l + 1 > (int)(sizeof(cur) / sizeof(cur[0])))
                    break;
                memcpy(cur, cand, base * sizeof(WCHAR));
                memcpy(cur + base, fd.cFileName, (l + 1) * sizeof(WCHAR));
                curlen = base + l;
                found = TRUE;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        if (!found)
            return FALSE;
    }

    if (curlen + 1 > cap) return FALSE;
    memcpy(out, cur, (curlen + 1) * sizeof(WCHAR));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* hooked functions                                                    */
/* ------------------------------------------------------------------ */

static void log_resolved(const char* api, const char* ansi, const WCHAR* wide)
{
    char a[MAX_PATH * 4];
    if (WideCharToMultiByte(CP_ACP, 0, wide, -1, a, (int)sizeof(a), NULL, NULL) <= 0)
        lstrcpynA(a, "?", sizeof(a));
    logf("%s resolved: %s -> %s%s", api, ansi, a,
         wcschr(wide, L'?') ? "  [still has ?]" : "");
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    HANDLE h;
    DWORD err;
    WCHAR w[4096];

    h = CreateFileA(name, access, share, sa, disp, flags, tmpl);
    err = GetLastError();

    if (h != INVALID_HANDLE_VALUE)
        return h;
    if (!path_has_lossy(name))
        return h;
    if (!resolve_ansi_path(name, w, 4096)) {
        logf("CreateFileA resolve FAILED: %s", name);
        SetLastError(err);
        return h;
    }
    log_resolved("CreateFileA", name, w);
    return CreateFileW(w, access, share, sa, disp, flags, tmpl);
}

static DWORD WINAPI Hook_GetFileAttributesA(LPCSTR name)
{
    DWORD r;
    DWORD err;
    WCHAR w[4096];

    r = GetFileAttributesA(name);
    err = GetLastError();

    if (r != INVALID_FILE_ATTRIBUTES)
        return r;
    if (!path_has_lossy(name))
        return r;
    if (!resolve_ansi_path(name, w, 4096)) {
        logf("GetFileAttributesA resolve FAILED: %s", name);
        SetLastError(err);
        return r;
    }
    log_resolved("GetFileAttributesA", name, w);
    return GetFileAttributesW(w);
}

static BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level,
                                             LPVOID info)
{
    BOOL r = GetFileAttributesExA(name, level, info);
    DWORD err;
    WCHAR w[4096];

    if (r)
        return r;
    err = GetLastError();
    if (!path_has_lossy(name))
        return r;
    if (!resolve_ansi_path(name, w, 4096)) {
        SetLastError(err);
        return r;
    }
    log_resolved("GetFileAttributesExA", name, w);
    return GetFileAttributesExW(w, level, info);
}

static BOOL WINAPI Hook_SetCurrentDirectoryA(LPCSTR name)
{
    BOOL r = SetCurrentDirectoryA(name);
    DWORD err;
    WCHAR w[4096];

    if (r)
        return r;
    err = GetLastError();
    if (!path_has_lossy(name))
        return r;
    if (!resolve_ansi_path(name, w, 4096)) {
        SetLastError(err);
        return r;
    }
    log_resolved("SetCurrentDirectoryA", name, w);
    return SetCurrentDirectoryW(w);
}

static BOOL WINAPI Hook_DeleteFileA(LPCSTR name)
{
    BOOL r = DeleteFileA(name);
    DWORD err;
    WCHAR w[4096];

    if (r)
        return r;
    err = GetLastError();
    if (!path_has_lossy(name))
        return r;
    if (!resolve_ansi_path(name, w, 4096)) {
        SetLastError(err);
        return r;
    }
    log_resolved("DeleteFileA", name, w);
    return DeleteFileW(w);
}

static BOOL WINAPI Hook_RemoveDirectoryA(LPCSTR name)
{
    BOOL r = RemoveDirectoryA(name);
    DWORD err;
    WCHAR w[4096];

    if (r)
        return r;
    err = GetLastError();
    if (!path_has_lossy(name))
        return r;
    if (!resolve_ansi_path(name, w, 4096)) {
        SetLastError(err);
        return r;
    }
    return RemoveDirectoryW(w);
}

static BOOL WINAPI Hook_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa)
{
    BOOL r = CreateDirectoryA(name, sa);
    DWORD err;
    WCHAR w[4096];

    if (r)
        return r;
    err = GetLastError();
    if (!path_has_lossy(name))
        return r;
    if (!resolve_ansi_path(name, w, 4096)) {
        SetLastError(err);
        return r;
    }
    return CreateDirectoryW(w, sa);
}

static BOOL to_wide_path(const char* a, WCHAR* w, int cap)
{
    if (!a) return FALSE;
    if (!path_has_lossy(a))
        return MultiByteToWideChar(CP_ACP, 0, a, -1, w, cap) > 0;
    return resolve_ansi_path(a, w, cap);
}

/* Build a wide search pattern.  A '?' in a directory component is a lossy
   character, not a wildcard, so that component is resolved first.  A '?' in
   the last component without '*' is treated as a literal lossy name. */
static BOOL pattern_to_wide(const char* pat, WCHAR* out, int cap)
{
    const char* leaf = pat;
    const char* q;
    int dirlen, wl, l;
    char dir[1024];
    WCHAR wleaf[1024];
    WCHAR wdir[2048];

    for (q = pat; *q; ) {
        if (is_sep(*q)) leaf = q + 1;
        q += ansi_charlen(q);
    }

    if (strchr(leaf, '?') && !strchr(leaf, '*'))
        return resolve_ansi_path(pat, out, cap);

    dirlen = (int)(leaf - pat);
    if (dirlen <= 0 || dirlen >= (int)sizeof(dir))
        return MultiByteToWideChar(CP_ACP, 0, pat, -1, out, cap) > 0;

    memcpy(dir, pat, dirlen);
    dir[dirlen] = 0;
    if (!path_has_lossy(dir))
        return MultiByteToWideChar(CP_ACP, 0, pat, -1, out, cap) > 0;

    if (!resolve_ansi_path(dir, wdir, 2048))
        return FALSE;
    wl = MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf, 1024);
    if (wl <= 0)
        return FALSE;
    l = (int)wcslen(wdir);
    if (l + 1 + wl > cap)
        return FALSE;
    memcpy(out, wdir, l * sizeof(WCHAR));
    out[l] = L'\\';
    memcpy(out + l + 1, wleaf, wl * sizeof(WCHAR));
    return TRUE;
}

static void find_data_w_to_a(const WIN32_FIND_DATAW* w, WIN32_FIND_DATAA* a)
{
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh;
    a->nFileSizeLow = w->nFileSizeLow;
    a->dwReserved0 = w->dwReserved0;
    a->dwReserved1 = w->dwReserved1;
    WideCharToMultiByte(CP_ACP, 0, w->cFileName, -1, a->cFileName, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, w->cAlternateFileName, -1,
                        a->cAlternateFileName, 14, NULL, NULL);
}

static HANDLE WINAPI Hook_FindFirstFileA(LPCSTR pat, LPWIN32_FIND_DATAA data)
{
    WCHAR wp[4096];
    WIN32_FIND_DATAW wd;
    HANDLE h;

    if (!pat || !data || !path_has_lossy(pat))
        return FindFirstFileA(pat, data);
    if (!pattern_to_wide(pat, wp, 4096))
        return FindFirstFileA(pat, data);

    h = FindFirstFileW(wp, &wd);
    if (h == INVALID_HANDLE_VALUE)
        return h;
    find_data_w_to_a(&wd, data);
    return h;
}

static BOOL WINAPI Hook_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data)
{
    WIN32_FIND_DATAW wd;
    if (!FindNextFileW(h, &wd))
        return FALSE;
    find_data_w_to_a(&wd, data);
    return TRUE;
}

static BOOL WINAPI Hook_MoveFileA(LPCSTR from, LPCSTR to)
{
    WCHAR wf[4096], wt[4096];
    BOOL r = MoveFileA(from, to);
    DWORD err;

    if (r)
        return r;
    err = GetLastError();
    if (!path_has_lossy(from) && !path_has_lossy(to)) {
        SetLastError(err);
        return r;
    }
    if (!to_wide_path(from, wf, 4096) || !to_wide_path(to, wt, 4096)) {
        SetLastError(err);
        return r;
    }
    return MoveFileW(wf, wt);
}

static BOOL WINAPI Hook_CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    WCHAR wf[4096], wt[4096];
    BOOL r = CopyFileA(from, to, fail_if_exists);
    DWORD err;

    if (r)
        return r;
    err = GetLastError();
    if (!path_has_lossy(from) && !path_has_lossy(to)) {
        SetLastError(err);
        return r;
    }
    if (!to_wide_path(from, wf, 4096) || !to_wide_path(to, wt, 4096)) {
        SetLastError(err);
        return r;
    }
    return CopyFileW(wf, wt, fail_if_exists);
}

static DWORD WINAPI Hook_GetShortPathNameA(LPCSTR name, LPSTR out, DWORD cch)
{
    WCHAR w[4096];
    WCHAR wo[4096];
    DWORD r;

    if (!path_has_lossy(name))
        return GetShortPathNameA(name, out, cch);
    if (!resolve_ansi_path(name, w, 4096))
        return GetShortPathNameA(name, out, cch);
    r = GetShortPathNameW(w, wo, 4096);
    if (r == 0 || r >= 4096)
        return r;
    r = (DWORD)WideCharToMultiByte(CP_ACP, 0, wo, -1, out, (int)cch, NULL, NULL);
    return r > 0 ? r - 1 : 0;
}

/* ------------------------------------------------------------------ */
/* IAT hooking                                                         */
/* ------------------------------------------------------------------ */

static void patch_module_iat(HMODULE mod);
static BOOL is_app_module(HMODULE mod);

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name)
{
    HMODULE m = LoadLibraryA(name);
    if (m && is_app_module(m))
        patch_module_iat(m);
    return m;
}

typedef struct {
    const char* name;
    void*       fn;
} HOOK_DESC;

static const HOOK_DESC g_hooks[] = {
    { "CreateFileA",           (void*)Hook_CreateFileA },
    { "GetFileAttributesA",    (void*)Hook_GetFileAttributesA },
    { "GetFileAttributesExA",  (void*)Hook_GetFileAttributesExA },
    { "SetCurrentDirectoryA",  (void*)Hook_SetCurrentDirectoryA },
    { "DeleteFileA",           (void*)Hook_DeleteFileA },
    { "RemoveDirectoryA",      (void*)Hook_RemoveDirectoryA },
    { "CreateDirectoryA",      (void*)Hook_CreateDirectoryA },
    { "MoveFileA",             (void*)Hook_MoveFileA },
    { "CopyFileA",             (void*)Hook_CopyFileA },
    { "GetShortPathNameA",     (void*)Hook_GetShortPathNameA },
    { "FindFirstFileA",        (void*)Hook_FindFirstFileA },
    { "FindNextFileA",         (void*)Hook_FindNextFileA },
    { "LoadLibraryA",          (void*)Hook_LoadLibraryA },
};

static void* lookup_hook(const char* name)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_hooks) / sizeof(g_hooks[0])); i++)
        if (strcmp(g_hooks[i].name, name) == 0)
            return g_hooks[i].fn;
    return NULL;
}

static BOOL is_app_module(HMODULE mod)
{
    WCHAR path[MAX_PATH];
    WCHAR windir[MAX_PATH];
    int i;

    if (mod == g_self)
        return FALSE;
    if (!GetModuleFileNameW(mod, path, MAX_PATH))
        return FALSE;
    if (!GetWindowsDirectoryW(windir, MAX_PATH))
        return TRUE;

    for (i = 0; windir[i]; i++) {
        WCHAR a = path[i], b = windir[i];
        if (a >= L'A' && a <= L'Z') a += 32;
        if (b >= L'A' && b <= L'Z') b += 32;
        if (a != b)
            return TRUE;
    }
    /* matched %WINDIR%; a system module only if that is the whole path or a dir prefix */
    if (path[i] == 0 || path[i] == L'\\')
        return FALSE;
    return TRUE;
}

static void patch_module_iat(HMODULE mod)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    IMAGE_DATA_DIRECTORY idd;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    int patched = 0;

    if (!base)
        return;
    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    idd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!idd.VirtualAddress)
        return;

    imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + idd.VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);

        for (; thunk->u1.AddressOfData; thunk++, iat++) {
            IMAGE_IMPORT_BY_NAME* ibn;
            void* repl;
            DWORD op;
            const char* name;

            if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME*)(base + thunk->u1.AddressOfData);
            name = (const char*)ibn->Name;
            repl = lookup_hook(name);
            if (!repl)
                continue;
            if (iat->u1.Function == (ULONG_PTR)repl)
                continue;
            if (VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &op)) {
                DWORD tmp;
                iat->u1.Function = (ULONG_PTR)repl;
                VirtualProtect(iat, sizeof(void*), op, &tmp);
                patched++;
            }
        }
    }
    logf("patch module %p: %d hook(s)", (void*)mod, patched);
}

/* ------------------------------------------------------------------ */
/* DLL load notification (catches arc.dll and image plugins)           */
/* ------------------------------------------------------------------ */

typedef struct _LDR_DLL_NOTIFICATION_DATA_MY {
    ULONG Flags;
    const void* FullDllName;
    const void* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA_MY;

typedef VOID (NTAPI *PLDR_DLL_NOTIFICATION_FUNCTION_MY)(
    ULONG NotificationReason, const LDR_DLL_NOTIFICATION_DATA_MY* NotificationData, PVOID Context);

typedef LONG (NTAPI *PLDR_REGISTER_NOTIFICATION)(
    ULONG Flags, PLDR_DLL_NOTIFICATION_FUNCTION_MY Function, PVOID Context, PVOID* Cookie);

static VOID NTAPI dll_notify(ULONG reason, const LDR_DLL_NOTIFICATION_DATA_MY* data,
                             PVOID context)
{
    (void)context;
    if (reason == 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */ && data && data->DllBase) {
        HMODULE m = (HMODULE)data->DllBase;
        if (is_app_module(m))
            patch_module_iat(m);
    }
}

static void install_notification(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PLDR_REGISTER_NOTIFICATION reg;
    PVOID cookie = NULL;

    if (!ntdll)
        return;
    reg = (PLDR_REGISTER_NOTIFICATION)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (!reg)
        return;
    reg(0, dll_notify, NULL, &cookie);
}

/* ------------------------------------------------------------------ */
/* logging / DllMain                                                   */
/* ------------------------------------------------------------------ */

static void logf(const char* fmt, ...)
{
    char buf[2048];
    char path[MAX_PATH];
    va_list ap;
    FILE* f;
    DWORD n;

    if (!g_log)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n + 8 >= MAX_PATH)
        return;
    lstrcatA(path, "mmu.log");
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

/* Imported by the patched EXE only so that the loader loads this DLL.
   The real work happens in DllMain. */
__declspec(dllexport) void MMUInit(void)
{
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char v[8];

        g_self = self;
        DisableThreadLibraryCalls(self);
        if (GetEnvironmentVariableA("MMU_LOG", v, sizeof(v)) > 0 && v[0] == '1')
            g_log = TRUE;

        logf("--- mmunicode attached self=%p ---", (void*)self);
        install_notification();
        patch_module_iat(GetModuleHandleA(NULL));
        logf("--- mmunicode install done ---");
    }
    return TRUE;
}
