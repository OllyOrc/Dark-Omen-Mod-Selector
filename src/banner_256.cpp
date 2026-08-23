// Stage11: zoom-aware banner positioning with continuous enlarged-body scaling.
//
// Dark Omen can report enlarged sprites through either nominal 128x128 or the
// added 128x256 resource bucket. The render frame record provides a stable
// per-body top-extent proxy even though it does not expose RDose's literal source
// dimensions directly. We calibrate once from the first valid body frame for a
// live unit and use a three-point continuous curve through the proven 133%-Troll
// point (top~=113, K=650), the ~202px Troll point (top~=150, K=1100), and the
// full-size point (top~=165+, K=1800). This prevents ~202px sprites from sitting
// almost at the full 256 banner height while preserving smooth interpolation for
// arbitrary enlarged sprites. Hooks E/F/G provide homogeneous W for zoom scaling
// and Hook B applies the shared overlay / interaction correction.
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

    static const float ANCHOR_K_MEDIUM = 650.0f;
    static const float ANCHOR_K_MID    = 1100.0f;
    static const float ANCHOR_K_LARGE  = 1800.0f;

    // Three-point calibration from the test Trolls:
    // ~152px source -> proxy top~=113 -> K=650
    // ~202px source -> proxy top~=150 -> K=1100
    // ~228px+ source -> proxy top~=165+ -> K=1800
    static const float CONTINUOUS_TOP_MEDIUM = 113.0f;
    static const float CONTINUOUS_TOP_MID    = 150.0f;
    static const float CONTINUOUS_TOP_LARGE  = 165.0f;

    // Conservative guard for nominal 128x128 winners. This is the proven
    // enlarged 133%-Troll envelope and keeps ordinary original units vanilla.
    static const float MEDIUM_BODY_MIN = 99.0f;
    static const float MEDIUM_TOP_MIN  = 110.0f;

    struct UNIT_STATE
    {
        DWORD unit;
        DWORD resourceWidth;
        DWORD resourceHeight;
        float bodyHeightPx;
        float topExtentPx;
        float calibrationTopPx;
        BOOL hasCalibrationTop;
        BOOL loggedClass;
        BOOL loggedSuppressedSmaller;
        BOOL loggedExtent;
        BOOL loggedRaise;
        BOOL loggedRawFrame;
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

    static BOOL IsExtendedTallClass(DWORD width, DWORD height)
    {
        return (height == 256 && (width == 128 || width == 256)) ? TRUE : FALSE;
    }

    static void __cdecl MarkUnitClass(DWORD unit, DWORD templateEntry)
    {
        if (unit == 0 || templateEntry == 0) return;

        const DWORD width  = *((DWORD*)(templateEntry + 0x18));
        const DWORD height = *((DWORD*)(templateEntry + 0x1C));
        const BOOL extendedTall = IsExtendedTallClass(width, height);

        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return;

        if (extendedTall)
        {
            if (state->resourceHeight != 256 || width > state->resourceWidth)
            {
                state->resourceWidth = width;
                state->resourceHeight = height;
                state->bodyHeightPx = 0.0f;
                state->topExtentPx = 0.0f;
                state->calibrationTopPx = 0.0f;
                state->hasCalibrationTop = FALSE;
                state->loggedExtent = FALSE;
                state->loggedRaise = FALSE;
                state->loggedRawFrame = FALSE;
            }

            if (!state->loggedClass)
            {
                darkomen::detour::trace(
                    "Stage11 extendedClass detected unit=%08lX template=%08lX width=%lu height=%lu",
                    unit, templateEntry, width, height);
                FlushTrace();
                state->loggedClass = TRUE;
            }
            return;
        }

        if (state->resourceHeight == 256)
        {
            if (!state->loggedSuppressedSmaller)
            {
                darkomen::detour::trace(
                    "Stage11 suppressed extended->smaller unit=%08lX template=%08lX width=%lu height=%lu",
                    unit, templateEntry, width, height);
                FlushTrace();
                state->loggedSuppressedSmaller = TRUE;
            }
            return;
        }

        state->resourceWidth = width;
        state->resourceHeight = height;
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

        if (!state->loggedRawFrame)
        {
            const DWORD* raw = (const DWORD*)frameRecord;
            darkomen::detour::trace(
                "Stage11 frameRaw unit=%08lX resource=%lux%lu frame=%ld rec=%08lX "
                "d00=%08lX d04=%08lX d08=%08lX d0C=%08lX d10=%08lX d14=%08lX "
                "d18=%08lX d1C=%08lX d20=%08lX d24=%08lX d28=%08lX",
                unit, state->resourceWidth, state->resourceHeight, frameIndex, frameRecord,
                raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                raw[6], raw[7], raw[8], raw[9], raw[10]);
            FlushTrace();
            state->loggedRawFrame = TRUE;
        }

        float frameScale = *((float*)(frameRecord + 0x14));
        float yOffsetRatio = *((float*)(frameRecord + 0x1C));
        if (frameScale < 0.0f) frameScale = -frameScale;
        if (yOffsetRatio < 0.0f) yOffsetRatio = -yOffsetRatio;

        const float backingHeight = (state->resourceHeight == 256) ? 256.0f :
                                    ((state->resourceHeight == 128) ? 128.0f : 0.0f);
        if (backingHeight <= 0.0f) return;

        const float nativeHeight = frameScale * backingHeight;
        const float topExtent = yOffsetRatio * nativeHeight;
        if (!(nativeHeight > 0.0f && nativeHeight < 1024.0f)) return;
        if (!(topExtent > 0.0f && topExtent < 1024.0f)) return;

        // Calibrate once from the first valid body frame. Later animation/body
        // passes can have very different extents, which must not make the banner
        // jump while the unit animates.
        if (!state->hasCalibrationTop)
        {
            state->calibrationTopPx = topExtent;
            state->hasCalibrationTop = TRUE;
            state->loggedRaise = FALSE;
        }

        if (nativeHeight > state->bodyHeightPx)
            state->bodyHeightPx = nativeHeight;
        if (topExtent > state->topExtentPx)
            state->topExtentPx = topExtent;

        if (!state->loggedExtent)
        {
            darkomen::detour::trace(
                "Stage11 bodyProxy unit=%08lX resource=%lux%lu bodyH=%.1f top=%.1f calibrationTop=%.1f",
                unit, state->resourceWidth, state->resourceHeight,
                state->bodyHeightPx, state->topExtentPx, state->calibrationTopPx);
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

    static float ContinuousAnchorK(float top)
    {
        if (top <= CONTINUOUS_TOP_MEDIUM)
            return ANCHOR_K_MEDIUM;

        if (top < CONTINUOUS_TOP_MID)
        {
            const float scale =
                (top - CONTINUOUS_TOP_MEDIUM) /
                (CONTINUOUS_TOP_MID - CONTINUOUS_TOP_MEDIUM);
            return ANCHOR_K_MEDIUM +
                scale * (ANCHOR_K_MID - ANCHOR_K_MEDIUM);
        }

        if (top < CONTINUOUS_TOP_LARGE)
        {
            const float scale =
                (top - CONTINUOUS_TOP_MID) /
                (CONTINUOUS_TOP_LARGE - CONTINUOUS_TOP_MID);
            return ANCHOR_K_MID +
                scale * (ANCHOR_K_LARGE - ANCHOR_K_MID);
        }

        return ANCHOR_K_LARGE;
    }

    static float GetAnchorK(const UNIT_STATE* state)
    {
        if (state == NULL) return 0.0f;

        // A true 256x256 winner remains the proven full-size path.
        if (state->resourceHeight == 256 && state->resourceWidth == 256)
            return ANCHOR_K_LARGE;

        // 128x256 is the physical bucket for intermediate/tall sprites. Before
        // the first body frame is seen, use the safe K=650 fallback; afterward
        // use the continuous three-point calibration curve.
        if (state->resourceHeight == 256 && state->resourceWidth == 128)
        {
            if (!state->hasCalibrationTop)
                return ANCHOR_K_MEDIUM;
            return ContinuousAnchorK(state->calibrationTopPx);
        }

        // Some enlarged sprites (the proven 133% Troll) can still win 128x128.
        // Promote only the conservative enlarged-body envelope.
        if (state->resourceHeight == 128 &&
            state->bodyHeightPx >= MEDIUM_BODY_MIN &&
            state->topExtentPx >= MEDIUM_TOP_MIN)
        {
            const float top = state->hasCalibrationTop ?
                state->calibrationTopPx : state->topExtentPx;
            return ContinuousAnchorK(top);
        }

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
                "Stage11 anchorRaiseK unit=%08lX resource=%lux%lu bodyH=%.1f top=%.1f calibrationTop=%.1f W=%.6f K=%.1f raise=%d",
                unit, state->resourceWidth, state->resourceHeight,
                state->bodyHeightPx, state->topExtentPx, state->calibrationTopPx,
                w, anchorK, safeRaise);
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
            darkomen::detour::trace("Stage11 install FAIL byte guard");
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
            "Stage11 installed: three-point continuous K=650/1100/1800 banner scaling active");
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
