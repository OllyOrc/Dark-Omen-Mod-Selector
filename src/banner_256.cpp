// Stage10 diagnostic: keep vanilla overlay / interaction placement for the
// original resource classes while investigating a zoom-aware correction for
// the new 256x256 body class.
//
// Hooks A/C/D/B are the existing Stage10 path. Hooks E and F cover both
// branches of FUN_0043FC60 and capture the completed homogeneous W divisor
// without disturbing the x87 stack. Hook G runs at the proven owner site in
// computeUnitScreenBounds immediately before CALL 0x00427C30, where ESI is
// still the true unit pointer, and publishes that pointer for E/F to associate
// with the nested projection sample. The stable-W sampler records valid W at a
// low fixed time cadence even when the camera is stationary. F11 temporarily
// toggles the game's DisplayOverlays flag so a calibration screenshot can show
// the unobstructed 256x256 body at exactly the same camera position. No visual
// anchor formula change is made here.
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

    static const DWORD RENDER_HEIGHT_TERM = 0x00502B6C;
    static const DWORD CAMERA_UP_Y        = 0x00502B84;
    static const DWORD DISPLAY_OVERLAYS    = 0x004BF0F0;

    struct UNIT_STATE
    {
        DWORD unit;
        BOOL is256;
        float renderedHeight;
        BOOL loggedClass256;
        BOOL loggedSuppressedFalse;
        BOOL loggedEntry;
        BOOL loggedHeight;
        BOOL loggedEdgeTerms;
        BOOL loggedRaise;
        BOOL loggedProjectionStatus;
        DWORD projectionWBits;
        DWORD projectionSource;
        DWORD projectionSequence;
        DWORD lastProjectionSequenceSeen;
        float lastLoggedProjectionW;
        DWORD projectionLogCount;
        DWORD lastProjectionLogTick;
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
    static volatile DWORD g_lastProjectionSource = 0;
    static volatile DWORD g_projectionSequence = 0;

    static BOOL g_overlayToggleKeyWasDown = FALSE;
    static BOOL g_overlayWasToggled = FALSE;
    static DWORD g_originalDisplayOverlays = 1;

    static void FlushTrace()
    {
        if (darkomen::detour::traceFile != NULL)
            fflush(darkomen::detour::traceFile);
    }

    static void PollOverlayCalibrationToggle()
    {
        const BOOL keyDown = (GetAsyncKeyState(VK_F11) & 0x8000) ? TRUE : FALSE;
        if (keyDown && !g_overlayToggleKeyWasDown)
        {
            volatile DWORD* displayOverlays = (volatile DWORD*)DISPLAY_OVERLAYS;
            const DWORD current = *displayOverlays;
            const DWORD next = current ? 0UL : 1UL;
            *displayOverlays = next;
            g_overlayWasToggled = TRUE;
            darkomen::detour::trace("Stage10 overlayToggle key=F11 DisplayOverlays=%lu", next);
            FlushTrace();
        }
        g_overlayToggleKeyWasDown = keyDown;
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
            if (freeSlot == _countof(g_units) && g_units[i].unit == 0) freeSlot = i;
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
        if (is256)
        {
            state->is256 = TRUE;
            if (!state->loggedClass256)
            {
                darkomen::detour::trace("Stage10 class256 detected unit=%08lX template=%08lX width=%lu height=%lu", unit, templateEntry, width, height);
                FlushTrace(); state->loggedClass256 = TRUE;
            }
            return;
        }
        if (state->is256)
        {
            if (!state->loggedSuppressedFalse)
            {
                darkomen::detour::trace("Stage10 suppressed TRUE->FALSE unit=%08lX template=%08lX width=%lu height=%lu", unit, templateEntry, width, height);
                FlushTrace(); state->loggedSuppressedFalse = TRUE;
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
                UNIT_STATE* state = FindUnitState(unit);
                if (state != NULL && state->is256 && !state->loggedEntry)
                {
                    darkomen::detour::trace("Stage10 entryToUnit populated unit=%08lX entry=%08lX", unit, entry);
                    FlushTrace(); state->loggedEntry = TRUE;
                }
                return;
            }
            if (freeSlot == _countof(g_entryToUnit) && g_entryToUnit[i].entry == 0) freeSlot = i;
        }
        if (freeSlot < _countof(g_entryToUnit))
        {
            g_entryToUnit[freeSlot].entry = entry;
            g_entryToUnit[freeSlot].unit = unit;
            UNIT_STATE* state = FindUnitState(unit);
            if (state != NULL && state->is256 && !state->loggedEntry)
            {
                darkomen::detour::trace("Stage10 entryToUnit populated unit=%08lX entry=%08lX", unit, entry);
                FlushTrace(); state->loggedEntry = TRUE;
            }
        }
    }

    static DWORD FindUnitForEntry(DWORD entry)
    {
        if (entry == 0) return 0;
        for (DWORD i = 0; i < _countof(g_entryToUnit); ++i)
            if (g_entryToUnit[i].entry == entry) return g_entryToUnit[i].unit;
        return 0;
    }

    static void __cdecl PublishProjectionSample(DWORD ignoredUnit, DWORD source)
    {
        (void)ignoredUnit;
        const DWORD bits = g_projectionWScratchBits;
        const DWORD owner = g_currentProjectionUnit;
        if (bits == 0 || owner == 0) return;
        g_lastProjectionUnit = owner;
        g_lastProjectionWBits = bits;
        g_lastProjectionSource = source;
        ++g_projectionSequence;
    }

    static void __cdecl RecordProjectionWFromE(DWORD unit) { PublishProjectionSample(unit, 1); }
    static void __cdecl RecordProjectionWFromF(DWORD unit) { PublishProjectionSample(unit, 2); }

    static void __cdecl CaptureRenderedHeight(DWORD entry)
    {
        const DWORD unit = FindUnitForEntry(entry);
        if (unit == 0 || entry == 0) return;
        const DWORD owner = *((DWORD*)entry);
        if (owner == 0) return;
        const DWORD frameBase = *((DWORD*)(owner + 0x10));
        if (frameBase == 0) return;
        const LONG frameIndex = *((LONG*)(entry + 0x04));
        if (frameIndex < 0 || frameIndex > 4096) return;
        const DWORD frameRecord = frameBase + ((DWORD)frameIndex * 0x2C);
        const float frameScale = *((float*)(frameRecord + 0x14));
        const float rawHeightField = *((float*)(frameRecord + 0x1C));
        const float resourceTerm = *((volatile float*)RENDER_HEIGHT_TERM);
        const float cameraUpY = *((volatile float*)CAMERA_UP_Y);
        float renderedHeight = -resourceTerm * frameScale * cameraUpY;
        if (renderedHeight < 0.0f) renderedHeight = -renderedHeight;
        const float tHeight = rawHeightField * resourceTerm;
        const float fVar36 = -tHeight * frameScale;
        const float fVar37 = -(resourceTerm + tHeight) * frameScale;
        float edge36Y = fVar36 * cameraUpY;
        float edge37Y = fVar37 * cameraUpY;
        if (edge36Y < 0.0f) edge36Y = -edge36Y;
        if (edge37Y < 0.0f) edge37Y = -edge37Y;
        if (!(renderedHeight > 0.0f && renderedHeight < 4096.0f)) return;
        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state != NULL)
        {
            state->renderedHeight = renderedHeight;
            if (state->is256 && !state->loggedHeight)
            {
                darkomen::detour::trace("Stage10 renderedHeight unit=%08lX entry=%08lX h=%.3f frameScale=%.6f resource=%.6f upY=%.6f", unit, entry, renderedHeight, frameScale, resourceTerm, cameraUpY);
                FlushTrace(); state->loggedHeight = TRUE;
            }
            if (state->is256 && !state->loggedEdgeTerms)
            {
                const LONG anchor5 = *((LONG*)(entry + 0x14));
                const LONG anchor6 = *((LONG*)(entry + 0x18));
                const LONG anchor7 = *((LONG*)(entry + 0x1C));
                darkomen::detour::trace("Stage10 edgeTerms unit=%08lX entry=%08lX rawH=%.6f T_h=%.6f f36=%.6f f37=%.6f edge36Y=%.6f edge37Y=%.6f p4[5..7]=%ld,%ld,%ld", unit, entry, rawHeightField, tHeight, fVar36, fVar37, edge36Y, edge37Y, anchor5, anchor6, anchor7);
                FlushTrace(); state->loggedEdgeTerms = TRUE;
            }
        }
    }

    static int __cdecl GetUnitAnchorRaise(DWORD unit)
    {
        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return 0;

        if (state->is256)
            PollOverlayCalibrationToggle();

        const DWORD sequence = g_projectionSequence;
        const DWORD lastUnit = g_lastProjectionUnit;
        const DWORD lastBits = g_lastProjectionWBits;
        const DWORD lastSource = g_lastProjectionSource;

        if (state->is256 && !state->loggedProjectionStatus)
        {
            union FLOAT_BITS { DWORD bits; float value; } raw;
            raw.bits = lastBits;
            darkomen::detour::trace(
                "Stage10 projectionStatus unit=%08lX seq=%lu capturedUnit=%08lX source=%c rawBits=%08lX rawW=%.6f match=%lu",
                unit, sequence, lastUnit,
                (lastSource == 2) ? 'F' : ((lastSource == 1) ? 'E' : '-'),
                lastBits, raw.value, (lastUnit == unit) ? 1UL : 0UL);
            FlushTrace();
            state->loggedProjectionStatus = TRUE;
        }

        if (lastUnit == unit && lastBits != 0 && sequence != state->projectionSequence)
        {
            state->projectionWBits = lastBits;
            state->projectionSource = lastSource;
            state->projectionSequence = sequence;
        }

        if (state->is256 && state->projectionWBits != 0 &&
            state->projectionSequence != state->lastProjectionSequenceSeen)
        {
            union FLOAT_BITS { DWORD bits; float value; } wBits;
            wBits.bits = state->projectionWBits;
            const float w = wBits.value;
            const float absW = (w < 0.0f) ? -w : w;
            const DWORD now = GetTickCount();
            if (absW > 0.0001f && absW < 1000000.0f &&
                (state->projectionLogCount == 0 ||
                 (DWORD)(now - state->lastProjectionLogTick) >= 500))
            {
                const LONG screenY = *((volatile LONG*)0x00503714);
                darkomen::detour::trace("Stage10 stableW unit=%08lX W=%.6f screenY=%ld source=%c sample=%lu seq=%lu", unit, w, screenY, (state->projectionSource == 2) ? 'F' : 'E', state->projectionLogCount + 1, state->projectionSequence);
                FlushTrace();
                state->lastProjectionLogTick = now;
                state->lastLoggedProjectionW = w;
                ++state->projectionLogCount;
            }
            state->lastProjectionSequenceSeen = state->projectionSequence;
        }

        if (!state->is256) return 0;
        const float h = state->renderedHeight;
        if (!(h > 0.0f && h < 4096.0f)) return 0;
        const int raise = (int)(h * 0.25f + 0.5f);
        const int safeRaise = (raise > 0 && raise < 1024) ? raise : 0;
        if (safeRaise > 0 && !state->loggedRaise)
        {
            darkomen::detour::trace("Stage10 anchorRaise unit=%08lX h=%.3f raise=%d", unit, h, safeRaise);
            FlushTrace(); state->loggedRaise = TRUE;
        }
        return safeRaise;
    }

    static BOOL BuildCaves()
    {
        g_caves = (BYTE*)VirtualAlloc(NULL, 0x400, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_caves == NULL) return FALSE;

        BYTE* a = g_caves + 0x00; DWORD n = 0;
        a[n++] = 0x60;
        a[n++] = 0x8B; a[n++] = 0x44; a[n++] = 0x24; a[n++] = 0x30;
        a[n++] = 0x8B; a[n++] = 0x40; a[n++] = 0x1C;
        a[n++] = 0x57; a[n++] = 0x50;
        DWORD callA = n; a[n++] = 0xE8; n += 4;
        a[n++] = 0x83; a[n++] = 0xC4; a[n++] = 0x08; a[n++] = 0x61;
        a[n++] = 0x8B; a[n++] = 0x6F; a[n++] = 0x48; a[n++] = 0x85; a[n++] = 0xED;
        DWORD jumpA = n; a[n++] = 0xE9; n += 4;
        WriteRel32(a + callA, (DWORD)&MarkUnitClass); WriteRel32(a + jumpA, RETURN_CLASSIFY);

        BYTE* c = g_caves + 0x40; n = 0;
        c[n++] = 0xC7; c[n++] = 0x45; c[n++] = 0x10; *((DWORD*)(c + n)) = 0; n += 4;
        c[n++] = 0x9C; c[n++] = 0x60;
        c[n++] = 0x8B; c[n++] = 0x44; c[n++] = 0x24; c[n++] = 0x34;
        c[n++] = 0x8B; c[n++] = 0x40; c[n++] = 0x1C; c[n++] = 0x50; c[n++] = 0x55;
        DWORD callC = n; c[n++] = 0xE8; n += 4;
        c[n++] = 0x83; c[n++] = 0xC4; c[n++] = 0x08; c[n++] = 0x61; c[n++] = 0x9D;
        DWORD jumpC = n; c[n++] = 0xE9; n += 4;
        WriteRel32(c + callC, (DWORD)&RecordEntryUnit); WriteRel32(c + jumpC, RETURN_ENTRY);

        BYTE* d = g_caves + 0x80; n = 0;
        d[n++] = 0x57; d[n++] = 0x55; d[n++] = 0x8B; d[n++] = 0x4E; d[n++] = 0x04;
        d[n++] = 0x9C; d[n++] = 0x60; d[n++] = 0x56;
        DWORD callD = n; d[n++] = 0xE8; n += 4;
        d[n++] = 0x83; d[n++] = 0xC4; d[n++] = 0x04; d[n++] = 0x61; d[n++] = 0x9D;
        DWORD jumpD = n; d[n++] = 0xE9; n += 4;
        WriteRel32(d + callD, (DWORD)&CaptureRenderedHeight); WriteRel32(d + jumpD, RETURN_RENDER);

        BYTE* b = g_caves + 0xC0; n = 0;
        b[n++] = 0xA1; *((DWORD*)(b + n)) = 0x00503714; n += 4; b[n++] = 0x2B; b[n++] = 0xC2;
        b[n++] = 0x9C; b[n++] = 0x51; b[n++] = 0x52; b[n++] = 0x50; b[n++] = 0x56;
        DWORD callB = n; b[n++] = 0xE8; n += 4;
        b[n++] = 0x83; b[n++] = 0xC4; b[n++] = 0x04; b[n++] = 0x8B; b[n++] = 0xD0;
        b[n++] = 0x58; b[n++] = 0x2B; b[n++] = 0xC2; b[n++] = 0x5A; b[n++] = 0x59; b[n++] = 0x9D;
        DWORD jumpB = n; b[n++] = 0xE9; n += 4;
        WriteRel32(b + callB, (DWORD)&GetUnitAnchorRaise); WriteRel32(b + jumpB, RETURN_ANCHOR);

        BYTE* e = g_caves + 0x100; n = 0;
        e[n++] = 0xD9; e[n++] = 0x15; *((DWORD*)(e + n)) = (DWORD)&g_projectionWScratchBits; n += 4;
        e[n++] = 0xD9; e[n++] = 0x81; e[n++] = 0x64; e[n++] = 0x01; e[n++] = 0x00; e[n++] = 0x00;
        e[n++] = 0x9C; e[n++] = 0x60; e[n++] = 0x56;
        DWORD callE = n; e[n++] = 0xE8; n += 4;
        e[n++] = 0x83; e[n++] = 0xC4; e[n++] = 0x04; e[n++] = 0x61; e[n++] = 0x9D;
        DWORD jumpE = n; e[n++] = 0xE9; n += 4;
        WriteRel32(e + callE, (DWORD)&RecordProjectionWFromE); WriteRel32(e + jumpE, RETURN_PROJECTION_E);

        BYTE* f = g_caves + 0x140; n = 0;
        f[n++] = 0xD9; f[n++] = 0x15; *((DWORD*)(f + n)) = (DWORD)&g_projectionWScratchBits; n += 4;
        f[n++] = 0xD9; f[n++] = 0xC9; f[n++] = 0xDE; f[n++] = 0xC2; f[n++] = 0xD9; f[n++] = 0xC9;
        f[n++] = 0x9C; f[n++] = 0x60; f[n++] = 0x56;
        DWORD callF = n; f[n++] = 0xE8; n += 4;
        f[n++] = 0x83; f[n++] = 0xC4; f[n++] = 0x04; f[n++] = 0x61; f[n++] = 0x9D;
        DWORD jumpF = n; f[n++] = 0xE9; n += 4;
        WriteRel32(f + callF, (DWORD)&RecordProjectionWFromF); WriteRel32(f + jumpF, RETURN_PROJECTION_F);

        BYTE* g = g_caves + 0x180; n = 0;
        // Hook G: ESI is the true computeUnitScreenBounds unit. The original
        // CALL argument has already been pushed, so stashing ESI is transparent.
        g[n++] = 0x89; g[n++] = 0x35;
        *((DWORD*)(g + n)) = (DWORD)&g_currentProjectionUnit; n += 4;
        DWORD callG = n; g[n++] = 0xE8; n += 4;
        DWORD jumpG = n; g[n++] = 0xE9; n += 4;
        WriteRel32(g + callG, CALL_PROJECT_BUFFER);
        WriteRel32(g + jumpG, RETURN_PROJECTION_G);

        FlushInstructionCache(GetCurrentProcess(), g_caves, 0x400); return TRUE;
    }

    void Load()
    {
        if (g_loaded) return;
        if (!BytesEqual(HOOK_CLASSIFY,kOriginalClassify,sizeof(kOriginalClassify)) ||
            !BytesEqual(HOOK_ENTRY,kOriginalEntry,sizeof(kOriginalEntry)) ||
            !BytesEqual(HOOK_RENDER,kOriginalRender,sizeof(kOriginalRender)) ||
            !BytesEqual(HOOK_PROJECTION_E,kOriginalProjectionE,sizeof(kOriginalProjectionE)) ||
            !BytesEqual(HOOK_PROJECTION_F,kOriginalProjectionF,sizeof(kOriginalProjectionF)) ||
            !BytesEqual(HOOK_PROJECTION_G,kOriginalProjectionG,sizeof(kOriginalProjectionG)) ||
            !BytesEqual(HOOK_ANCHOR,kOriginalAnchor,sizeof(kOriginalAnchor)))
        {
            darkomen::detour::trace("Stage10 install FAIL byte guard"); FlushTrace(); return;
        }
        memset(g_units,0,sizeof(g_units)); memset(g_entryToUnit,0,sizeof(g_entryToUnit));
        g_projectionWScratchBits = g_currentProjectionUnit = g_lastProjectionUnit = g_lastProjectionWBits = g_lastProjectionSource = g_projectionSequence = 0;
        g_originalDisplayOverlays = *((volatile DWORD*)DISPLAY_OVERLAYS);
        g_overlayToggleKeyWasDown = FALSE;
        g_overlayWasToggled = FALSE;
        if (!BuildCaves()) return;
        WriteJump(HOOK_CLASSIFY,(DWORD)(g_caves+0x00),5);
        WriteJump(HOOK_ENTRY,(DWORD)(g_caves+0x40),7);
        WriteJump(HOOK_RENDER,(DWORD)(g_caves+0x80),5);
        WriteJump(HOOK_PROJECTION_E,(DWORD)(g_caves+0x100),6);
        WriteJump(HOOK_PROJECTION_F,(DWORD)(g_caves+0x140),6);
        WriteJump(HOOK_PROJECTION_G,(DWORD)(g_caves+0x180),5);
        WriteJump(HOOK_ANCHOR,(DWORD)(g_caves+0xC0),7);
        FlushInstructionCache(GetCurrentProcess(),NULL,0);
        g_loaded=TRUE;
        darkomen::detour::trace("Stage10 installed: Hook-G stableW + F11 overlay calibration diagnostic active; correction unchanged");
        darkomen::detour::trace("Stage10 overlayCalibration initial DisplayOverlays=%lu; press F11 to toggle", g_originalDisplayOverlays);
        FlushTrace();
    }

    void Unload()
    {
        if (!g_loaded) return;
        memcpy((void*)HOOK_CLASSIFY,kOriginalClassify,sizeof(kOriginalClassify));
        memcpy((void*)HOOK_ENTRY,kOriginalEntry,sizeof(kOriginalEntry));
        memcpy((void*)HOOK_RENDER,kOriginalRender,sizeof(kOriginalRender));
        memcpy((void*)HOOK_PROJECTION_E,kOriginalProjectionE,sizeof(kOriginalProjectionE));
        memcpy((void*)HOOK_PROJECTION_F,kOriginalProjectionF,sizeof(kOriginalProjectionF));
        memcpy((void*)HOOK_PROJECTION_G,kOriginalProjectionG,sizeof(kOriginalProjectionG));
        memcpy((void*)HOOK_ANCHOR,kOriginalAnchor,sizeof(kOriginalAnchor));
        FlushInstructionCache(GetCurrentProcess(),NULL,0);
        if (g_overlayWasToggled)
            *((volatile DWORD*)DISPLAY_OVERLAYS) = g_originalDisplayOverlays;
        if(g_caves!=NULL) VirtualFree(g_caves,0,MEM_RELEASE);
        g_caves=NULL;
        memset(g_units,0,sizeof(g_units));
        memset(g_entryToUnit,0,sizeof(g_entryToUnit));
        g_projectionWScratchBits = g_currentProjectionUnit = g_lastProjectionUnit = g_lastProjectionWBits = g_lastProjectionSource = g_projectionSequence = 0;
        g_overlayToggleKeyWasDown = FALSE;
        g_overlayWasToggled = FALSE;
        g_loaded=FALSE;
    }
}
