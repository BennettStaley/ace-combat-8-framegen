/*
 * Self-check for the launcher. Builds a fake game install under %TEMP% and asserts the
 * safety invariant: after any mix of toggle, arm, crash and disarm, the install and the
 * user's config are byte-for-byte what they were, with no mod DLL left enabled.
 *
 * Usage: ac8tweaks_selftest [path to the built ac8tweaks.exe]
 * With the path given, the shipped binary itself is also driven end to end.
 *
 * A copy of this program named AceCombat8.exe plays the game in the session tests.
 */
#define AC8_NO_MAIN
#include "main.c"
#include <shellapi.h>
#include <shlobj.h>

#define OFFICIAL   "OFFICIAL-BOOTSTRAPPER-V1"
#define USER_INI   "USER-ORIGINAL"
#define TEMPLATE   "TEMPLATE"
#define GAME_OK    42

static int g_failed;
#define CHECK(c) do { if (!(c)) { g_failed++; wprintf(L"    FAIL line %d: %hs\n", __LINE__, #c); } } while (0)

static wchar_t g_tmp[PATHLEN];  /* scratch folder for this run */
static wchar_t g_case[PATHLEN]; /* fake install of the current test */
static wchar_t g_before[1 << 15], g_after[1 << 15];

static void put(const wchar_t *dir, const wchar_t *name, const char *text, DWORD attrs)
{
    wchar_t path[PATHLEN];
    FILE *f;
    join(path, dir, name);
    f = _wfopen(path, L"wb");
    if (!f) { CHECK(!"could not write test file"); return; }
    fputs(text, f);
    fclose(f);
    if (attrs) SetFileAttributesW(path, attrs);
}

static BOOL has(const wchar_t *dir, const wchar_t *name)
{
    wchar_t path[PATHLEN];
    join(path, dir, name);
    return exists(path);
}

static BOOL is(const wchar_t *dir, const wchar_t *name, const char *text)
{
    wchar_t path[PATHLEN];
    char buf[256] = { 0 };
    FILE *f;
    join(path, dir, name);
    f = _wfopen(path, L"rb");
    if (!f) return FALSE;
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return strcmp(buf, text) == 0;
}

static BOOL is_ours(const wchar_t *dir, const wchar_t *name)
{
    wchar_t path[PATHLEN];
    join(path, dir, name);
    return file_has_marker(path);
}

static unsigned hash_file(const wchar_t *path)
{
    unsigned h = 2166136261u;
    FILE *f = _wfopen(path, L"rb");
    int c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) h = (h ^ (unsigned)c) * 16777619u;
    fclose(f);
    return h;
}

/* One line per file: relative path, read-only flag, size, content hash. Logs are not part of the invariant. */
static void snapshot(const wchar_t *dir, const wchar_t *rel, wchar_t *out, size_t cap)
{
    wchar_t pat[PATHLEN], path[PATHLEN], sub[PATHLEN], line[PATHLEN + 64];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    join(pat, dir, L"*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !wcscmp(fd.cFileName, LOG_FILE) || !wcscmp(fd.cFileName, DISPLAY_FILE)) continue;
        join(path, dir, fd.cFileName);
        swprintf(sub, PATHLEN, L"%ls/%ls", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            snapshot(path, sub, out, cap);
            continue;
        }
        swprintf(line, ARRAYSIZE(line), L"%ls ro=%d size=%lu hash=%08x\n", sub,
                 !!(fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY), fd.nFileSizeLow, hash_file(path));
        wcsncat(out, line, cap - wcslen(out) - 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void snap(wchar_t *out)
{
    out[0] = 0;
    snapshot(g_case, L"", out, ARRAYSIZE(g_before));
}

static void check_unchanged(void)
{
    snap(g_after);
    if (wcscmp(g_before, g_after) != 0) {
        g_failed++;
        wprintf(L"    FAIL install differs from its starting state\n--- before\n%ls--- after\n%ls", g_before, g_after);
    }
}

/* Fake install: official bootstrapper, game exe, two disabled proxies, one DLL the game ships, mod folder. */
static void sandbox(Paths *p, BOOL user_has_engine_ini)
{
    static int n;
    wchar_t local[PATHLEN];
    memset(p, 0, sizeof *p);
    swprintf(g_case, PATHLEN, L"%ls\\case%d", g_tmp, ++n);
    GetModuleFileNameW(NULL, p->self, PATHLEN);
    join(p->root, g_case, L"root");
    join(p->bin, p->root, BIN_REL);
    join(p->mod, p->root, MOD_REL);
    join(local, g_case, L"local");
    join(p->cfg, local, CFG_REL);
    SHCreateDirectoryExW(NULL, p->bin, NULL);
    SHCreateDirectoryExW(NULL, p->mod, NULL);
    SHCreateDirectoryExW(NULL, p->cfg, NULL);
    put(p->root, BOOT, OFFICIAL, 0);
    put(p->bin, GAME_EXE, "GAME", 0);
    put(p->bin, L"dwmapi.dll" OFF_EXT, "UE4SS-PROXY", 0);
    put(p->bin, L"dxgi.dll" OFF_EXT, "OVERLAY-PROXY", 0);
    put(p->bin, L"tbb.dll", "SHIPPED-WITH-GAME", 0);
    put(p->mod, ENGINE_INI, TEMPLATE, 0);
    if (user_has_engine_ini) put(p->cfg, ENGINE_INI, USER_INI, FILE_ATTRIBUTE_READONLY);
    snap(g_before);
}

static void test_roundtrip(void)
{
    const wchar_t *why;
    Paths p;
    sandbox(&p, TRUE);

    CHECK(toggle(&p, &why) == NOW_OFFLINE);
    CHECK(is(p.root, BOOT_ORIG, OFFICIAL));
    CHECK(is_ours(p.root, BOOT));

    CHECK(arm(&p));
    CHECK(has(p.bin, L"dwmapi.dll") && !has(p.bin, L"dwmapi.dll" OFF_EXT));
    CHECK(has(p.bin, L"dxgi.dll") && !has(p.bin, L"dxgi.dll" OFF_EXT));
    CHECK(is(p.bin, L"tbb.dll", "SHIPPED-WITH-GAME"));
    CHECK(is(p.cfg, ENGINE_INI, TEMPLATE));
    CHECK(is(p.cfg, ENGINE_BAK, USER_INI));

    CHECK(disarm(&p));
    CHECK(is(p.cfg, ENGINE_INI, USER_INI));
    CHECK(toggle(&p, &why) == NOW_ONLINE);
    check_unchanged();
}

static void test_no_user_engine_ini(void)
{
    Paths p;
    sandbox(&p, FALSE);
    CHECK(arm(&p));
    CHECK(is(p.cfg, ENGINE_INI, TEMPLATE));
    CHECK(disarm(&p));
    CHECK(!has(p.cfg, ENGINE_INI));
    check_unchanged();
}

static void test_crash_mid_session_then_go_online(void)
{
    const wchar_t *why;
    Paths p;
    sandbox(&p, TRUE);
    CHECK(toggle(&p, &why) == NOW_OFFLINE);
    CHECK(arm(&p)); /* the launcher dies here and never cleans up */
    CHECK(toggle(&p, &why) == NOW_ONLINE);
    check_unchanged();
}

static void test_crash_before_any_change(void)
{
    Paths p;
    sandbox(&p, TRUE);
    put(p.mod, STATE_FILE, "engine=1\nproxy=dwmapi.dll\nproxy=dxgi.dll\n", 0); /* recorded, nothing done yet */
    CHECK(disarm(&p));
    CHECK(is(p.cfg, ENGINE_INI, USER_INI));
    check_unchanged();
}

static void test_crash_then_next_session(void)
{
    Paths p;
    sandbox(&p, TRUE);
    CHECK(arm(&p));
    CHECK(arm(&p)); /* next launch after a crash must not treat the template as the user's file */
    CHECK(is(p.cfg, ENGINE_BAK, USER_INI));
    CHECK(disarm(&p));
    check_unchanged();
}

static void test_steam_update_restores_bootstrapper(void)
{
    const wchar_t *why;
    Paths p;
    sandbox(&p, TRUE);
    CHECK(toggle(&p, &why) == NOW_OFFLINE);
    put(p.root, BOOT, "OFFICIAL-BOOTSTRAPPER-V2", 0); /* the update overwrites our stand-in */

    CHECK(toggle(&p, &why) == NOW_OFFLINE);
    CHECK(is_ours(p.root, BOOT));
    CHECK(is(p.root, BOOT_ORIG, "OFFICIAL-BOOTSTRAPPER-V2"));

    CHECK(toggle(&p, &why) == NOW_ONLINE);
    CHECK(is(p.root, BOOT, "OFFICIAL-BOOTSTRAPPER-V2"));
    CHECK(!has(p.root, BOOT_ORIG));
}

static void test_hand_installed_proxy_is_disabled(void)
{
    const wchar_t *why;
    Paths p;
    sandbox(&p, TRUE);

    put(p.bin, L"version.dll", "HAND-INSTALLED", 0);
    CHECK(arm(&p));
    CHECK(disarm(&p));
    CHECK(!has(p.bin, L"version.dll") && is(p.bin, L"version.dll" OFF_EXT, "HAND-INSTALLED"));

    CHECK(toggle(&p, &why) == NOW_OFFLINE);
    put(p.bin, L"winmm.dll", "HAND-INSTALLED", 0);
    CHECK(toggle(&p, &why) == NOW_ONLINE);
    CHECK(!has(p.bin, L"winmm.dll") && has(p.bin, L"winmm.dll" OFF_EXT));
}

static void test_go_online_fails_closed(void)
{
    const wchar_t *why;
    Paths p;
    sandbox(&p, TRUE);
    CHECK(toggle(&p, &why) == NOW_OFFLINE);
    put(p.bin, L"dwmapi.dll", "CANNOT-BE-DISABLED", 0); /* its .off twin already exists */
    CHECK(toggle(&p, &why) == TOGGLE_FAILED);
    CHECK(is_ours(p.root, BOOT)); /* EAC launcher not restored while a mod DLL is still live */
}

static void test_wrong_folder(void)
{
    const wchar_t *why;
    wchar_t game[PATHLEN];
    Paths p;
    sandbox(&p, TRUE);
    join(game, p.bin, GAME_EXE);
    DeleteFileW(game);
    snap(g_before);
    CHECK(toggle(&p, &why) == TOGGLE_FAILED);
    check_unchanged();
}

static void use_self_as_game(const Paths *p, wchar_t *result)
{
    wchar_t game[PATHLEN];
    join(game, p->bin, GAME_EXE);
    CHECK(CopyFileW(p->self, game, FALSE));
    snap(g_before);
    join(result, g_case, L"result.txt");
    SetEnvironmentVariableW(L"AC8TWEAKS_TEST_CFG", p->cfg);
    SetEnvironmentVariableW(L"AC8TWEAKS_TEST_RESULT", result);
}

static void test_session_waits_for_respawned_game(void)
{
    wchar_t result[PATHLEN];
    Paths p;
    sandbox(&p, TRUE);
    use_self_as_game(&p, result);

    CHECK(run_session(&p, L"-testarg") == GAME_OK);
    CHECK(is(g_case, L"result.txt", "ok"));
    DeleteFileW(result);
    check_unchanged();
}

static int display_hz(const wchar_t *mod)
{
    wchar_t path[PATHLEN], line[128];
    FILE *f;
    int hz = 0;
    join(path, mod, DISPLAY_FILE);
    f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return 0;
    while (fgetws(line, ARRAYSIZE(line), f))
        if (swscanf(line, L"RefreshHz=%d", &hz) == 1) break;
    fclose(f);
    return hz;
}

static int run(const wchar_t *exe, const wchar_t *args)
{
    wchar_t cmd[PATHLEN * 2];
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-2;
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls", exe, args);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -2;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* Drives the binary that ships: mode chosen by file name, paths derived from its location and LOCALAPPDATA. */
static void test_shipped_binary(const wchar_t *launcher)
{
    wchar_t result[PATHLEN], toggler[PATHLEN], boot[PATHLEN], local[PATHLEN], real_local[PATHLEN];
    Paths p;
    sandbox(&p, TRUE);
    join(toggler, p.root, L"ac8tweaks.exe");
    join(boot, p.root, BOOT);
    join(local, g_case, L"local");
    CHECK(CopyFileW(launcher, toggler, FALSE));
    use_self_as_game(&p, result);

    GetEnvironmentVariableW(L"LOCALAPPDATA", real_local, PATHLEN);
    SetEnvironmentVariableW(L"LOCALAPPDATA", local); /* keeps the real user config out of reach */

    CHECK(run(toggler, L"--quiet") == 0);
    CHECK(is(p.root, BOOT_ORIG, OFFICIAL));
    CHECK(is_ours(p.root, BOOT));

    CHECK(run(boot, L"-SaveToUserDir -testarg") == GAME_OK);
    CHECK(is(g_case, L"result.txt", "ok"));
    CHECK(!has(p.mod, STATE_FILE));
    CHECK(has(p.mod, LOG_FILE));
    CHECK(display_hz(p.mod) >= 24); /* the primary monitor is recorded before the game even opens a window */

    CHECK(run(toggler, L"--quiet") == 0);
    DeleteFileW(result);
    check_unchanged();

    SetEnvironmentVariableW(L"LOCALAPPDATA", real_local);
}

/* Stands in for the game. Verifies what a real session must provide, then re-spawns itself once like a relaunching game. */
static int fake_game(const wchar_t *self)
{
    wchar_t v[PATHLEN], bin[PATHLEN], cfg[PATHLEN], result[PATHLEN], cmd[PATHLEN * 2];
    const wchar_t *cl = GetCommandLineW();
    const char *verdict = "ok";
    BOOL child = wcsstr(cl, L"--child") != NULL;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    FILE *f;

    wcscpy(bin, self);
    *wcsrchr(bin, L'\\') = 0;
    GetEnvironmentVariableW(L"AC8TWEAKS_TEST_CFG", cfg, PATHLEN);
    GetEnvironmentVariableW(L"AC8TWEAKS_TEST_RESULT", result, PATHLEN);
    if (child) Sleep(500); /* outlive the first process */

    if (!GetEnvironmentVariableW(GUARD_ENV, v, PATHLEN) || wcscmp(v, L"1")) verdict = "guard variable missing";
    else if (!GetEnvironmentVariableW(L"SteamAppId", v, PATHLEN) || wcscmp(v, APP_ID)) verdict = "SteamAppId missing";
    else if (!wcsstr(cl, USER_DIR) || !wcsstr(cl, L"-testarg")) verdict = "arguments lost";
    else if (wcsstr(wcsstr(cl, USER_DIR) + 1, USER_DIR)) verdict = "-SaveToUserDir passed twice";
    else if (!GetEnvironmentVariableW(ROOT_ENV, v, PATHLEN) || !has(v, MOD_REL)) verdict = "root variable wrong";
    else if (!has(bin, L"dwmapi.dll") || has(bin, L"dwmapi.dll" OFF_EXT)) verdict = "proxy not armed";
    else if (!is(cfg, ENGINE_INI, TEMPLATE)) verdict = "Engine.ini not armed";

    if (child) { /* still armed half a second after the first process left means the launcher waited */
        f = _wfopen(result, L"wb");
        if (f) { fputs(verdict, f); fclose(f); }
        return 0;
    }
    if (strcmp(verdict, "ok")) return 1;
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" -SaveToUserDir -testarg --child", self);
    if (!CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 2;
    return GAME_OK;
}

static void rmtree(const wchar_t *dir)
{
    wchar_t from[PATHLEN + 1] = { 0 }; /* the API wants a double terminator */
    SHFILEOPSTRUCTW op = { 0 };
    wcscpy(from, dir);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

#define RUN(test) do { int before = g_failed; wprintf(L"  %hs\n", #test); test; if (g_failed == before) passed++; total++; } while (0)

int wmain(int argc, wchar_t **argv)
{
    wchar_t self[PATHLEN], temp[PATHLEN];
    int passed = 0, total = 0;

    GetModuleFileNameW(NULL, self, PATHLEN);
    if (_wcsicmp(wcsrchr(self, L'\\') + 1, GAME_EXE) == 0) return fake_game(self);

    GetTempPathW(PATHLEN, temp);
    swprintf(g_tmp, PATHLEN, L"%lsac8tweaks-selftest-%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(g_tmp, NULL);
    join(g_log, g_tmp, LOG_FILE);
    wprintf(L"ac8tweaks self-test, scratch folder %ls\n", g_tmp);

    RUN(test_roundtrip());
    RUN(test_no_user_engine_ini());
    RUN(test_crash_mid_session_then_go_online());
    RUN(test_crash_before_any_change());
    RUN(test_crash_then_next_session());
    RUN(test_steam_update_restores_bootstrapper());
    RUN(test_hand_installed_proxy_is_disabled());
    RUN(test_go_online_fails_closed());
    RUN(test_wrong_folder());
    RUN(test_session_waits_for_respawned_game());
    if (argc > 1) RUN(test_shipped_binary(argv[1]));
    else wprintf(L"  test_shipped_binary skipped, no launcher path given\n");

    wprintf(L"%d of %d tests passed\n", passed, total);
    if (g_failed) wprintf(L"scratch folder kept for inspection\n");
    else rmtree(g_tmp);
    return g_failed ? 1 : 0;
}
