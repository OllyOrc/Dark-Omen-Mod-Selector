// Stage10 final positioning with an intermediate large-sprite banner tier.
//
// Original-sized sprites keep vanilla overlay / interaction placement. Units
// whose body uses the 256x256 resource path are additionally measured from the
// body frame's native Y origin. This lets moderately enlarged sprites (for
// example the 133% Troll) receive a smaller zoom-aware raise than genuinely
// large 256 sprites, while preserving the proven K=1900 correction for Dread
// King / full-size large sprites.
//
// Hook A identifies the body resource class. Hooks C/D associate the body draw
// entry with its live unit and recover the native top extent from the frame
// data. Hooks E/F/G capture homogeneous W for zoom scaling. Hook B applies the
// shared overlay / interaction correction.
#include "header.h"
#include "detour.h"
#include <string.h>

namespace banner_256
{
    static const DWORD HOOK_CLASSIFY       = 0x0042B84F;
    static const DWORD HOOK_ENTRY          = 0x0042B97F;
    static const DWORD HOOK_RENDER         = 0x00442B49;
    static const DWORD HOOK_PROJECTION_E   = 0x0043FD2C;
    static const DWORD HOOK_PROJECTION_F   = 0x0043FE24;
    static const DWORD HOOK_PROJECTION_G   = 0x00450427;
    static const DWORD HOOK_ANCHOR         = 0x004504B8;

    static const DWORD RETURN_CLASSIFY     = 0x0042B854;
    static const DWORD RETURN_ENTRY        = 0x0042B986;
    static const DWORD RETURN_RENDER       = 0x00442B4E;
    static const DWORD RETURN_PROJECTION_E = 0x0043FD32;
    static const DWORD RETURN_PROJECTION_F = 0x0043FE2A;
    static const DWORD RETURN_PROJECTION_G = 0x0045042C;
    static const DWORD RETURN_ANCHOR       = 0x004504BF;

    static const DWORD CALL_PROJECT_BUFFER = 0x00427C30;

    static const BYTE kOriginalClassify[5] =
        { 0x8B,0x6F,0x48,0x85,0xED };
    static const BYTE kOriginalEntry[7] =
        { 0xC7,0x45,0x10,0x00,0x00,0x00,0x00 };
    static const BYTE kOriginalRender[5] =
        { 0x57,0x55,0x8B,0x4E,0x04 };
    static const BYTE kOriginalProjectionE[6] =
        { 0xD9,0x81,0x64,0x01,0x00,0x00 };
    static const BYTE kOriginalProjectionF[6] =
        { 0xD9,0xC9,0xDE,0xC2,0xD9,0xC9 };
    static const BYTE kOriginalProjectionG[5] =
        { 0xE8,0x04,0x78,0xFD,0xFF };
    static const BYTE kOriginalAnchor[7] =
        { 0xA1,0x14,0x37,0x50,0x00,0x2B,0xC2 };

    // Calibrated full-size value plus an intermediate tier for sprites whose
    // top extent is larger than vanilla but still well below the full 256 case.
    static const float ANCHOR_K_MEDIUM = 650.0f;
    static const float ANCHOR_K_LARGE  = 1900.0f;
    static const float MEDIUM_TOP_MIN   = 130.0f;
    static const float LARGE_TOP_MIN    = 180.0f;

    struct UNIT_STATE
    {
        DWORD unit;
        BOOL is256;
        float topExtentPx;
        BOOL loggedClass256;
        BOOL loggedSuppressedFalse;
        BOOL loggedExtent;
        BOOL loggedRaise;
        DWORD projectionWBits;
        DWORD projectionSequence;
    };

    struct ENTRY_UNIT_LINK
    {
        DWORD entry;
        DWORD unit;
    };

    static UNIT_STATE g_units[400];
    static ENTRY_UNIT_LINK g_entryToUnit[400];

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

    static UNIT_STATE* FindUnitState(DWORD unit)
    {
        if (unit == 0) return NULL;
        for (DWORD i = 0; i < _countof(g_units); ++i)
            if (g_units[i].unit == unit) return &g_units[i];
        return NULL;
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

        // Keep positive 256 identification sticky: auxiliary passes can later
        // use smaller templates and must not demote the live unit.
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

    static void __cdecl RecordEntryUnit(DWORD entry, DWORD unit)
    {
        if (entry == 0 || unit == 0) return;

        DWORD freeSlot = _countof(g_entryToUnit);
        for (DWORD i = 0; i < _countof(g_entryToUnit); ++i)
        {
            if (g_entryToUnit[i].entry == entry)
            {
                g_entryToUnit[i].unit = unit;
                return;
            }
            if (freeSlot == _countof(g_entryToUnit) && g_entryToUnit[i].entry == 0)
                freeSlot = i;
        }

        if (freeSlot < _countof(g_entryToUnit))
        {
            g_entryToUnit[freeSlot].entry = entry;
            g_entryToUnit[freeSlot].unit = unit;
        }
    }

    static DWORD FindUnitForEntry(DWORD entry)
    {
        if (entry == 0) return 0;
        for (DWORD i = 0; i < _countof(g_entryToUnit); ++i)
            if (g_entryToUnit[i].entry == entry) return g_entryToUnit[i].unit;
        return 0;
    }

    static void __cdecl CaptureBodyTopExtent(DWORD entry)
    {
        const DWORD unit = FindUnitForEntry(entry);
        if (unit == 0 || entry == 0) return;

        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return;

        const DWORD owner = *((DWORD*)entry);
        if (owner == 0) return;
        const DWORD frameBase = *((DWORD*)(owner + 0x10));
        if (frameBase == 0) return;

        const LONG frameIndex = *((LONG*)(entry + 0x04));
        if (frameIndex < 0 || frameIndex > 4096) return;

        const DWORD frameRecord = frameBase + ((DWORD)frameIndex * 0x2C);
        float frameScale = *((float*)(frameRecord + 0x14));
        float yOffsetRatio = *((float*)(frameRecord + 0x1C));
        if (frameScale < 0.0f) frameScale = -frameScale;
        if (yOffsetRatio < 0.0f) yOffsetRatio = -yOffsetRatio;

        // For the 256-backed body resource, +0x14 is nativeFrameHeight / 256
        // and +0x1C is nativeYOffset / nativeFrameHeight. Their product therefore
        // recovers the native Y-origin magnitude (the useful top extent). If the
        // 256 classification has not landed yet, retain the sample but use 128 as
        // the conservative backing height; a later 256 body pass will replace it.
        const float backingHeight = state->is256 ? 256.0f : 128.0f;
        const float nativeHeight = frameScale * backingHeight;
        const float topExtent = yOffsetRatio * nativeHeight;
        if (!(topExtent > 0.0f && topExtent < 1024.0f)) return;

        if (topExtent > state->topExtentPx)
        {
            state->topExtentPx = topExtent;
            state->loggedRaise = FALSE;
        }

        if (!state->loggedExtent && state->is256)
        {
            const char* tier = (state->topExtentPx >= LARGE_TOP_MIN) ? "large" :
                               ((state->topExtentPx >= MEDIUM_TOP_MIN) ? "medium" : "vanilla");
            darkomen::detour::trace(
                "Stage10 bodyExtent unit=%08lX top=%.1f frameH=%.1f tier=%s",
                unit, state->topExtentPx, nativeHeight, tier);
            FlushTrace();
            state->loggedExtent = TRUE;
        }
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

    static float GetAnchorK(const UNIT_STATE* state)
    {
        if (state == NULL || !state->is256) return 0.0f;

        // Until the body frame measurement arrives, preserve the already-tested
        // full 256 behaviour rather than momentarily dropping to vanilla.
        if (state->topExtentPx <= 0.0f) return ANCHOR_K_LARGE;
        if (state->topExtentPx >= LARGE_TOP_MIN) return ANCHOR_K_LARGE;
        if (state->topExtentPx >= MEDIUM_TOP_MIN) return ANCHOR_K_MEDIUM;
        return 0.0f;
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

        const float anchorK = GetAnchorK(state);
        if (anchorK <= 0.0f || state->projectionWBits == 0)
            return 0;

        union FLOAT_BITS { DWORD bits; float value; } wBits;
        wBits.bits = state->projectionWBits;

        const float w = wBits.value;
        const float absW = (w < 0.0f) ? -w : w;
        if (!(absW > 0.0001f && absW < 1000000.0f))
            return 0;

        const int raise = (int)((anchorK / absW) + 0.5f);
        const int safeRaise = (raise > 0 && raise < 1024) ? raise : 0;

        if (safeRaise > 0 && !state->loggedRaise)
        {
            darkomen::detour::trace(
                "Stage10 anchorRaiseK unit=%08lX top=%.1f W=%.6f K=%.1f raise=%d",
                unit, state->topExtentPx, w, anchorK, safeRaise);
            FlushTrace();
            state->loggedRaise = TRUE;
        }

        return safeRaise;
    }

    static BOOL BuildCaves()
    {
        g_caves = (BYTE*)VirtualAlloc(
            NULL, 0x200, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_caves == NULL) return FALSE;

        // Hook A: classify the winning body resource template.
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

        // Hook C: retain the proven body-entry -> live-unit association.
        BYTE* c = g_caves + 0x40;
        n = 0;
        c[n++] = 0xC7; c[n++] = 0x45; c[n++] = 0x10;
        *((DWORD*)(c + n)) = 0; n += 4;
        c[n++] = 0x9C; c[n++] = 0x60;
        c[n++] = 0x8B; c[n++] = 0x44; c[n++] = 0x24; c[n++] = 0x34;
        c[n++] = 0x8B; c[n++] = 0x40; c[n++] = 0x1C;
        c[n++] = 0x50; c[n++] = 0x55;
        DWORD callC = n; c[n++] = 0xE8; n += 4;
        c[n++] = 0x83; c[n++] = 0xC4; c[n++] = 0x08;
        c[n++] = 0x61; c[n++] = 0x9D;
        DWORD jumpC = n; c[n++] = 0xE9; n += 4;
        WriteRel32(c + callC, (DWORD)&RecordEntryUnit);
        WriteRel32(c + jumpC, RETURN_ENTRY);

        // Hook D: inspect the associated body frame and recover native top extent.
        BYTE* d = g_caves + 0x80;
        n = 0;
        d[n++] = 0x57; d[n++] = 0x55;
        d[n++] = 0x8B; d[n++] = 0x4E; d[n++] = 0x04;
        d[n++] = 0x9C; d[n++] = 0x60; d[n++] = 0x56;
        DWORD callD = n; d[n++] = 0xE8; n += 4;
        d[n++] = 0x83; d[n++] = 0xC4; d[n++] = 0x04;
        d[n++] = 0x61; d[n++] = 0x9D;
        DWORD jumpD = n; d[n++] = 0xE9; n += 4;
        WriteRel32(d + callD, (DWORD)&CaptureBodyTopExtent);
        WriteRel32(d + jumpD, RETURN_RENDER);

        // Hook B: replay vanilla anchor calculation and subtract the tiered raise.
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

        // Hook E: completed homogeneous W, alternate projection branch 1.
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

        // Hook F: completed homogeneous W, alternate projection branch 2.
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

        // Hook G: ESI is the true computeUnitScreenBounds unit.
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
            !BytesEqual(HOOK_ENTRY, kOriginalEntry, sizeof(kOriginalEntry)) ||
            !BytesEqual(HOOK_RENDER, kOriginalRender, sizeof(kOriginalRender)) ||
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
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));
        g_projectionWScratchBits = 0;
        g_currentProjectionUnit = 0;
        g_lastProjectionUnit = 0;
        g_lastProjectionWBits = 0;
        g_projectionSequence = 0;

        if (!BuildCaves()) return;

        WriteJump(HOOK_CLASSIFY,     (DWORD)(g_caves + 0x00), 5);
        WriteJump(HOOK_ENTRY,        (DWORD)(g_caves + 0x40), 7);
        WriteJump(HOOK_RENDER,       (DWORD)(g_caves + 0x80), 5);
        WriteJump(HOOK_PROJECTION_E, (DWORD)(g_caves + 0x100), 6);
        WriteJump(HOOK_PROJECTION_F, (DWORD)(g_caves + 0x140), 6);
        WriteJump(HOOK_PROJECTION_G, (DWORD)(g_caves + 0x180), 5);
        WriteJump(HOOK_ANCHOR,       (DWORD)(g_caves + 0xC0), 7);
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        g_loaded = TRUE;
        darkomen::detour::trace(
            "Stage10 installed: tiered 256 banner correction active (medium K=650, large K=1900)");
        FlushTrace();
    }

    void Unload()
    {
        if (!g_loaded) return;

        memcpy((void*)HOOK_CLASSIFY,     kOriginalClassify,     sizeof(kOriginalClassify));
        memcpy((void*)HOOK_ENTRY,        kOriginalEntry,        sizeof(kOriginalEntry));
        memcpy((void*)HOOK_RENDER,       kOriginalRender,       sizeof(kOriginalRender));
        memcpy((void*)HOOK_PROJECTION_E, kOriginalProjectionE, sizeof(kOriginalProjectionE));
        memcpy((void*)HOOK_PROJECTION_F, kOriginalProjectionF, sizeof(kOriginalProjectionF));
        memcpy((void*)HOOK_PROJECTION_G, kOriginalProjectionG, sizeof(kOriginalProjectionG));
        memcpy((void*)HOOK_ANCHOR,       kOriginalAnchor,       sizeof(kOriginalAnchor));
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        if (g_caves != NULL)
            VirtualFree(g_caves, 0, MEM_RELEASE);

        g_caves = NULL;
        memset(g_units, 0, sizeof(g_units));
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));
        g_projectionWScratchBits = 0;
        g_currentProjectionUnit = 0;
        g_lastProjectionUnit = 0;
        g_lastProjectionWBits = 0;
        g_projectionSequence = 0;
        g_loaded = FALSE;
    }
}
