// Runtime Dark Omen sprite extensions.
//
// This module ports the already-tested EngRel changes into darkpatch.dll so
// the on-disk EngRel.exe does not need to be modified:
//   * Stage4H: full 8-bit/256-colour sprite path.
//   * Stage8E/11: keep the original classes, add 128x256 for moderately tall
//                 sprites, and retain 256x256 for the largest sprites.
//
// IMPORTANT: the Stage4H code payload below is copied byte-for-byte from the
// tested Stage4H executable, then its external rel32 branches are relocated to
// wherever VirtualAlloc places the runtime block. Internal relative branches
// remain valid because the complete payload is moved as one block.
#include "header.h"
#include <string.h>

namespace sprite_256
{
    static const DWORD CODE_SIZE = 0x800;
    static const DWORD TABLE_OFFSET = 0x1000;
    static const DWORD BLOCK_SIZE = 0x2000;

    static BYTE* g_block = NULL;
    static BYTE* g_code = NULL;
    static DWORD* g_table = NULL;
    static BOOL g_loaded = FALSE;

    static const BYTE kSpr8Code[CODE_SIZE] =
    {
#include "sprite_256_payload_0.inc"
#include "sprite_256_payload_1.inc"
#include "sprite_256_payload_2.inc"
#include "sprite_256_payload_3.inc"

    };

    struct REL32_FIXUP
    {
        DWORD offset;
        DWORD target;
    };

    static const REL32_FIXUP kRel32Fixups[] =
    {
        { 0x010, 0x004486BA },
        { 0x01C, 0x004486BA },
        { 0x04D, 0x0044330C },
        { 0x0D7, 0x00448630 },
        { 0x0E3, 0x00448630 },
        { 0x210, 0x0044B830 },
        { 0x226, 0x004487B6 },
        { 0x230, 0x0044870F },
        { 0x24F, 0x004486F3 },
        { 0x257, 0x004486F5 },
        { 0x291, 0x004477A0 },
        { 0x296, 0x00448805 },
        { 0x2B0, 0x004432E6 },
        { 0x31B, 0x00443383 },
        { 0x3B2, 0x00448805 },
        { 0x3B7, 0x004477A0 },
        { 0x3BC, 0x00448805 },
        { 0x410, 0x00443240 },
        { 0x44B, 0x00443383 },
        { 0x69F, 0x0044BE20 },
        { 0x6C9, 0x00442460 },
        { 0x6D5, 0x004422F0 },
        { 0x6DA, 0x00442460 },
    };

    struct HOOK_SITE
    {
        DWORD address;
        BYTE size;
        DWORD code_offset;
        BYTE original[7];
    };

    static const HOOK_SITE kHookSites[] =
    {
        { 0x0044244E, 6, 0x600, { 0x8B,0x47,0x18,0x8B,0x4E,0x30,0x00 } },
        { 0x00443239, 7, 0x400, { 0x8B,0x44,0x24,0x40,0x8B,0x73,0x24 } },
        { 0x004432DF, 7, 0x2A0, { 0x8B,0x44,0x24,0x40,0x8B,0x7B,0x0C } },
        { 0x00443307, 5, 0x040, { 0xC1,0xE6,0x05,0x2B,0xCA,0x00,0x00 } },
        { 0x00448629, 7, 0x0C0, { 0xC7,0x40,0x1C,0x10,0x00,0x00,0x00 } },
        { 0x004486B3, 7, 0x000, { 0x8B,0x46,0x0C,0x8B,0x54,0x24,0x14 } },
        { 0x004486ED, 6, 0x240, { 0x0F,0xAF,0xC1,0x99,0x2B,0xC2,0x00 } },
        { 0x00448709, 6, 0x110, { 0x8B,0x46,0x18,0x90,0x6A,0x00,0x00 } },
        { 0x00448800, 5, 0x380, { 0xE8,0x9B,0xEF,0xFF,0xFF,0x00,0x00 } },
    };

    // Ten DWORDs per row, 0x28-byte stride. Keep rows ordered from smaller to
    // larger fits so a 63x152-style frame can resolve to 128x256 before 256x256.
    // The engine already supports rectangular resource classes (32x64/64x32),
    // so 128x256 follows the same power-of-two pattern.
    static const DWORD kResourceTemplates[7][10] =
    {
        { 400, 30, 50,  32,  32, 4, 0, 0, 0, 0 },
        { 400, 30, 50,  32,  64, 4, 0, 0, 0, 0 },
        { 400, 30, 50,  64,  32, 4, 0, 0, 0, 0 },
        { 400, 30, 50,  64,  64, 4, 0, 0, 0, 0 },
        { 400, 10, 20, 128, 128, 4, 0, 0, 0, 0 },
        { 400, 10, 20, 128, 256, 4, 0, 0, 0, 0 },
        { 400, 10, 20, 256, 256, 4, 0, 0, 0, 0 },
    };

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

    static BOOL VerifyCompatibleEngRel()
    {
        for (DWORD i = 0; i < _countof(kHookSites); ++i)
        {
            const HOOK_SITE& s = kHookSites[i];
            if (!BytesEqual(s.address, s.original, s.size))
            {
                Log("sprite_256: EngRel mismatch at 0x%08X; 256-colour patch not installed", s.address);
                return FALSE;
            }
        }

        const BYTE count_original[2] = { 0x6A, 0x05 };
        const BYTE table_original[5] = { 0x68, 0xF8, 0x6D, 0x4D, 0x00 };

        if (!BytesEqual(0x0042B685, count_original, sizeof(count_original)))
        {
            Log("sprite_256: EngRel mismatch at 0x0042B685; extended classes not installed");
            return FALSE;
        }
        if (!BytesEqual(0x0042B68C, table_original, sizeof(table_original)))
        {
            Log("sprite_256: EngRel mismatch at 0x0042B68C; extended classes not installed");
            return FALSE;
        }
        return TRUE;
    }

    void Load()
    {
        if (g_loaded) return;

        if (!VerifyCompatibleEngRel())
        {
            Log("sprite_256: skipped; expected original EngRel patch sites were not found");
            return;
        }

        g_block = (BYTE*)VirtualAlloc(NULL, BLOCK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_block == NULL)
        {
            Log("sprite_256: VirtualAlloc failed (%lu)", GetLastError());
            return;
        }

        g_code = g_block;
        g_table = (DWORD*)(g_block + TABLE_OFFSET);

        memcpy(g_code, kSpr8Code, CODE_SIZE);
        memcpy(g_table, kResourceTemplates, sizeof(kResourceTemplates));

        for (DWORD i = 0; i < _countof(kRel32Fixups); ++i)
        {
            const REL32_FIXUP& f = kRel32Fixups[i];
            BYTE* op = g_code + f.offset;
            if (op[0] != 0xE8 && op[0] != 0xE9)
            {
                Log("sprite_256: internal relocation verification failed at +0x%X", f.offset);
                VirtualFree(g_block, 0, MEM_RELEASE);
                g_block = g_code = NULL;
                g_table = NULL;
                return;
            }
            WriteRel32(op, f.target);
        }

        for (DWORD i = 0; i < _countof(kHookSites); ++i)
        {
            const HOOK_SITE& s = kHookSites[i];
            WriteJump(s.address, (DWORD)(g_code + s.code_offset), s.size);
        }

        // Original manager count is five. We now expose seven rows:
        // original five + 128x256 + 256x256.
        *((BYTE*)0x0042B686) = 0x07;
        *((DWORD*)0x0042B68D) = (DWORD)g_table;

        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        g_loaded = TRUE;

        Log("sprite_256: installed 256-colour path + 128x256/256x256 resource classes");
        Log("sprite_256: runtime code=%08X table=%08X; classes=32x32,32x64,64x32,64x64,128x128,128x256,256x256",
            (DWORD)g_code, (DWORD)g_table);
    }

    void Unload()
    {
        if (!g_loaded) return;

        for (DWORD i = 0; i < _countof(kHookSites); ++i)
        {
            const HOOK_SITE& s = kHookSites[i];
            memcpy((void*)s.address, s.original, s.size);
        }

        *((BYTE*)0x0042B686) = 0x05;
        *((DWORD*)0x0042B68D) = 0x004D6DF8;
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);

        if (g_block != NULL) VirtualFree(g_block, 0, MEM_RELEASE);
        g_block = g_code = NULL;
        g_table = NULL;
        g_loaded = FALSE;
    }
}
