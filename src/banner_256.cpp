// Stage10 final: keep vanilla overlay / interaction placement for the original
// resource classes, while applying the calibrated zoom-aware correction only
// to units whose body sprite resolves to the new 256x256 resource class.
//
// Hooks E/F/G capture homogeneous W and associate it with the true live unit.
// Hook B raises the shared overlay / interaction anchor by round(1900 / |W|).
// Temporary Stage10 calibration hooks, F11 DisplayOverlays control, rendered-
// height experiments, and high-frequency stable-W tracing have been removed.
#include "header.h"
#include "detour.h"
#include <string.h>

namespace banner_256
{
    static const DWORD HOOK_CLASSIFY       = 0x0042B84F;
    static const DWORD HOOK_PROJECTION_E   = 0x0043FD2C;
    static const DWORD HOOK_PROJECTION_F   = 0x0043FE24;
    static const DWORD HOOK_PROJECTION_G   = 0x00450427;
    static const DWORD HOOK_ANCHOR         = 0x004504B8;

    static const DWORD RETURN_CLASSIFY     = 0x0042B854;
    static const DWORD RETURN_PROJECTION_E = 0x0043FD32;
    static const DWORD RETURN_PROJECTION_F = 0x0043FE2A;
    static const DWORD RETURN_PROJECTION_G = 0x0045042C;
    static const DWORD RETURN_ANCHOR       = 0x004504BF;

    static const DWORD CALL_PROJECT_BUFFER = 0x00427C30;

    static const BYTE kOriginalClassify[5] =
        { 0x8B,0x6F,0x48,0x85,0xED };
    static const BYTE kOriginalProjectionE[6] =
        { 0xD9,0x81,0x64,0x01,0x00,0x00 };
    static const BYTE kOriginalProjectionF[6] =
        { 0xD9,0xC9,0xDE,0xC2,0xD9,0xC9 };
    static const BYTE kOriginalProjectionG[5] =
        { 0xE8,0x04,0x78,0xFD,0xFF };
    static const BYTE kOriginalAnchor[7] =
        { 0xA1,0x14,0x37,0x50,0x00,0x2B,0xC2 };

    static const float ANCHOR_K_256 = 1900.0f;

    struct UNIT_STATE
    {
        DWORD unit;
        BOOL is256;
        BOOL loggedClass256;
        BOOL loggedSuppressedFalse;
        BOOL loggedRaise;
        DWORD projectionWBits;
        DWORD projectionSequence;
    };

    static UNIT_STATE g_units[400];

    static BYTE* g_caves = NULL;
    static BOOL g_loaded = FALSE;

    static volatile DWORD g_projectionWScratchBits = 0;
    static volatile DWORD g_currentProjectionUnit = 0;
    static volatile DWORD g_lastProjectionUnit = 0;
    static volatile DWORD g_lastProjectionWBits = 0;
    static volatile DWORD g_projectionSequence = 0;

    static void FlushTrace()
    {
        if (darkomen::detour::traceFile != NULL)
            fflush(darkomen::detour::traceFile);
    }

    static BOOL BytesEqual(DWORD address, const BYTE* expected, DWORD count)
    {
        return (memcmp((const void*)address, expected, count) == 0);
    }

    static void WriteRel32(BYTE* instruction, DWORD target)
    {
        DWORD src_after = (DWORD)instruction + 5;
        *((DWORD*)(instruction + 1)) = target - src_after;
    }

    static void WriteJump(DWORD address, DWORD target, BYTE overwritten_size)
    {
        BYTE* p = (BYTE*)address;
        p[0] = 0xE9;
        *((DWORD*)(p + 1)) = target - (address + 5);
        for (BYTE i = 5; i < overwritten_size; ++i) p[i] = 0x90;
    }

    static UNIT_STATE* FindOrCreateUnitState(DWORD unit)
    {
        if (unit == 0) return NULL;

        DWORD freeSlot = _countof(g_units);
        for (DWORD i = 0; i < _countof(g_units); ++i)
        {
            if (g_units[i].unit == unit) return &g_units[i];
            if (freeSlot == _countof(g_units) && g_units[i].unit == 0)
                freeSlot = i;
        }

        if (freeSlot >= _countof(g_units)) return NULL;
        memset(&g_units[freeSlot], 0, sizeof(g_units[freeSlot]));
        g_units[freeSlot].unit = unit;
        return &g_units[freeSlot];
    }

    static void __cdecl MarkUnitClass(DWORD unit, DWORD templateEntry)
    {
        if (unit == 0 || templateEntry == 0) return;

        const DWORD width  = *((DWORD*)(templateEntry + 0x18));
        const DWORD height = *((DWORD*)(templateEntry + 0x1C));
        const BOOL is256 = (width == 256 && height == 256) ? TRUE : FALSE;

        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return;

        // Once a body pass has positively identified this live unit as using
        // the 256x256 class, later auxiliary 128x128 passes must not clear it.
        if (is256)
        {
            state->is256 = TRUE;
            if (!state->loggedClass256)
            {
                darkomen::detour::trace(
                    "Stage10 class256 detected unit=%08lX template=%08lX width=%lu height=%lu",
                    unit, templateEntry, width, height);
                FlushTrace();
                state->loggedClass256 = TRUE;
            }
            return;
        }

        if (state->is256)
        {
            if (!state->loggedSuppressedFalse)
            {
                darkomen::detour::trace(
                    "Stage10 suppressed TRUE->FALSE unit=%08lX template=%08lX width=%lu height=%lu",
                    unit, templateEntry, width, height);
                FlushTrace();
                state->loggedSuppressedFalse = TRUE;
            }
            return;
        }

        state->is256 = FALSE;
    }

    static void __cdecl PublishProjectionSample(DWORD ignoredUnit)
    {
        (void)ignoredUnit;

        const DWORD bits = g_projectionWScratchBits;
        const DWORD owner = g_currentProjectionUnit;
        if (bits == 0 || owner == 0) return;

        g_lastProjectionUnit = owner;
        g_lastProjectionWBits = bits;
        ++g_projectionSequence;
    }

    static void __cdecl RecordProjectionW(DWORD unit)
    {
        PublishProjectionSample(unit);
    }

    static int __cdecl GetUnitAnchorRaise(DWORD unit)
    {
        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return 0;

        const DWORD sequence = g_projectionSequence;
        if (g_lastProjectionUnit == unit &&
            g_lastProjectionWBits != 0 &&
            sequence != state->projectionSequence)
        {
            state->projectionWBits = g_lastProjectionWBits;
            state->projectionSequence = sequence;
        }

        if (!state->is256 || state->projectionWBits == 0)
            return 0;

        union FLOAT_BITS { DWORD bits; float value; } wBits;
        wBits.bits = state->projectionWBits;

        const float w = wBits.value;
        const float absW = (w < 0.0f) ? -w : w;
        if (!(absW > 0.0001f && absW < 1000000.0f))
            return 0;

        const int raise = (int)((ANCHOR_K_256 / absW) + 0.5f);
        const int safeRaise = (raise > 0 && raise < 1024) ? raise : 0;

        if (safeRaise > 0 && !state->loggedRaise)
        {
            darkomen::detour::trace(
                "Stage10 anchorRaiseK unit=%08lX W=%.6f K=%.1f raise=%d",
                unit, w, ANCHOR_K_256, safeRaise);
            FlushTrace();
            state->loggedRaise = TRUE;
        }

        return safeRaise;
    }

    static BOOL BuildCaves()
    {
        // Keep the proven cave offsets used during Stage10 validation. The
        // retired diagnostic slots at +0x40/+0x80 are intentionally unused.
        g_caves = (BYTE*)VirtualAlloc(
            NULL, 0x200, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_caves == NULL) return FALSE;

        // Hook A: classify the winning body resource template. Preserve the
        // original MOV EBP,[EDI+48] / TEST EBP,EBP sequence before returning.
        BYTE* a = g_caves + 0x00;
        DWORD n = 0;
        a[n++] = 0x60;
        a[n++] = 0x8B; a[n++] = 0x44; a[n++] = 0x24; a[n++] = 0x30;
        a[n++] = 0x8B; a[n++] = 0x40; a[n++] = 0x1C;
        a[n++] = 0x57; a[n++] = 0x50;
        DWORD callA = n; a[n++] = 0xE8; n += 4;
        a[n++] = 0x83; a[n++] = 0xC4; a[n++] = 0x08;
        a[n++] = 0x61;
        a[n++] = 0x8B; a[n++] = 0x6F; a[n++] = 0x48;
        a[n++] = 0x85; a[n++] = 0xED;
        DWORD jumpA = n; a[n++] = 0xE9; n += 4;
        WriteRel32(a + callA, (DWORD)&MarkUnitClass);
        WriteRel32(a + jumpA, RETURN_CLASSIFY);

        // Hook B: replay vanilla anchor calculation, obtain the per-unit
        // additional raise, then subtract it from the shared Y anchor.
        BYTE* b = g_caves + 0xC0;
        n = 0;
        b[n++] = 0xA1; *((DWORD*)(b + n)) = 0x00503714; n += 4;
        b[n++] = 0x2B; b[n++] = 0xC2;
        b[n++] = 0x9C;
        b[n++] = 0x51; b[n++] = 0x52; b[n++] = 0x50; b[n++] = 0x56;
        DWORD callB = n; b[n++] = 0xE8; n += 4;
        b[n++] = 0x83; b[n++] = 0xC4; b[n++] = 0x04;
        b[n++] = 0x8B; b[n++] = 0xD0;
        b[n++] = 0x58;
        b[n++] = 0x2B; b[n++] = 0xC2;
        b[n++] = 0x5A; b[n++] = 0x59;
        b[n++] = 0x9D;
        DWORD jumpB = n; b[n++] = 0xE9; n += 4;
        WriteRel32(b + callB, (DWORD)&GetUnitAnchorRaise);
        WriteRel32(b + jumpB, RETURN_ANCHOR);

        // Hook E: capture completed homogeneous W without popping ST0, replay
        // the stolen FLD, then publish the sample for the unit set by Hook G.
        BYTE* e = g_caves + 0x100;
        n = 0;
        e[n++] = 0xD9; e[n++] = 0x15;
        *((DWORD*)(e + n)) = (DWORD)&g_projectionWScratchBits; n += 4;
        e[n++] = 0xD9; e[n++] = 0x81; e[n++] = 0x64;
        e[n++] = 0x01; e[n++] = 0x00; e[n++] = 0x00;
        e[n++] = 0x9C; e[n++] = 0x60; e[n++] = 0x56;
        DWORD callE = n; e[n++] = 0xE8; n += 4;
        e[n++] = 0x83; e[n++] = 0xC4; e[n++] = 0x04;
        e[n++] = 0x61; e[n++] = 0x9D;
        DWORD jumpE = n; e[n++] = 0xE9; n += 4;
        WriteRel32(e + callE, (DWORD)&RecordProjectionW);
        WriteRel32(e + jumpE, RETURN_PROJECTION_E);

        // Hook F: same capture for the alternate projection branch, replaying
        // FXCH / FADDP ST2,ST0 / FXCH exactly.
        BYTE* f = g_caves + 0x140;
        n = 0;
        f[n++] = 0xD9; f[n++] = 0x15;
        *((DWORD*)(f + n)) = (DWORD)&g_projectionWScratchBits; n += 4;
        f[n++] = 0xD9; f[n++] = 0xC9;
        f[n++] = 0xDE; f[n++] = 0xC2;
        f[n++] = 0xD9; f[n++] = 0xC9;
        f[n++] = 0x9C; f[n++] = 0x60; f[n++] = 0x56;
        DWORD callF = n; f[n++] = 0xE8; n += 4;
        f[n++] = 0x83; f[n++] = 0xC4; f[n++] = 0x04;
        f[n++] = 0x61; f[n++] = 0x9D;
        DWORD jumpF = n; f[n++] = 0xE9; n += 4;
        WriteRel32(f + callF, (DWORD)&RecordProjectionW);
        WriteRel32(f + jumpF, RETURN_PROJECTION_F);

        // Hook G: ESI is the true computeUnitScreenBounds unit. The original
        // CALL argument is already on the stack, so stashing ESI is transparent.
        BYTE* g = g_caves + 0x180;
        n = 0;
        g[n++] = 0x89; g[n++] = 0x35;
        *((DWORD*)(g + n)) = (DWORD)&g_currentProjectionUnit; n += 4;
        DWORD callG = n; g[n++] = 0xE8; n += 4;
        DWORD jumpG = n; g[n++] = 0xE9; n += 4;
        WriteRel32(g + callG, CALL_PROJECT_BUFFER);
        WriteRel32(g + jumpG, RETURN_PROJECTION_G);

        FlushInstructionCache(GetCurrentProcess(), g_caves, 0x200);
        return TRUE;
    }

    void Load()
    {
        if (g_loaded) return;

        if (!BytesEqual(HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify)) ||
            !BytesEqual(HOOK_PROJECTION_E, kOriginalProjectionE, sizeof(kOriginalProjectionE)) ||
            !BytesEqual(HOOK_PROJECTION_F, kOriginalProjectionF, sizeof(kOriginalProjectionF)) ||
            !BytesEqual(HOOK_PROJECTION_G, kOriginalProjectionG, sizeof(kOriginalProjectionG)) ||
            !BytesEqual(HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor)))
        {
            darkomen::detour::trace("Stage10 install FAIL byte guard");
            FlushTrace();
            return;
        }

        memset(g_units, 0, sizeof(g_units));
        g_projectionWScratchBits = 0;
        g_currentProjectionUnit = 0;
        g_lastProjectionUnit = 0;
        g_lastProjectionWBits = 0;
        g_projectionSequence = 0;

        if (!BuildCaves()) return;

        WriteJump(HOOK_CLASSIFY,     (DWORD)(g_caves + 0x00), 5);
        WriteJump(HOOK_PROJECTION_E, (DWORD)(g_caves + 0x100), 6);
        WriteJump(HOOK_PROJECTION_F, (DWORD)(g_caves + 0x140), 6);
        WriteJump(HOOK_PROJECTION_G, (DWORD)(g_caves + 0x180), 5);
        WriteJump(HOOK_ANCHOR,       (DWORD)(g_caves + 0xC0), 7);
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        g_loaded = TRUE;
        darkomen::detour::trace(
            "Stage10 installed: final K=1900 zoom-aware 256 anchor correction active");
        FlushTrace();
    }

    void Unload()
    {
        if (!g_loaded) return;

        memcpy((void*)HOOK_CLASSIFY,     kOriginalClassify,     sizeof(kOriginalClassify));
        memcpy((void*)HOOK_PROJECTION_E, kOriginalProjectionE, sizeof(kOriginalProjectionE));
        memcpy((void*)HOOK_PROJECTION_F, kOriginalProjectionF, sizeof(kOriginalProjectionF));
        memcpy((void*)HOOK_PROJECTION_G, kOriginalProjectionG, sizeof(kOriginalProjectionG));
        memcpy((void*)HOOK_ANCHOR,       kOriginalAnchor,       sizeof(kOriginalAnchor));
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        if (g_caves != NULL)
            VirtualFree(g_caves, 0, MEM_RELEASE);

        g_caves = NULL;
        memset(g_units, 0, sizeof(g_units));
        g_projectionWScratchBits = 0;
        g_currentProjectionUnit = 0;
        g_lastProjectionUnit = 0;
        g_lastProjectionWBits = 0;
        g_projectionSequence = 0;
        g_loaded = FALSE;
    }
}
