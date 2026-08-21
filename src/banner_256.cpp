// Stage10: keep vanilla overlay / interaction placement for the original
// resource classes, and raise it for the new 256x256 body class by one extra
// projected 64-pixel half-height.  The correction is therefore one quarter of
// that body's actual current rendered screen height, not a fixed screen-pixel
// constant.
//
// Static tracing established four stable points in EngRel:
//   Hook A  0x0042B84F - buildUnitBodySpriteDrawQueue, hardware path.
//                         EDI is the winning resource-template entry and
//                         [ESP+0x10]+0x1C is the owning unit pointer.
//   Hook C  0x0042B97F - same function, after the body draw entry is populated.
//                         EBP is the body draw-entry and [ESP+0x10] is still the
//                         icon-queue entry whose +0x1C field is the unit ptr.
//   Hook D  0x00442B49 - FUN_00442B40, the hardware sprite quad builder.
//                         ESI is param_4, exactly the same body draw-entry that
//                         Hook C recorded.  The renderer's own live terms let
//                         us reproduce the body's full on-screen height.
//   Hook B  0x004504B8 - computeUnitScreenBounds.
//                         ESI is param_1 (the same unit pointer) for the whole
//                         function; decreasing its final screen Y raises banner,
//                         border, arrow, target marker and Engage hit area
//                         together without changing their dimensions.
//
// No game structure is repurposed.  The entry->unit association and per-unit
// class/rendered-height state are entirely DLL-owned.
#include "header.h"
#include <string.h>

namespace banner_256
{
    static const DWORD HOOK_CLASSIFY = 0x0042B84F;
    static const DWORD HOOK_ENTRY    = 0x0042B97F;
    static const DWORD HOOK_RENDER   = 0x00442B49;
    static const DWORD HOOK_ANCHOR   = 0x004504B8;

    static const DWORD RETURN_CLASSIFY = 0x0042B854;
    static const DWORD RETURN_ENTRY    = 0x0042B986;
    static const DWORD RETURN_RENDER   = 0x00442B4E;
    static const DWORD RETURN_ANCHOR   = 0x004504BF;

    static const BYTE kOriginalClassify[5] =
        { 0x8B,0x6F,0x48,0x85,0xED }; // mov ebp,[edi+48] ; test ebp,ebp
    static const BYTE kOriginalEntry[7] =
        { 0xC7,0x45,0x10,0x00,0x00,0x00,0x00 }; // mov dword ptr [ebp+10],0
    static const BYTE kOriginalRender[5] =
        { 0x57,0x55,0x8B,0x4E,0x04 }; // push edi ; push ebp ; mov ecx,[esi+04]
    static const BYTE kOriginalAnchor[7] =
        { 0xA1,0x14,0x37,0x50,0x00,0x2B,0xC2 }; // mov eax,[00503714] ; sub eax,edx

    // Live renderer terms used by FUN_00442B40's vertex-Y calculation.
    static const DWORD RENDER_HEIGHT_TERM = 0x00502B6C;
    static const DWORD CAMERA_UP_Y        = 0x00502B84;

    struct UNIT_STATE
    {
        DWORD unit;
        BOOL is256;
        float renderedHeight;
    };

    struct ENTRY_UNIT_LINK
    {
        DWORD entry;
        DWORD unit;
    };

    // Dark Omen's battle-side structures are capped at 400 entries elsewhere.
    static UNIT_STATE g_units[400];
    static ENTRY_UNIT_LINK g_entryToUnit[400];

    static BYTE* g_caves = NULL;
    static BOOL g_loaded = FALSE;

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
        {
            if (g_units[i].unit == unit)
                return &g_units[i];
        }
        return NULL;
    }

    static UNIT_STATE* FindOrCreateUnitState(DWORD unit)
    {
        if (unit == 0) return NULL;

        DWORD freeSlot = _countof(g_units);
        for (DWORD i = 0; i < _countof(g_units); ++i)
        {
            if (g_units[i].unit == unit)
                return &g_units[i];
            if (freeSlot == _countof(g_units) && g_units[i].unit == 0)
                freeSlot = i;
        }

        if (freeSlot >= _countof(g_units)) return NULL;

        g_units[freeSlot].unit = unit;
        g_units[freeSlot].is256 = FALSE;
        g_units[freeSlot].renderedHeight = 0.0f;
        return &g_units[freeSlot];
    }

    static void __cdecl MarkUnitClass(DWORD unit, DWORD templateEntry)
    {
        if (unit == 0 || templateEntry == 0) return;

        const DWORD width  = *((DWORD*)(templateEntry + 0x18));
        const DWORD height = *((DWORD*)(templateEntry + 0x1C));
        const BOOL is256 = (width == 256 && height == 256) ? TRUE : FALSE;

        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state != NULL)
            state->is256 = is256;
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
        {
            if (g_entryToUnit[i].entry == entry)
                return g_entryToUnit[i].unit;
        }
        return 0;
    }

    static void __cdecl CaptureRenderedHeight(DWORD entry)
    {
        const DWORD unit = FindUnitForEntry(entry);
        if (unit == 0 || entry == 0) return;

        // FUN_00442B40 computes the current animation frame record as:
        //   frameRecord = *(DWORD*)(*(DWORD*)entry + 0x10)
        //               + *(int*)(entry + 0x04) * 0x2C
        // and reads fVar28 from frameRecord+0x14.
        const DWORD owner = *((DWORD*)entry);
        if (owner == 0) return;

        const DWORD frameBase = *((DWORD*)(owner + 0x10));
        if (frameBase == 0) return;

        const LONG frameIndex = *((LONG*)(entry + 0x04));
        if (frameIndex < 0 || frameIndex > 4096) return;

        const DWORD frameRecord = frameBase + ((DWORD)frameIndex * 0x2C);
        const float frameScale = *((float*)(frameRecord + 0x14));
        const float resourceTerm = *((volatile float*)RENDER_HEIGHT_TERM);
        const float cameraUpY = *((volatile float*)CAMERA_UP_Y);

        float renderedHeight = -resourceTerm * frameScale * cameraUpY;
        if (renderedHeight < 0.0f)
            renderedHeight = -renderedHeight;

        // Reject zero, NaN and obviously nonsensical values without touching the
        // last known-good height.  NaN fails both ordered comparisons below.
        if (!(renderedHeight > 0.0f && renderedHeight < 4096.0f))
            return;

        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state != NULL)
            state->renderedHeight = renderedHeight;
    }

    static int __cdecl GetUnitAnchorRaise(DWORD unit)
    {
        UNIT_STATE* state = FindUnitState(unit);
        if (state == NULL || !state->is256) return 0;

        const float h = state->renderedHeight;
        if (!(h > 0.0f && h < 4096.0f)) return 0;

        // 256 full height has 64 native pixels of extra centre-to-top extent
        // compared with 128: 64/256 == 0.25.  h is positive, so +0.5 gives a
        // stable nearest-pixel integer before Hook B subtracts it from anchorY.
        const int raise = (int)(h * 0.25f + 0.5f);
        return (raise > 0 && raise < 1024) ? raise : 0;
    }

    static BOOL BuildCaves()
    {
        // Four compact x86 trampolines, kept well separated for readability.
        g_caves = (BYTE*)VirtualAlloc(NULL, 0x200, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_caves == NULL)
        {
            Log("banner_256: VirtualAlloc failed (%lu)", GetLastError());
            return FALSE;
        }

        BYTE* a = g_caves + 0x00;
        DWORD n = 0;

        // Hook A, entered at 0x0042B84F.
        // pushad changes ESP by 0x20, so original [esp+0x10] becomes [esp+0x30].
        a[n++] = 0x60;                                      // pushad
        a[n++] = 0x8B; a[n++] = 0x44; a[n++] = 0x24; a[n++] = 0x30; // mov eax,[esp+30]
        a[n++] = 0x8B; a[n++] = 0x40; a[n++] = 0x1C;       // mov eax,[eax+1C] (unit)
        a[n++] = 0x57;                                      // push edi (template entry)
        a[n++] = 0x50;                                      // push eax (unit)
        DWORD callA = n;
        a[n++] = 0xE8; n += 4;                              // call MarkUnitClass
        a[n++] = 0x83; a[n++] = 0xC4; a[n++] = 0x08;       // add esp,8
        a[n++] = 0x61;                                      // popad
        a[n++] = 0x8B; a[n++] = 0x6F; a[n++] = 0x48;       // mov ebp,[edi+48]
        a[n++] = 0x85; a[n++] = 0xED;                       // test ebp,ebp
        DWORD jumpA = n;
        a[n++] = 0xE9; n += 4;                              // jmp 0042B854

        WriteRel32(a + callA, (DWORD)&MarkUnitClass);
        WriteRel32(a + jumpA, RETURN_CLASSIFY);

        BYTE* c = g_caves + 0x40;
        n = 0;

        // Hook C, entered at 0x0042B97F.
        // Recreate MOV [EBP+10],0 before preserving state. pushfd+pushad then
        // shift the original stack by 0x24, so original [ESP+10] is [ESP+34].
        c[n++] = 0xC7; c[n++] = 0x45; c[n++] = 0x10;
        *((DWORD*)(c + n)) = 0; n += 4;                     // mov dword ptr [ebp+10],0
        c[n++] = 0x9C;                                      // pushfd
        c[n++] = 0x60;                                      // pushad
        c[n++] = 0x8B; c[n++] = 0x44; c[n++] = 0x24; c[n++] = 0x34; // mov eax,[esp+34]
        c[n++] = 0x8B; c[n++] = 0x40; c[n++] = 0x1C;       // mov eax,[eax+1C] (unit)
        c[n++] = 0x50;                                      // push eax (unit)
        c[n++] = 0x55;                                      // push ebp (entry)
        DWORD callC = n;
        c[n++] = 0xE8; n += 4;                              // call RecordEntryUnit
        c[n++] = 0x83; c[n++] = 0xC4; c[n++] = 0x08;       // add esp,8
        c[n++] = 0x61;                                      // popad
        c[n++] = 0x9D;                                      // popfd
        DWORD jumpC = n;
        c[n++] = 0xE9; n += 4;                              // jmp 0042B986

        WriteRel32(c + callC, (DWORD)&RecordEntryUnit);
        WriteRel32(c + jumpC, RETURN_ENTRY);

        BYTE* d = g_caves + 0x80;
        n = 0;

        // Hook D, entered at 0x00442B49 after ESI=param_4 has already loaded.
        // The first two pushes belong to the game's real stack frame and must
        // remain in place when we return to 0x00442B4E.
        d[n++] = 0x57;                                      // push edi
        d[n++] = 0x55;                                      // push ebp
        d[n++] = 0x8B; d[n++] = 0x4E; d[n++] = 0x04;       // mov ecx,[esi+04]
        d[n++] = 0x9C;                                      // pushfd
        d[n++] = 0x60;                                      // pushad
        d[n++] = 0x56;                                      // push esi (entry)
        DWORD callD = n;
        d[n++] = 0xE8; n += 4;                              // call CaptureRenderedHeight
        d[n++] = 0x83; d[n++] = 0xC4; d[n++] = 0x04;       // add esp,4
        d[n++] = 0x61;                                      // popad
        d[n++] = 0x9D;                                      // popfd
        DWORD jumpD = n;
        d[n++] = 0xE9; n += 4;                              // jmp 00442B4E

        WriteRel32(d + callD, (DWORD)&CaptureRenderedHeight);
        WriteRel32(d + jumpD, RETURN_RENDER);

        BYTE* b = g_caves + 0xC0;
        n = 0;

        // Hook B, entered at 0x004504B8. ESI is the current unit pointer.
        // Recreate the displaced vanilla Y calculation first and preserve its
        // flags.  The helper returns a positive number of pixels to raise.
        b[n++] = 0xA1;                                      // mov eax,[00503714]
        *((DWORD*)(b + n)) = 0x00503714; n += 4;
        b[n++] = 0x2B; b[n++] = 0xC2;                       // sub eax,edx
        b[n++] = 0x9C;                                      // pushfd (vanilla flags)
        b[n++] = 0x51;                                      // push ecx
        b[n++] = 0x52;                                      // push edx
        b[n++] = 0x50;                                      // push eax (saved vanilla Y)
        b[n++] = 0x56;                                      // push esi (unit argument)
        DWORD callB = n;
        b[n++] = 0xE8; n += 4;                              // call GetUnitAnchorRaise
        b[n++] = 0x83; b[n++] = 0xC4; b[n++] = 0x04;       // add esp,4
        b[n++] = 0x8B; b[n++] = 0xD0;                       // mov edx,eax (raise)
        b[n++] = 0x58;                                      // pop eax (vanilla Y)
        b[n++] = 0x2B; b[n++] = 0xC2;                       // sub eax,edx
        b[n++] = 0x5A;                                      // pop edx
        b[n++] = 0x59;                                      // pop ecx
        b[n++] = 0x9D;                                      // popfd (restore vanilla flags)
        DWORD jumpB = n;
        b[n++] = 0xE9; n += 4;                              // jmp 004504BF

        WriteRel32(b + callB, (DWORD)&GetUnitAnchorRaise);
        WriteRel32(b + jumpB, RETURN_ANCHOR);

        FlushInstructionCache(GetCurrentProcess(), g_caves, 0x200);
        return TRUE;
    }

    void Load()
    {
        if (g_loaded) return;

        if (!BytesEqual(HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify)))
        {
            Log("banner_256: EngRel mismatch at 0x0042B84F; Stage10 not installed");
            return;
        }
        if (!BytesEqual(HOOK_ENTRY, kOriginalEntry, sizeof(kOriginalEntry)))
        {
            Log("banner_256: EngRel mismatch at 0x0042B97F; Stage10 not installed");
            return;
        }
        if (!BytesEqual(HOOK_RENDER, kOriginalRender, sizeof(kOriginalRender)))
        {
            Log("banner_256: EngRel mismatch at 0x00442B49; Stage10 not installed");
            return;
        }
        if (!BytesEqual(HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor)))
        {
            Log("banner_256: EngRel mismatch at 0x004504B8; Stage10 not installed");
            return;
        }

        memset(g_units, 0, sizeof(g_units));
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));

        if (!BuildCaves()) return;

        WriteJump(HOOK_CLASSIFY, (DWORD)(g_caves + 0x00), 5);
        WriteJump(HOOK_ENTRY,    (DWORD)(g_caves + 0x40), 7);
        WriteJump(HOOK_RENDER,   (DWORD)(g_caves + 0x80), 5);
        WriteJump(HOOK_ANCHOR,   (DWORD)(g_caves + 0xC0), 7);
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        g_loaded = TRUE;
        Log("banner_256: Stage10 installed; 256x256 overlay/interaction Y tracks 1/4 of rendered body height");
    }

    void Unload()
    {
        if (!g_loaded) return;

        memcpy((void*)HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify));
        memcpy((void*)HOOK_ENTRY, kOriginalEntry, sizeof(kOriginalEntry));
        memcpy((void*)HOOK_RENDER, kOriginalRender, sizeof(kOriginalRender));
        memcpy((void*)HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor));
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        if (g_caves != NULL)
            VirtualFree(g_caves, 0, MEM_RELEASE);

        g_caves = NULL;
        memset(g_units, 0, sizeof(g_units));
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));
        g_loaded = FALSE;
    }
}
