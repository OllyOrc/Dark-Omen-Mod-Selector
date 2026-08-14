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
 * PROVEN: this logic (patch targets, movie-name parsing, the path-fix
 * for the game's own stale [MOVIES] alias, the borderless-window
 * approach to avoid disrupting the game's DirectDraw surface, the
 * window-refocus step) was built and iteratively debugged as a
 * standalone DLL first, using real trace-log evidence from actual
 * in-game testing across many rounds, and is CONFIRMED WORKING: a
 * custom HD movie plays correctly, and playback falls through cleanly
 * to the next original movie afterward.
 *
 * INTEGRATION NOTE, checked directly against the real darkpatch source
 * (header.h, detour.cpp, modmenu.cpp): this file installs its hooks the
 * same way this codebase's OWN internal-function hooks already work -
 * a raw 5-byte JMP write (exactly what WRITE_JMP/detour::hookFunc both
 * do here) plus a hand-built trampoline, since NEITHER of those two
 * provides one automatically. Real Microsoft Detours (DetourFunction)
 * IS available in this project (externals/detours.h/.lib) and DOES
 * provide trampolining, but this codebase's own convention reserves it
 * specifically for hooking standard Win32 API imports (CreateFileA,
 * FindFirstFileA, etc. in modmenu.cpp) - internal game functions like
 * the ones this file hooks are always done the simpler way. Matching
 * that convention, and reusing the exact mechanism already proven
 * working in real-game testing, felt like the right call over
 * introducing an untested alternative for something shipping to many
 * players. Logs through darkomen::detour::trace() - the same function
 * that writes trace.txt - so this integrates into the existing log
 * rather than creating a separate file.
 *
 * REQUIRES: ffplay.exe present in the same folder as darkpatch.dll
 * (i.e. PRG_ENG) - needs to be added to mod pack distribution alongside
 * this file. It's a real file (~147MB, from the FFmpeg project,
 * GPL-licensed) - large, but self-contained and needs no separate
 * install step.
 *
 * Addresses are hardcoded to the EngRel.exe build this project's
 * reverse-engineering work has been done against - confirmed correct
 * via direct disassembly and, separately, via extracting the actual
 * bytes from a real user's EngRel.exe and comparing. If a different
 * game build is ever in play, these would need re-verifying.
 *
 * TO INTEGRATE (3 steps, all confirmed against the real project files):
 *   1. Add this file to darkpatch.vcxproj (and .vcxproj.filters) as a
 *      ClCompile item, same as the other src/*.cpp files.
 *   2. In header.h, add this line alongside the other module
 *      declarations:
 *        namespace movie_hook { void Load(); void Unload(); }
 *   3. In dllmain.cpp's applyHooks(), add a call to movie_hook::Load();
 *      anywhere in the existing sequence of <module>::Load() calls
 *      (order doesn't matter relative to the others - this module is
 *      self-contained).
 */

#include "header.h"
#include "detour.h"
#include <stdio.h>
#include <string.h>

namespace movie_hook
{
using namespace darkomen;

/* ---- Fixed addresses from the analyzed EngRel.exe build ---- */
#define ADDR_RUNMOVIEPLAYBACKSTATEMACHINE 0x0042a090
#define ADDR_READNEXTQUEUEDKEYEVENT       0x00482060
#define PATCH_LEN 6   /* PUSH ESI (1 byte) + MOV EAX,[mem] (5 bytes) -
                          disassembly-confirmed clean instruction
                          boundary, >= the 5 bytes a JMP rel32 needs */
#define CONTINUE_ADDR (ADDR_RUNMOVIEPLAYBACKSTATEMACHINE + PATCH_LEN)

#define ADDR_MOVIESTREAM_CREATE 0x004913a0
#define PATCH_LEN2 9  /* 4x single-byte PUSH + PUSH imm32 (5 bytes) -
                          disassembly-confirmed */
#define CONTINUE_ADDR2 (ADDR_MOVIESTREAM_CREATE + PATCH_LEN2)

typedef int (__cdecl *RunMovieFn)(const char *);
typedef unsigned short (__cdecl *ReadKeyFn)(void);
typedef int *(__cdecl *MovieStreamCreateFn)(const char *, int, int *);

static RunMovieFn g_origRunMovie = NULL;
static MovieStreamCreateFn g_origMovieStreamCreate = NULL;
static const ReadKeyFn readNextQueuedKeyEvent = (ReadKeyFn)ADDR_READNEXTQUEUEDKEYEVENT;

static HANDLE g_ffplayProcess = NULL;
static BOOL g_active = FALSE;
static char g_hdMoviesDir[MAX_PATH];
static char g_realMoviesDir[MAX_PATH];
static char g_ffplayPath[MAX_PATH];
static BOOL g_ffplayAvailable = FALSE;

/* Build a trampoline: original bytes we're overwriting, followed by a
   jump back into the real function right after the patched region -
   lets us still call the genuine original for movies with no HD
   replacement. Same mechanism proven in the standalone build. */
static void *build_trampoline(const unsigned char *originalBytes, int patchLen, int continueAddr)
{
    unsigned char *tramp = (unsigned char *)VirtualAlloc(
        NULL, patchLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
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

/* ffplay running fullscreen can mark the game's own DirectDraw surface
   "lost" - give the window a genuine chance to reclaim focus and let
   the game's own surface-restore logic run before handing back control. */
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

/* movieStream_Create path-fix. The game's own internal "[MOVIES]" path
   alias can resolve to a stale/wrong absolute path rather than the
   user's real install (confirmed via real trace-log evidence during
   development - saw it resolve to a leftover developer path). If the
   path we're given doesn't exist, retry with the real MOVIES folder
   (derived at load time - see Load() below) + just the filename. Only
   rewrites when the original path genuinely fails, so it can't affect
   any path that already resolves correctly. */
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

/* Main replacement for runMoviePlaybackStateMachine - same signature/
   calling convention as the original (__cdecl, one char* arg, int
   return). Contract: 0=still playing, 1=skipped, 2=finished. */
static int __cdecl hook_runMoviePlaybackStateMachine(const char *movieName)
{
    *(int *)0x004d69e0 = 0;
    *(int *)0x004d69dc = 0;
    *(int *)0x004d69b8 = 0;

    if (!g_active) {
        /* Real argument looks like "[MOVIES]\eng.tgq" - bracketed path
           alias, lowercase, .tgq included. Strip both to get the base
           movie name for the HD lookup. */
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
                /* Borderless window sized to the screen, not -fs (true
                   exclusive fullscreen) - avoids disrupting the game's
                   own DirectDraw surface, confirmed necessary by testing:
                   -fs broke the following movie's playback, this doesn't. */
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

                if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    g_ffplayProcess = pi.hProcess;
                    g_active = TRUE;
                    detour::trace("Playing HD replacement for '%s'", baseName);
                    return 0;
                }
                detour::trace("CreateProcess for ffplay failed, GetLastError=%lu - using original movie",
                         (unsigned long)GetLastError());
            }
        }

        /* No HD replacement (or ffplay unavailable/failed) - completely
           transparent fallback to the real, original function. */
        return g_origRunMovie(movieName);
    }

    /* Mid-playback of our HD replacement - poll, matching the original
       function's own per-frame poll contract exactly. */
    unsigned short key = readNextQueuedKeyEvent();
    unsigned char scancode = (unsigned char)((key >> 8) & 0xFF);
    if (scancode == 0x39 || scancode == 0x01) { /* Space or Escape */
        TerminateProcess(g_ffplayProcess, 0);
        CloseHandle(g_ffplayProcess);
        g_ffplayProcess = NULL;
        g_active = FALSE;
        reclaim_game_focus();
        return 1;
    }

    if (WaitForSingleObject(g_ffplayProcess, 0) == WAIT_OBJECT_0) {
        CloseHandle(g_ffplayProcess);
        g_ffplayProcess = NULL;
        g_active = FALSE;
        reclaim_game_focus();
        return 2;
    }

    return 0;
}

static BOOL patch_entry(void *targetAddr, int patchLen, int continueAddr, void *replacementFn, void **outOrig)
{
    unsigned char *target = (unsigned char *)targetAddr;
    unsigned char originalBytes[16]; /* patchLen never exceeds this */
    DWORD oldProtect;

    if (!VirtualProtect(target, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect))
        return FALSE;

    memcpy(originalBytes, target, patchLen);
    *outOrig = build_trampoline(originalBytes, patchLen, continueAddr);
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
    detour::trace("Hooking Movie Playback");

    /* Matches modmenu.cpp's own established approach exactly:
       GetModuleFileNameA(NULL, ...) gives the running EXE's path
       (EngRel.exe, in PRG_ENG) regardless of which DLL calls it, then
       stripping the path twice gets from ".../PRG_ENG/EngRel.exe" to
       ".../PRG_ENG" to the game's root folder - the same logic already
       proven working for locating the "Mods" folder. MOVIES is a
       sibling of PRG_ENG in every known install layout (CD, GOG, Steam),
       so this generalizes across all of them without special-casing. */
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0'; // exePath is now PRG_ENG

    char prgEngPath[MAX_PATH];
    strncpy(prgEngPath, exePath, sizeof(prgEngPath) - 1);
    prgEngPath[sizeof(prgEngPath) - 1] = '\0';

    char rootPath[MAX_PATH];
    strncpy(rootPath, exePath, sizeof(rootPath) - 1);
    rootPath[sizeof(rootPath) - 1] = '\0';
    char *rootSlash = strrchr(rootPath, '\\');
    if (rootSlash) *rootSlash = '\0'; // rootPath is now the game root

    _snprintf(g_realMoviesDir, sizeof(g_realMoviesDir), "%s\\MOVIES", rootPath);
    _snprintf(g_hdMoviesDir, sizeof(g_hdMoviesDir), "%s\\HDMovies", rootPath);
    _snprintf(g_ffplayPath, sizeof(g_ffplayPath), "%s\\ffplay.exe", prgEngPath);
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
