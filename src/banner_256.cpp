// Stage10: keep vanilla overlay / interaction placement for the original
// resource classes, and raise it for the new 256x256 body class by one extra
// projected 64-pixel half-height.  The correction is therefore one quarter of
// that body's actual current rendered screen height, not a fixed screen-pixel
// constant.
//
// Static tracing established the existing four-hook Stage10 path plus a
// fifth diagnostic projection hook.  Hook D's CPU-side billboard terms were
// proven by trace to be world/view-space rather than final screen pixels; the
// current visual correction is intentionally left unchanged while Hook E
// measures the real homogeneous projection divisor used by the CPU projector.
//   Hook A  0x0042B84F - buildUnitBodySpriteDrawQueue, hardware path.
//                         EDI is the winning resource-template entry and
//                         [ESP+0x10]+0x1C is the owning unit pointer.
//   Hook C  0x0042B97F - same function, after the body draw entry is populated.
//                         EBP is the body draw-entry and [ESP+0x10] is still the
//                         icon-queue entry whose +0x1C field is the unit ptr.
//   Hook D  0x00442B49 - FUN_00442B40, the hardware sprite quad builder.
//                         ESI is param_4, exactly the same body draw-entry that
//                         Hook C recorded.
//   Hook E  0x0043FD2C - FUN_0043FC60 billboard projection branch.
//                         ST0 holds the completed homogeneous W/fVar7 divisor;
//                         ESI is the current unit pointer.  A non-popping FST
//                         copies W to DLL-owned scratch storage.
//   Hook B  0x004504B8 - computeUnitScreenBounds.
//                         ESI is param_1 (the same unit pointer) for the whole
//                         function; decreasing its final screen Y raises banner,
//                         border, arrow, target marker and Engage hit area
//                         together without changing their dimensions.
//
// No game structure is repurposed.  All diagnostic state is DLL-owned.
#include "header.h"
#include "detour.h"
#include <string.h>

namespace banner_256
{
    static const DWORD HOOK_CLASSIFY   = 0x0042B84F;
    static const DWORD HOOK_ENTRY      = 0x0042B97F;
    static const DWORD HOOK_RENDER     = 0x00442B49;
    static const DWORD HOOK_PROJECTION = 0x0043FD2C;
    static const DWORD HOOK_ANCHOR     = 0x004504B8;

    static const DWORD RETURN_CLASSIFY   = 0x0042B854;
    static const DWORD RETURN_ENTRY      = 0x0042B986;
    static const DWORD RETURN_RENDER     = 0x00442B4E;
    static const DWORD RETURN_PROJECTION = 0x0043FD32;
    static const DWORD RETURN_ANCHOR     = 0x004504BF;

    static const BYTE kOriginalClassify[5] =
        { 0x8B,0x6F,0x48,0x85,0xED };
    static const BYTE kOriginalEntry[7] =
        { 0xC7,0x45,0x10,0x00,0x00,0x00,0x00 };
    static const BYTE kOriginalRender[5] =
        { 0x57,0x55,0x8B,0x4E,0x04 };
    static const BYTE kOriginalProjection[6] =
        { 0xD9,0x81,0x64,0x01,0x00,0x00 }; // fld dword ptr [ecx+164]
    static const BYTE kOriginalAnchor[7] =
        { 0xA1,0x14,0x37,0x50,0x00,0x2B,0xC2 };

    static const DWORD RENDER_HEIGHT_TERM = 0x00502B6C;
    static const DWORD CAMERA_UP_Y        = 0x00502B84;

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
        DWORD projectionWBits;
        float lastLoggedProjectionW;
        DWORD projectionLogCount;
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

    // Hook E uses FST (not FSTP) so the game's x87 stack is untouched.  The
    // helper copies only these raw IEEE-754 bits while the projection routine
    // is active; float formatting is deferred until Hook B, after projection.
    static volatile DWORD g_projectionWScratchBits = 0;

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

        if (is256)
        {
            state->is256 = TRUE;
            if (!state->loggedClass256)
            {
                darkomen::detour::trace("Stage10 class256 detected unit=%08lX template=%08lX width=%lu height=%lu",
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
                darkomen::detour::trace("Stage10 suppressed TRUE->FALSE unit=%08lX template=%08lX width=%lu height=%lu",
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
                UNIT_STATE* state = FindUnitState(unit);
                if (state != NULL && state->is256 && !state->loggedEntry)
                {
                    darkomen::detour::trace("Stage10 entryToUnit populated unit=%08lX entry=%08lX", unit, entry);
                    FlushTrace();
                    state->loggedEntry = TRUE;
                }
                return;
            }
            if (freeSlot == _countof(g_entryToUnit) && g_entryToUnit[i].entry == 0)
                freeSlot = i;
        }

        if (freeSlot < _countof(g_entryToUnit))
        {
            g_entryToUnit[freeSlot].entry = entry;
            g_entryToUnit[freeSlot].unit = unit;
            UNIT_STATE* state = FindUnitState(unit);
            if (state != NULL && state->is256 && !state->loggedEntry)
            {
                darkomen::detour::trace("Stage10 entryToUnit populated unit=%08lX entry=%08lX", unit, entry);
                FlushTrace();
                state->loggedEntry = TRUE;
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

    static void __cdecl RecordProjectionWBits(DWORD unit)
    {
        // Keep this helper integer-only so the diagnostic call does not need to
        // perform floating-point work while FUN_0043FC60's x87 stack is live.
        UNIT_STATE* state = FindUnitState(unit);
        if (state == NULL || !state->is256) return;

        const DWORD bits = g_projectionWScratchBits;
        if (bits != 0)
            state->projectionWBits = bits;
    }

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

        // Keep the current known-wrong Stage10 correction unchanged for this
        // diagnostic build.  We only add measurements for the full edge terms.
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
                darkomen::detour::trace("Stage10 renderedHeight unit=%08lX entry=%08lX h=%.3f frameScale=%.6f resource=%.6f upY=%.6f",
                    unit, entry, renderedHeight, frameScale, resourceTerm, cameraUpY);
                FlushTrace();
                state->loggedHeight = TRUE;
            }

            if (state->is256 && !state->loggedEdgeTerms)
            {
                const LONG anchor5 = *((LONG*)(entry + 0x14));
                const LONG anchor6 = *((LONG*)(entry + 0x18));
                const LONG anchor7 = *((LONG*)(entry + 0x1C));
                darkomen::detour::trace("Stage10 edgeTerms unit=%08lX entry=%08lX rawH=%.6f T_h=%.6f f36=%.6f f37=%.6f edge36Y=%.6f edge37Y=%.6f p4[5..7]=%ld,%ld,%ld",
                    unit, entry, rawHeightField, tHeight, fVar36, fVar37, edge36Y, edge37Y,
                    anchor5, anchor6, anchor7);
                FlushTrace();
                state->loggedEdgeTerms = TRUE;
            }
        }
    }

    static int __cdecl GetUnitAnchorRaise(DWORD unit)
    {
        UNIT_STATE* state = FindUnitState(unit);
        if (state == NULL || !state->is256) return 0;

        if (state->projectionWBits != 0 && state->projectionLogCount < 16)
        {
            union FLOAT_BITS
            {
                DWORD bits;
                float value;
            } wBits;
            wBits.bits = state->projectionWBits;

            float w = wBits.value;
            float absW = (w < 0.0f) ? -w : w;
            float delta = w - state->lastLoggedProjectionW;
            if (delta < 0.0f) delta = -delta;

            // Log the first sample, then only material changes so a stationary
            // unit does not fill trace.txt every frame.  0.25 is diagnostic
            // throttling only; it is not used by the banner correction.
            if (absW > 0.0001f && absW < 1000000.0f &&
                (state->projectionLogCount == 0 || delta >= 0.25f))
            {
                const LONG screenY = *((volatile LONG*)0x00503714);
                darkomen::detour::trace(
                    "Stage10 projectionW unit=%08lX W=%.6f screenY=%ld sample=%lu",
                    unit, w, screenY, state->projectionLogCount + 1);
                FlushTrace();
                state->lastLoggedProjectionW = w;
                ++state->projectionLogCount;
            }
        }

        const float h = state->renderedHeight;
        if (!(h > 0.0f && h < 4096.0f)) return 0;

        const int raise = (int)(h * 0.25f + 0.5f);
        const int safeRaise = (raise > 0 && raise < 1024) ? raise : 0;

        if (safeRaise > 0 && !state->loggedRaise)
        {
            darkomen::detour::trace("Stage10 anchorRaise unit=%08lX h=%.3f raise=%d", unit, h, safeRaise);
            FlushTrace();
            state->loggedRaise = TRUE;
        }

        return safeRaise;
    }

    static BOOL BuildCaves()
    {
        g_caves = (BYTE*)VirtualAlloc(NULL, 0x300, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_caves == NULL)
        {
            Log("banner_256: VirtualAlloc failed (%lu)", GetLastError());
            return FALSE;
        }

        BYTE* a = g_caves + 0x00;
        DWORD n = 0;

        a[n++] = 0x60;
        a[n++] = 0x8B; a[n++] = 0x44; a[n++] = 0x24; a[n++] = 0x30;
        a[n++] = 0x8B; a[n++] = 0x40; a[n++] = 0x1C;
        a[n++] = 0x57;
        a[n++] = 0x50;
        DWORD callA = n;
        a[n++] = 0xE8; n += 4;
        a[n++] = 0x83; a[n++] = 0xC4; a[n++] = 0x08;
        a[n++] = 0x61;
        a[n++] = 0x8B; a[n++] = 0x6F; a[n++] = 0x48;
        a[n++] = 0x85; a[n++] = 0xED;
        DWORD jumpA = n;
        a[n++] = 0xE9; n += 4;
        WriteRel32(a + callA, (DWORD)&MarkUnitClass);
        WriteRel32(a + jumpA, RETURN_CLASSIFY);

        BYTE* c = g_caves + 0x40;
        n = 0;
        c[n++] = 0xC7; c[n++] = 0x45; c[n++] = 0x10;
        *((DWORD*)(c + n)) = 0; n += 4;
        c[n++] = 0x9C;
        c[n++] = 0x60;
        c[n++] = 0x8B; c[n++] = 0x44; c[n++] = 0x24; c[n++] = 0x34;
        c[n++] = 0x8B; c[n++] = 0x40; c[n++] = 0x1C;
        c[n++] = 0x50;
        c[n++] = 0x55;
        DWORD callC = n;
        c[n++] = 0xE8; n += 4;
        c[n++] = 0x83; c[n++] = 0xC4; c[n++] = 0x08;
        c[n++] = 0x61;
        c[n++] = 0x9D;
        DWORD jumpC = n;
        c[n++] = 0xE9; n += 4;
        WriteRel32(c + callC, (DWORD)&RecordEntryUnit);
        WriteRel32(c + jumpC, RETURN_ENTRY);

        BYTE* d = g_caves + 0x80;
        n = 0;
        d[n++] = 0x57;
        d[n++] = 0x55;
        d[n++] = 0x8B; d[n++] = 0x4E; d[n++] = 0x04;
        d[n++] = 0x9C;
        d[n++] = 0x60;
        d[n++] = 0x56;
        DWORD callD = n;
        d[n++] = 0xE8; n += 4;
        d[n++] = 0x83; d[n++] = 0xC4; d[n++] = 0x04;
        d[n++] = 0x61;
        d[n++] = 0x9D;
        DWORD jumpD = n;
        d[n++] = 0xE9; n += 4;
        WriteRel32(d + callD, (DWORD)&CaptureRenderedHeight);
        WriteRel32(d + jumpD, RETURN_RENDER);

        BYTE* e = g_caves + 0x100;
        n = 0;

        // Hook E, entered at 0x0043FD2C.  ST0 already contains completed
        // fVar7/W.  FST m32 stores without popping, then we recreate the
        // displaced FLD [ECX+164] before preserving integer state.
        e[n++] = 0xD9; e[n++] = 0x15;                       // fst dword ptr [abs32]
        *((DWORD*)(e + n)) = (DWORD)&g_projectionWScratchBits; n += 4;
        e[n++] = 0xD9; e[n++] = 0x81;                       // fld dword ptr [ecx+164]
        e[n++] = 0x64; e[n++] = 0x01; e[n++] = 0x00; e[n++] = 0x00;
        e[n++] = 0x9C;
        e[n++] = 0x60;
        e[n++] = 0x56;
        DWORD callE = n;
        e[n++] = 0xE8; n += 4;
        e[n++] = 0x83; e[n++] = 0xC4; e[n++] = 0x04;
        e[n++] = 0x61;
        e[n++] = 0x9D;
        DWORD jumpE = n;
        e[n++] = 0xE9; n += 4;
        WriteRel32(e + callE, (DWORD)&RecordProjectionWBits);
        WriteRel32(e + jumpE, RETURN_PROJECTION);

        BYTE* b = g_caves + 0xC0;
        n = 0;
        b[n++] = 0xA1;
        *((DWORD*)(b + n)) = 0x00503714; n += 4;
        b[n++] = 0x2B; b[n++] = 0xC2;
        b[n++] = 0x9C;
        b[n++] = 0x51;
        b[n++] = 0x52;
        b[n++] = 0x50;
        b[n++] = 0x56;
        DWORD callB = n;
        b[n++] = 0xE8; n += 4;
        b[n++] = 0x83; b[n++] = 0xC4; b[n++] = 0x04;
        b[n++] = 0x8B; b[n++] = 0xD0;
        b[n++] = 0x58;
        b[n++] = 0x2B; b[n++] = 0xC2;
        b[n++] = 0x5A;
        b[n++] = 0x59;
        b[n++] = 0x9D;
        DWORD jumpB = n;
        b[n++] = 0xE9; n += 4;
        WriteRel32(b + callB, (DWORD)&GetUnitAnchorRaise);
        WriteRel32(b + jumpB, RETURN_ANCHOR);

        FlushInstructionCache(GetCurrentProcess(), g_caves, 0x300);
        return TRUE;
    }

    void Load()
    {
        if (g_loaded) return;

        if (!BytesEqual(HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify)))
        {
            Log("banner_256: EngRel mismatch at 0x0042B84F; Stage10 not installed");
            darkomen::detour::trace("Stage10 install FAIL byte-guard 0x0042B84F"); FlushTrace();
            return;
        }
        if (!BytesEqual(HOOK_ENTRY, kOriginalEntry, sizeof(kOriginalEntry)))
        {
            Log("banner_256: EngRel mismatch at 0x0042B97F; Stage10 not installed");
            darkomen::detour::trace("Stage10 install FAIL byte-guard 0x0042B97F"); FlushTrace();
            return;
        }
        if (!BytesEqual(HOOK_RENDER, kOriginalRender, sizeof(kOriginalRender)))
        {
            Log("banner_256: EngRel mismatch at 0x00442B49; Stage10 not installed");
            darkomen::detour::trace("Stage10 install FAIL byte-guard 0x00442B49"); FlushTrace();
            return;
        }
        if (!BytesEqual(HOOK_PROJECTION, kOriginalProjection, sizeof(kOriginalProjection)))
        {
            Log("banner_256: EngRel mismatch at 0x0043FD2C; Stage10 not installed");
            darkomen::detour::trace("Stage10 install FAIL byte-guard 0x0043FD2C"); FlushTrace();
            return;
        }
        if (!BytesEqual(HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor)))
        {
            Log("banner_256: EngRel mismatch at 0x004504B8; Stage10 not installed");
            darkomen::detour::trace("Stage10 install FAIL byte-guard 0x004504B8"); FlushTrace();
            return;
        }

        memset(g_units, 0, sizeof(g_units));
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));
        g_projectionWScratchBits = 0;

        if (!BuildCaves()) return;

        WriteJump(HOOK_CLASSIFY,   (DWORD)(g_caves + 0x00), 5);
        WriteJump(HOOK_ENTRY,      (DWORD)(g_caves + 0x40), 7);
        WriteJump(HOOK_RENDER,     (DWORD)(g_caves + 0x80), 5);
        WriteJump(HOOK_PROJECTION, (DWORD)(g_caves + 0x100), 6);
        WriteJump(HOOK_ANCHOR,     (DWORD)(g_caves + 0xC0), 7);
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        g_loaded = TRUE;
        Log("banner_256: Stage10 projection-W diagnostic installed");
        darkomen::detour::trace("Stage10 installed: projection-W diagnostic active; correction unchanged");
        FlushTrace();
    }

    void Unload()
    {
        if (!g_loaded) return;

        memcpy((void*)HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify));
        memcpy((void*)HOOK_ENTRY, kOriginalEntry, sizeof(kOriginalEntry));
        memcpy((void*)HOOK_RENDER, kOriginalRender, sizeof(kOriginalRender));
        memcpy((void*)HOOK_PROJECTION, kOriginalProjection, sizeof(kOriginalProjection));
        memcpy((void*)HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor));
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        if (g_caves != NULL)
            VirtualFree(g_caves, 0, MEM_RELEASE);

        g_caves = NULL;
        memset(g_units, 0, sizeof(g_units));
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));
        g_projectionWScratchBits = 0;
        g_loaded = FALSE;
    }
}
