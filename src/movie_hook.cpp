/*
 * movie_hook.cpp - HD movie playback for darkpatch.dll
 * =====================================================
 *
 * Plays a modern replacement video (via bundled ffplay.exe) for any
 * movie the user has supplied one for, completely bypassing the
 * original .TGQ codec's resolution ceiling. Movies with no replacement
 * play exactly as they always have, through the untouched original
 * code - this is purely additive.
 *
 * INTEGRATION NOTE: this file does its own byte-patching (VirtualProtect
 * + a hand-built trampoline) rather than using this project's own
 * HOOK_CALL/WRITE_JMP macros from header.h, since the real
 * detour::hookFunc is a bare 5-byte JMP write with no trampoline - the
 * manual trampoline here is necessary to still be able to call through
 * to the original function for movies with no HD replacement.
 *
 * REQUIRES: ffplay.exe present in the same folder as darkpatch.dll
 * (i.e. PRG_ENG).
 */

#include "header.h"
#include "detour.h"
#include <stdio.h>

using namespace darkomen;

/* Standard Windows idiom for getting the current module's own base
   address without needing the HINSTANCE passed to DllMain stored
   globally - the linker always provides this symbol pointing at the
   start of the current module in memory. */
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

/* OCR_NORMAL/OCR_APPSTARTING are normally only declared when
   OEMRESOURCE is defined before windows.h is included - header.h
   (shared across the whole project) doesn't define it, so both are
   defined here locally instead of touching that shared file. These
   are stable, long-documented Win32 constants (standard cursor
   resource IDs for SetSystemCursor), not something that changes
   between SDK versions. */
#ifndef OCR_NORMAL
#define OCR_NORMAL 32512
#endif
#ifndef OCR_APPSTARTING
#define OCR_APPSTARTING 32650
#endif

namespace movie_hook
{

/* ---- Fixed addresses from the analyzed EngRel.exe build ---- */
#define ADDR_RUNMOVIEPLAYBACKSTATEMACHINE 0x0042a090
#define ADDR_READNEXTQUEUEDKEYEVENT       0x00482060
#define PATCH_LEN 6
#define CONTINUE_ADDR (ADDR_RUNMOVIEPLAYBACKSTATEMACHINE + PATCH_LEN)

#define ADDR_MOVIESTREAM_CREATE 0x004913a0
#define PATCH_LEN2 9
#define CONTINUE_ADDR2 (ADDR_MOVIESTREAM_CREATE + PATCH_LEN2)

typedef int (__cdecl *RunMovieFn)(const char *);
typedef unsigned short (__cdecl *ReadKeyFn)(void);
typedef int *(__cdecl *MovieStreamCreateFn)(const char *, int, int *);

static RunMovieFn g_origRunMovie = NULL;
static MovieStreamCreateFn g_origMovieStreamCreate = NULL;
static const ReadKeyFn readNextQueuedKeyEvent = (ReadKeyFn)ADDR_READNEXTQUEUEDKEYEVENT;

static HANDLE g_ffplayProcess = NULL;
static DWORD g_ffplayPid = 0;
static HWND g_ffplayWindow = NULL;
static volatile BOOL g_ffplayShown = FALSE;
static HANDLE g_suppressThread = NULL;
static BOOL g_active = FALSE;
static char g_hdMoviesDir[MAX_PATH];
static char g_realMoviesDir[MAX_PATH];
static char g_ffplayPath[MAX_PATH];
static BOOL g_ffplayAvailable = FALSE;

static void *build_trampoline_generic(const unsigned char *originalBytes, int patchLen, int continueAddr)
{
    unsigned char *tramp = (unsigned char *)VirtualAlloc(
        NULL, patchLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return NULL;

    memcpy(tramp, originalBytes, patchLen);

    tramp[patchLen] = 0xE9;
    int rel = continueAddr - (int)(tramp + patchLen + 5);
    memcpy(tramp + patchLen + 1, &rel, 4);

    return tramp;
}

static HWND g_gameWindow = NULL;

static BOOL CALLBACK find_game_window_proc(HWND hwnd, LPARAM lParam)
{
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
        g_gameWindow = hwnd;
        return FALSE;
    }
    return TRUE;
}

static void reclaim_game_focus(void)
{
    g_gameWindow = NULL;
    EnumWindows(find_game_window_proc, 0);
    if (g_gameWindow) {
        ShowWindow(g_gameWindow, SW_RESTORE);
        SetForegroundWindow(g_gameWindow);
        SetActiveWindow(g_gameWindow);
    }
    Sleep(300);
}

static BOOL CALLBACK find_ffplay_window_proc(HWND hwnd, LPARAM lParam)
{
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == g_ffplayPid) {
        g_ffplayWindow = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* ffplay/SDL has its own built-in "hide cursor after inactivity"
   behavior (confirmed against FFmpeg's real source - a hardcoded
   CURSOR_HIDE_DELAY of exactly 1 second, not exposed as a runtime
   flag) - it shows the cursor whenever its window gains focus, then
   auto-hides it after that fixed delay. Rather than fight ffplay's own
   timer, replace the actual system cursor RESOURCE with a blank one
   for the duration of playback - this affects whatever's currently
   displaying the standard arrow cursor regardless of which process
   owns the window, then restores the real cursor via the standard
   "reset to registry defaults" call once playback ends. Safe even if
   ffplay is skipped or the movie ends unexpectedly, since both exit
   paths call this. */
static void hide_system_cursor(void)
{
    /* A 32x32 monochrome cursor needs each mask sized (width/8)*height =
       4*32 = 128 bytes, NOT 4 - a genuine under-allocation here previously
       left Windows reading ~124 bytes of uninitialized stack memory past
       the end of a 4-byte array, producing an undefined/garbage cursor
       image (which happened to render as a solid black square) instead
       of the intended fully transparent one.
       AND=1 (0xFF) + XOR=0 everywhere means "leave the destination pixel
       unchanged" for every pixel - i.e. genuinely invisible, not just
       black. */
    unsigned char andMask[128];
    unsigned char xorMask[128];
    memset(andMask, 0xFF, sizeof(andMask));
    memset(xorMask, 0x00, sizeof(xorMask));

    /* Deliberately only OCR_NORMAL, not OCR_APPSTARTING - tried blanking
       both, but real testing showed it made things worse: the app-
       starting spinner then appeared on EVERY movie call, including
       INTRO.TGQ's fallback to the original codec (no HD replacement, no
       CreateProcess at all) - not just on genuine HD-replacement
       launches. The user explicitly preferred the previous, simpler
       behavior (a brief spinner once at real process startup only) over
       that regression, so this reverts to OCR_NORMAL-only. */
    HCURSOR blankNormal = CreateCursor(NULL, 0, 0, 32, 32, andMask, xorMask);
    if (blankNormal)
        SetSystemCursor(blankNormal, OCR_NORMAL);
}

static void restore_system_cursor(void)
{
    SystemParametersInfoA(SPI_SETCURSORS, 0, NULL, 0);
}

static DWORD WINAPI suppress_flash_thread(LPVOID param)
{
    for (int i = 0; i < 2500; i++) {
        g_ffplayWindow = NULL;
        EnumWindows(find_ffplay_window_proc, 0);
        if (g_ffplayWindow) {
            hide_system_cursor();
            ShowWindow(g_ffplayWindow, SW_HIDE);
            ShowWindow(g_ffplayWindow, SW_SHOW);
            SetForegroundWindow(g_ffplayWindow);
            g_ffplayShown = TRUE;
            return 0;
        }
        Sleep(2);
    }
    detour::trace("suppress_flash_thread: never located ffplay's window "
                   "(pid=%lu) after 5 seconds of tight polling",
                   (unsigned long)g_ffplayPid);
    return 0;
}

static int *__cdecl fixpath_movieStream_Create(const char *path, int flag, int *config)
{
    const char *usePath = path;
    char fixedPath[MAX_PATH];

    if (path && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        const char *lastSlash1 = strrchr(path, '\\');
        const char *lastSlash2 = strrchr(path, '/');
        const char *filename = path;
        if (lastSlash1 && lastSlash1 + 1 > filename) filename = lastSlash1 + 1;
        if (lastSlash2 && lastSlash2 + 1 > filename) filename = lastSlash2 + 1;

        _snprintf(fixedPath, sizeof(fixedPath), "%s\\%s", g_realMoviesDir, filename);
        if (GetFileAttributesA(fixedPath) != INVALID_FILE_ATTRIBUTES) {
            detour::trace("Corrected stale movie path '%s' -> '%s'", path, fixedPath);
            usePath = fixedPath;
        }
    }

    return g_origMovieStreamCreate(usePath, flag, config);
}

static int __cdecl hook_runMoviePlaybackStateMachine(const char *movieName)
{
    *(int *)0x004d69e0 = 0;
    *(int *)0x004d69dc = 0;
    *(int *)0x004d69b8 = 0;

    if (!g_active) {
        const char *afterPrefix = movieName;
        const char *lastSlash = strrchr(movieName, '\\');
        const char *lastBracket = strrchr(movieName, ']');
        if (lastSlash && lastSlash >= afterPrefix) afterPrefix = lastSlash + 1;
        if (lastBracket && lastBracket >= afterPrefix) afterPrefix = lastBracket + 1;
        while (*afterPrefix == '\\') afterPrefix++;

        char baseName[MAX_PATH];
        strncpy(baseName, afterPrefix, sizeof(baseName) - 1);
        baseName[sizeof(baseName) - 1] = '\0';
        size_t baseLen = strlen(baseName);
        if (baseLen > 4 && _stricmp(baseName + baseLen - 4, ".tgq") == 0)
            baseName[baseLen - 4] = '\0';

        if (g_ffplayAvailable) {
            char hdPath[MAX_PATH];
            _snprintf(hdPath, sizeof(hdPath), "%s\\%s.mp4", g_hdMoviesDir, baseName);

            if (GetFileAttributesA(hdPath) != INVALID_FILE_ATTRIBUTES) {
                int screenW = GetSystemMetrics(SM_CXSCREEN);
                int screenH = GetSystemMetrics(SM_CYSCREEN);

                char cmdline[1600];
                _snprintf(cmdline, sizeof(cmdline),
                          "\"%s\" -autoexit -noborder -alwaysontop -left 0 -top 0 -x %d -y %d "
                          "-loglevel quiet -window_title \"Dark Omen\" \"%s\"",
                          g_ffplayPath, screenW, screenH, hdPath);

                STARTUPINFOA si;
                PROCESS_INFORMATION pi;
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));

                if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    g_ffplayProcess = pi.hProcess;
                    g_ffplayPid = pi.dwProcessId;
                    g_ffplayWindow = NULL;
                    g_ffplayShown = FALSE;
                    g_active = TRUE;
                    if (g_suppressThread)
                        CloseHandle(g_suppressThread);
                    g_suppressThread = CreateThread(NULL, 0, suppress_flash_thread, NULL, 0, NULL);
                    detour::trace("Playing HD replacement for '%s'", baseName);
                    return 0;
                }
                detour::trace("CreateProcess for ffplay failed, GetLastError=%lu - using original movie",
                         (unsigned long)GetLastError());
            }
        }

        return g_origRunMovie(movieName);
    }

    unsigned short key = readNextQueuedKeyEvent();
    unsigned char scancode = (unsigned char)((key >> 8) & 0xFF);
    if (scancode == 0x39 || scancode == 0x01) {
        TerminateProcess(g_ffplayProcess, 0);
        CloseHandle(g_ffplayProcess);
        g_ffplayProcess = NULL;
        g_active = FALSE;
        restore_system_cursor();
        reclaim_game_focus();
        return 1;
    }

    if (WaitForSingleObject(g_ffplayProcess, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_ffplayProcess);
        g_ffplayProcess = NULL;
        g_active = FALSE;
        restore_system_cursor();
        reclaim_game_focus();
        return 2;
    }

    return 0;
}

static BOOL patch_entry(void *targetAddr, int patchLen, int continueAddr, void *replacementFn, void **outOrig)
{
    unsigned char *target = (unsigned char *)targetAddr;
    unsigned char originalBytes[16];
    DWORD oldProtect;

    if (!VirtualProtect(target, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect))
        return FALSE;

    memcpy(originalBytes, target, patchLen);
    *outOrig = build_trampoline_generic(originalBytes, patchLen, continueAddr);
    if (!*outOrig) {
        VirtualProtect(target, patchLen, oldProtect, &oldProtect);
        return FALSE;
    }

    target[0] = 0xE9;
    int rel = (int)replacementFn - (int)(target + 5);
    memcpy(target + 1, &rel, 4);
    for (int i = 5; i < patchLen; i++)
        target[i] = 0x90;

    VirtualProtect(target, patchLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, patchLen);
    return TRUE;
}

void Load()
{
    /* Cheap insurance: if a previous session somehow crashed mid-
       playback with the system cursor still swapped to blank, this
       resets it back to the real one on every fresh load - harmless
       no-op if the cursor was already normal. */
    restore_system_cursor();

    char selfPath[MAX_PATH];
    GetModuleFileNameA((HINSTANCE)&__ImageBase, selfPath, MAX_PATH);
    char *lastSlash = strrchr(selfPath, '\\');
    if (lastSlash) *lastSlash = '\0';

    char parentPath[MAX_PATH];
    strncpy(parentPath, selfPath, sizeof(parentPath) - 1);
    parentPath[sizeof(parentPath) - 1] = '\0';
    char *parentSlash = strrchr(parentPath, '\\');
    if (parentSlash) *parentSlash = '\0';

    _snprintf(g_realMoviesDir, sizeof(g_realMoviesDir), "%s\\MOVIES", parentPath);
    _snprintf(g_hdMoviesDir, sizeof(g_hdMoviesDir), "%s\\HDMovies", parentPath);
    _snprintf(g_ffplayPath, sizeof(g_ffplayPath), "%s\\ffplay.exe", selfPath);
    g_ffplayAvailable = (GetFileAttributesA(g_ffplayPath) != INVALID_FILE_ATTRIBUTES);

    if (!g_ffplayAvailable) {
        detour::trace("ffplay.exe not found at '%s' - HD movie playback disabled, "
                 "original movies unaffected", g_ffplayPath);
    }

    if (!patch_entry((void *)ADDR_RUNMOVIEPLAYBACKSTATEMACHINE, PATCH_LEN, CONTINUE_ADDR,
                      (void *)&hook_runMoviePlaybackStateMachine, (void **)&g_origRunMovie)) {
        detour::trace("Failed to install movie playback hook");
        return;
    }

    if (!patch_entry((void *)ADDR_MOVIESTREAM_CREATE, PATCH_LEN2, CONTINUE_ADDR2,
                      (void *)&fixpath_movieStream_Create, (void **)&g_origMovieStreamCreate)) {
        detour::trace("Failed to install movie path-fix hook");
        return;
    }

    detour::trace("Movie hook installed OK. HD movies folder: %s (drop <moviename>.mp4 files here, "
             "e.g. eng.mp4 for the startup movie)", g_hdMoviesDir);
}

void Unload()
{
    // todo:
}

}
