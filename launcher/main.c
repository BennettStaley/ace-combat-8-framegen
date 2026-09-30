/*
 * ac8tweaks: offline launcher and anti-cheat toggler for ACE COMBAT 8.
 *
 * Mode is chosen by this program's own file name:
 *   start_protected_game.exe   launcher: arm the mod, start the game without EAC, wait, disarm
 *   any other name             toggler:  swap this program in or out as start_protected_game.exe
 *
 * Invariant: outside a running offline session no proxy DLL is enabled and the
 * user's Engine.ini is exactly what it was. A Steam update that restores the
 * official bootstrapper therefore always starts a clean, unmodded EAC session.
 *
 * Mechanism follows techiew/EldenRingEacToggler. The code is original.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PATHLEN     520
#define APP_ID      L"2288340"
#define GUARD_ENV   L"AC8TWEAKS_OFFLINE" /* mod code must refuse to run without this */
#define ROOT_ENV    L"AC8TWEAKS_ROOT"    /* where the mod finds settings.ini */
#define USER_DIR    L"-SaveToUserDir"
#define GAME_EXE    L"AceCombat8.exe"
#define BIN_REL     L"Game\\Binaries\\Win64"
#define MOD_REL     L"AC8Tweaks"
#define CFG_REL     L"BANDAI NAMCO Entertainment\\ACE COMBAT 8\\Saved\\Config\\Windows"
#define BOOT        L"start_protected_game.exe"
#define BOOT_ORIG   BOOT L".original"
#define ENGINE_INI  L"Engine.ini"
#define ENGINE_BAK  ENGINE_INI L".ac8tweaks-backup"
#define STATE_FILE  L"armed.state"
#define LOG_FILE    L"ac8tweaks.log"
#define OFF_EXT     L".off"

/* Present in every build of this program, never in the official bootstrapper. */
static const char MARKER[] = "AC8TWEAKS-LAUNCHER-MARKER-5d6d83ad";

/* System DLL names a mod loader can hijack when placed beside the game exe. The game ships none of them there. */
static const wchar_t *const PROXY_NAMES[] = {
    L"dwmapi.dll", L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"dinput8.dll", L"dsound.dll",
    L"winmm.dll", L"winhttp.dll", L"wininet.dll", L"version.dll", L"xinput1_4.dll",
};

enum { ENGINE_NO_ORIGINAL = 0, ENGINE_HAD_ORIGINAL = 1, ENGINE_SKIP = 2 };

typedef enum { NOW_OFFLINE, NOW_ONLINE, TOGGLE_FAILED } Toggle;

typedef struct {
    wchar_t self[PATHLEN]; /* this executable */
    wchar_t root[PATHLEN]; /* game root, holds start_protected_game.exe */
    wchar_t bin[PATHLEN];  /* folder of the game exe, where proxy DLLs live */
    wchar_t mod[PATHLEN];  /* AC8Tweaks folder: Engine.ini template, state, log */
    wchar_t cfg[PATHLEN];  /* folder of the user's Engine.ini */
} Paths;

static wchar_t g_log[PATHLEN];

static void join(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    swprintf(out, PATHLEN, L"%ls\\%ls", dir, name);
}

static void logw(const wchar_t *fmt, ...)
{
    FILE *f = g_log[0] ? _wfopen(g_log, L"a, ccs=UTF-8") : NULL;
    SYSTEMTIME t;
    va_list ap;
    if (!f) return;
    GetLocalTime(&t);
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

static BOOL contains_ci(const wchar_t *haystack, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    for (; *haystack; haystack++)
        if (_wcsnicmp(haystack, needle, n) == 0) return TRUE;
    return FALSE;
}

static BOOL exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL remove_file(const wchar_t *path)
{
    if (!exists(path)) return TRUE;
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL); /* read-only files cannot be deleted */
    if (DeleteFileW(path)) return TRUE;
    logw(L"delete failed (%lu): %ls", GetLastError(), path);
    return FALSE;
}

static BOOL rename_file(const wchar_t *from, const wchar_t *to, DWORD flags)
{
    if (MoveFileExW(from, to, flags)) return TRUE;
    logw(L"rename failed (%lu): %ls -> %ls", GetLastError(), from, to);
    return FALSE;
}

static BOOL copy_file(const wchar_t *from, const wchar_t *to)
{
    if (CopyFileW(from, to, TRUE)) return TRUE;
    logw(L"copy failed (%lu): %ls -> %ls", GetLastError(), from, to);
    return FALSE;
}

static BOOL file_has_marker(const wchar_t *path)
{
    const size_t n = sizeof MARKER - 1;
    BOOL found = FALSE;
    FILE *f = _wfopen(path, L"rb");
    long size;
    char *buf;
    size_t i;
    if (!f) return FALSE;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = size > 0 ? malloc((size_t)size) : NULL;
    if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size)
        for (i = 0; i + n <= (size_t)size && !found; i++)
            found = memcmp(buf + i, MARKER, n) == 0;
    free(buf);
    fclose(f);
    return found;
}

/* Flip one DLL between name and name.off. TRUE when it ends in the wanted state or does not exist at all. */
static BOOL set_proxy(const Paths *p, const wchar_t *name, BOOL enable)
{
    wchar_t on[PATHLEN], off[PATHLEN];
    const wchar_t *from, *to;
    join(on, p->bin, name);
    swprintf(off, PATHLEN, L"%ls" OFF_EXT, on);
    from = enable ? off : on;
    to = enable ? on : off;
    if (!exists(from)) return TRUE;
    if (exists(to)) {
        logw(L"%ls exists both enabled and disabled, leaving both alone", on);
        return FALSE;
    }
    return rename_file(from, to, 0);
}

static BOOL disable_known_proxies(const Paths *p)
{
    BOOL ok = TRUE;
    size_t i;
    for (i = 0; i < ARRAYSIZE(PROXY_NAMES); i++)
        ok = set_proxy(p, PROXY_NAMES[i], FALSE) && ok;
    return ok;
}

/* Enable or disable every proxy recorded in the state file and report the recorded Engine.ini mode. */
static BOOL apply_state(const Paths *p, BOOL enable, int *engine)
{
    wchar_t state[PATHLEN], line[PATHLEN];
    BOOL ok = TRUE;
    FILE *f;
    join(state, p->mod, STATE_FILE);
    *engine = ENGINE_SKIP;
    f = _wfopen(state, L"r, ccs=UTF-8");
    if (!f) return FALSE;
    while (fgetws(line, PATHLEN, f)) {
        line[wcscspn(line, L"\r\n")] = 0;
        if (!wcsncmp(line, L"engine=", 7)) *engine = _wtoi(line + 7);
        else if (!wcsncmp(line, L"proxy=", 6)) ok = set_proxy(p, line + 6, enable) && ok;
    }
    fclose(f);
    return ok;
}

/* Undo whatever the state file records. Safe to call in any partial state, any number of times. */
static BOOL disarm(const Paths *p)
{
    wchar_t state[PATHLEN], ini[PATHLEN], bak[PATHLEN];
    int engine;
    BOOL ok;
    join(state, p->mod, STATE_FILE);
    join(ini, p->cfg, ENGINE_INI);
    join(bak, p->cfg, ENGINE_BAK);
    if (!exists(state)) return TRUE;

    ok = apply_state(p, FALSE, &engine);
    /* No backup means the crash came before the original was moved, so Engine.ini is still the user's. */
    if (engine == ENGINE_HAD_ORIGINAL && exists(bak)) ok = remove_file(ini) && rename_file(bak, ini, 0) && ok;
    if (engine == ENGINE_NO_ORIGINAL) ok = remove_file(ini) && ok;
    if (ok) ok = remove_file(state);
    logw(ok ? L"disarmed" : L"disarm incomplete, state kept for the next run");
    return ok;
}

/* Enable the mod for one session. Every intended change is recorded before any is made. */
static BOOL arm(const Paths *p)
{
    wchar_t state[PATHLEN], ini[PATHLEN], bak[PATHLEN], tmpl[PATHLEN], path[PATHLEN];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    FILE *f;
    int engine;
    BOOL ok;
    size_t i;

    if (!disarm(p)) return FALSE; /* recover from a session that never cleaned up */

    join(state, p->mod, STATE_FILE);
    join(tmpl, p->mod, ENGINE_INI);
    join(ini, p->cfg, ENGINE_INI);
    join(bak, p->cfg, ENGINE_BAK);

    f = _wfopen(state, L"w, ccs=UTF-8");
    if (!f) {
        logw(L"no mod folder at %ls, starting the game unmodded", p->mod);
        return FALSE;
    }
    engine = !exists(tmpl) ? ENGINE_SKIP : (exists(ini) || exists(bak)) ? ENGINE_HAD_ORIGINAL : ENGINE_NO_ORIGINAL;
    fwprintf(f, L"engine=%d\n", engine);
    for (i = 0; i < ARRAYSIZE(PROXY_NAMES); i++) { /* proxies somebody enabled by hand become managed */
        join(path, p->bin, PROXY_NAMES[i]);
        if (exists(path)) fwprintf(f, L"proxy=%ls\n", PROXY_NAMES[i]);
    }
    join(path, p->bin, L"*" OFF_EXT);
    h = FindFirstFileW(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            fd.cFileName[wcslen(fd.cFileName) - wcslen(OFF_EXT)] = 0;
            fwprintf(f, L"proxy=%ls\n", fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    fclose(f);

    ok = apply_state(p, TRUE, &engine);
    if (engine != ENGINE_SKIP) {
        BOOL ini_ok = TRUE;
        /* A backup that already exists is the true original, so it is never overwritten. */
        if (exists(ini) && !exists(bak)) ini_ok = rename_file(ini, bak, 0);
        ini_ok = ini_ok && remove_file(ini) && copy_file(tmpl, ini);
        if (ini_ok) SetFileAttributesW(ini, FILE_ATTRIBUTE_READONLY); /* stops the game rewriting it */
        ok = ini_ok && ok;
    }
    logw(L"armed%ls, Engine.ini mode %d (0 none before, 1 backed up, 2 untouched)", ok ? L"" : L" with errors", engine);
    return ok;
}

static Toggle toggle(const Paths *p, const wchar_t **why)
{
    wchar_t boot[PATHLEN], orig[PATHLEN], game[PATHLEN];
    join(boot, p->root, BOOT);
    join(orig, p->root, BOOT_ORIG);
    join(game, p->bin, GAME_EXE);
    *why = L"";

    if (!exists(game) || !exists(boot)) {
        *why = L"Game files not found.\n\nPut this program in the game's root folder, next to start_protected_game.exe.";
        return TOGGLE_FAILED;
    }

    if (file_has_marker(boot)) { /* offline -> online. Fails closed: any doubt keeps the game offline. */
        if (!disarm(p) || !disable_known_proxies(p)) {
            *why = L"Could not disable a mod DLL, so the game stays in offline mode.\n\nSee " LOG_FILE L".";
            return TOGGLE_FAILED;
        }
        if (!exists(orig)) {
            *why = L"The backup of the official launcher is missing.\n\nUse Steam's \"Verify integrity of game files\" to restore it.";
            return TOGGLE_FAILED;
        }
        if (!remove_file(boot) || !rename_file(orig, boot, 0)) {
            *why = L"Could not restore the official launcher.\n\nSee " LOG_FILE L".";
            return TOGGLE_FAILED;
        }
        logw(L"toggled: online, official launcher restored");
        return NOW_ONLINE;
    }

    /* Official bootstrapper, fresh or put back by a Steam update. An older backup is superseded. */
    if (!rename_file(boot, orig, MOVEFILE_REPLACE_EXISTING)) {
        *why = L"Could not back up the official launcher.\n\nSee " LOG_FILE L".";
        return TOGGLE_FAILED;
    }
    if (!copy_file(p->self, boot)) {
        rename_file(orig, boot, 0);
        *why = L"Could not install the offline launcher.\n\nSee " LOG_FILE L".";
        return TOGGLE_FAILED;
    }
    logw(L"toggled: offline, stand-in launcher installed");
    return NOW_OFFLINE;
}

/* Arm, run the game without EAC, wait until it is fully gone, disarm. Returns the game's exit code, -1 if it never started. */
static int run_session(const Paths *p, const wchar_t *args)
{
    wchar_t game[PATHLEN], cmd[4096];
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT port;
    HANDLE job, iocp;
    DWORD code = 1, msg = 0;
    ULONG_PTR key;
    OVERLAPPED *ov;

    join(game, p->bin, GAME_EXE);
    arm(p);
    SetEnvironmentVariableW(L"SteamAppId", APP_ID);
    SetEnvironmentVariableW(GUARD_ENV, L"1");
    SetEnvironmentVariableW(ROOT_ENV, p->root);
    if (!args) args = L"";
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls %ls", game, contains_ci(args, USER_DIR) ? L"" : USER_DIR, args);
    logw(L"starting %ls", cmd);

    /* The job keeps us waiting for every process the game spawns, not just the first one. */
    job = CreateJobObjectW(NULL, NULL);
    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    port.CompletionKey = job;
    port.CompletionPort = iocp;
    SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &port, sizeof port);

    if (!CreateProcessW(game, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, p->bin, &si, &pi)) {
        logw(L"could not start the game (%lu)", GetLastError());
        disarm(p);
        return -1;
    }
    if (AssignProcessToJobObject(job, pi.hProcess)) {
        ResumeThread(pi.hThread);
        while (GetQueuedCompletionStatus(iocp, &msg, &key, &ov, INFINITE) && msg != JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {}
    } else {
        logw(L"job unavailable (%lu), waiting on the first process only", GetLastError());
        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(iocp);
    CloseHandle(job);
    logw(L"game exited with code %lu", code);
    disarm(p);
    return (int)code;
}

static BOOL init_paths(Paths *p)
{
    wchar_t local[PATHLEN], *slash;
    if (!GetModuleFileNameW(NULL, p->self, PATHLEN)) return FALSE;
    wcscpy(p->root, p->self);
    slash = wcsrchr(p->root, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    join(p->bin, p->root, BIN_REL);
    join(p->mod, p->root, MOD_REL);
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, PATHLEN)) return FALSE;
    join(p->cfg, local, CFG_REL);
    join(g_log, exists(p->mod) ? p->mod : p->root, LOG_FILE);
    return TRUE;
}

#ifndef AC8_NO_MAIN
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    const wchar_t *title = L"AC8 Tweaks";
    const wchar_t *why;
    Paths p;
    BOOL quiet = wcsstr(args, L"--quiet") != NULL;
    (void)inst;
    (void)prev;
    (void)show;

    if (!init_paths(&p)) return 1;

    if (_wcsicmp(wcsrchr(p.self, L'\\') + 1, BOOT) == 0) {
        int code = run_session(&p, args);
        if (code == -1)
            MessageBoxW(NULL, L"Could not start AceCombat8.exe.\n\nStart the game once through Steam with the official launcher, then switch back to offline mode.\n\nSee " LOG_FILE L".", title, MB_OK | MB_ICONERROR);
        return code;
    }

    switch (toggle(&p, &why)) {
    case NOW_OFFLINE:
        if (!quiet)
            MessageBoxW(NULL, L"Offline mode is ON.\n\nSteam's Play button now starts the game without Easy Anti-Cheat. Online modes will not work.\n\nRun this program again before playing online, and after every game update.", title, MB_OK | MB_ICONINFORMATION);
        return 0;
    case NOW_ONLINE:
        if (!quiet)
            MessageBoxW(NULL, L"Offline mode is OFF.\n\nThe official Easy Anti-Cheat launcher is restored and all mod DLLs are disabled.", title, MB_OK | MB_ICONINFORMATION);
        return 0;
    default:
        if (!quiet) MessageBoxW(NULL, why, title, MB_OK | MB_ICONERROR);
        return 1;
    }
}
#endif
