// Stage9C: raise the shared unit overlay / interaction Y anchor only for
// units whose current body sprite resolves to the new 256x256 resource class.
//
// Live tracing established two stable points in EngRel:
//   0x0042B84F - buildUnitBodySpriteDrawQueue, hardware path.
//                EDI is the winning resource-template entry and [ESP+0x10]
//                is the queue-entry pointer; queueEntry+0x1C is the unit ptr.
//   0x004504B8 - computeUnitScreenBounds.
//                ESI is param_1 (the same unit pointer) for the whole function.
//
// The original five resource classes are left untouched.  A unit is marked
// class-256 only when the resolved template is exactly 256x256.  The proven
// correction is then -32 pixels on the shared Y anchor, which moves banner,
// border, arrow, target box and Engage hit area together without changing
// their dimensions.
#include "header.h"
#include <string.h>

namespace banner_256
{
    static const DWORD HOOK_CLASSIFY = 0x0042B84F;
    static const DWORD HOOK_ANCHOR   = 0x004504B8;
    static const DWORD RETURN_CLASSIFY = 0x0042B854;
    static const DWORD RETURN_ANCHOR   = 0x004504BF;

    static const BYTE kOriginalClassify[5] =
        { 0x8B,0x6F,0x48,0x85,0xED }; // mov ebp,[edi+48] ; test ebp,ebp
    static const BYTE kOriginalAnchor[7] =
        { 0xA1,0x14,0x37,0x50,0x00,0x2B,0xC2 }; // mov eax,[00503714] ; sub eax,edx

    struct UNIT_CLASS_STATE
    {
        DWORD unit;
        BOOL is256;
    };

    // Dark Omen's battle-side structures are capped at 400 entries elsewhere;
    // keep the cache fixed-size and entirely DLL-owned rather than borrowing an
    // undocumented byte from the game's unit structure.
    static UNIT_CLASS_STATE g_units[400];

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

    static void __cdecl MarkUnitClass(DWORD unit, DWORD templateEntry)
    {
        if (unit == 0 || templateEntry == 0) return;

        const DWORD width  = *((DWORD*)(templateEntry + 0x18));
        const DWORD height = *((DWORD*)(templateEntry + 0x1C));
        const BOOL is256 = (width == 256 && height == 256) ? TRUE : FALSE;

        DWORD freeSlot = _countof(g_units);
        for (DWORD i = 0; i < _countof(g_units); ++i)
        {
            if (g_units[i].unit == unit)
            {
                g_units[i].is256 = is256;
                return;
            }
            if (freeSlot == _countof(g_units) && g_units[i].unit == 0)
                freeSlot = i;
        }

        if (freeSlot < _countof(g_units))
        {
            g_units[freeSlot].unit = unit;
            g_units[freeSlot].is256 = is256;
        }
    }

    static BOOL __cdecl IsUnitClass256(DWORD unit)
    {
        if (unit == 0) return FALSE;

        for (DWORD i = 0; i < _countof(g_units); ++i)
        {
            if (g_units[i].unit == unit)
                return g_units[i].is256;
        }
        return FALSE;
    }

    static BOOL BuildCaves()
    {
        // Two tiny x86 trampolines live in one allocation.
        g_caves = (BYTE*)VirtualAlloc(NULL, 0x100, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_caves == NULL)
        {
            Log("banner_256: VirtualAlloc failed (%lu)", GetLastError());
            return FALSE;
        }

        BYTE* a = g_caves;
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

        BYTE* b = g_caves + 0x40;
        n = 0;

        // Hook B, entered at 0x004504B8. ESI is the current unit pointer.
        // Recreate the displaced vanilla calculation first.
        b[n++] = 0xA1;                                      // mov eax,[00503714]
        *((DWORD*)(b + n)) = 0x00503714; n += 4;
        b[n++] = 0x2B; b[n++] = 0xC2;                       // sub eax,edx

        // Preserve the vanilla result and volatile registers around the helper.
        b[n++] = 0x51;                                      // push ecx
        b[n++] = 0x52;                                      // push edx
        b[n++] = 0x50;                                      // push eax (saved vanilla Y)
        b[n++] = 0x56;                                      // push esi (unit argument)
        DWORD callB = n;
        b[n++] = 0xE8; n += 4;                              // call IsUnitClass256
        b[n++] = 0x83; b[n++] = 0xC4; b[n++] = 0x04;       // add esp,4
        b[n++] = 0x85; b[n++] = 0xC0;                       // test eax,eax
        b[n++] = 0x58;                                      // pop eax (restore vanilla Y; flags retained)
        b[n++] = 0x5A;                                      // pop edx
        b[n++] = 0x59;                                      // pop ecx
        b[n++] = 0x74; b[n++] = 0x03;                       // jz +3
        b[n++] = 0x83; b[n++] = 0xE8; b[n++] = 0x20;       // sub eax,20h
        DWORD jumpB = n;
        b[n++] = 0xE9; n += 4;                              // jmp 004504BF

        WriteRel32(b + callB, (DWORD)&IsUnitClass256);
        WriteRel32(b + jumpB, RETURN_ANCHOR);

        FlushInstructionCache(GetCurrentProcess(), g_caves, 0x100);
        return TRUE;
    }

    void Load()
    {
        if (g_loaded) return;

        if (!BytesEqual(HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify)))
        {
            Log("banner_256: EngRel mismatch at 0x0042B84F; Stage9C not installed");
            return;
        }
        if (!BytesEqual(HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor)))
        {
            Log("banner_256: EngRel mismatch at 0x004504B8; Stage9C not installed");
            return;
        }

        memset(g_units, 0, sizeof(g_units));

        if (!BuildCaves()) return;

        WriteJump(HOOK_CLASSIFY, (DWORD)g_caves, 5);
        WriteJump(HOOK_ANCHOR, (DWORD)(g_caves + 0x40), 7);
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        g_loaded = TRUE;
        Log("banner_256: Stage9C installed; 256x256 units use -32 shared overlay/interaction Y correction");
    }

    void Unload()
    {
        if (!g_loaded) return;

        memcpy((void*)HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify));
        memcpy((void*)HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor));
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        if (g_caves != NULL)
            VirtualFree(g_caves, 0, MEM_RELEASE);

        g_caves = NULL;
        memset(g_units, 0, sizeof(g_units));
        g_loaded = FALSE;
    }
}
