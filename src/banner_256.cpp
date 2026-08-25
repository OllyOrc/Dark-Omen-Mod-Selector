// Stage11: zoom-aware banner positioning with owner-stable sprite geometry.
//
// Atlas/resource buckets are per-frame packing details and can change while a
// unit animates. Banner classification must therefore follow the SPR owner and
// native frame geometry, not the current 128x128/128x256/256x256 bucket.
//
// owner+0x08 is the frame count and owner+0x10 is the 0x2c-stride frame table.
// We scan each owner once, read native frame width/height directly from +0x24
// and +0x28, derive a sprite-wide maximum body/top proxy, calculate K from the
// existing 650/1100/1800 curve, and cache that result by owner only.
//
// Body-entry ownership is captured before pushIconDrawRecord loses provenance:
// FUN_00469840 arms the real unit for exactly one push, the default-queue branch
// commits that unit to the final source queue slot, and a post-call cleanup
// always clears the handoff. When buildUnitBodySpriteDrawQueue later creates its
// downstream body-draw record, Hook C propagates only that trusted association;
// it never reads the shared queue's accidental/stale entry+0x1c contents.
//
// A newly discovered non-zero K forces bounds refresh once per genuinely new
// (owner,K,scale) signature. Enlarged sprites use one proportional banner-gap
// rule derived from the validated native-147 reference: effective K per top
// pixel is 3.6503. A further fixed +20px raise is applied only to those Stage11
// enlarged sprites, leaving zero-K vanilla owners completely untouched.
//
// Stage11 state lives for the lifetime of EngRel.exe, while missions can be
// loaded repeatedly without restarting the process. Entry/unit/owner tables are
// therefore LRU-recycled when full and owner caches are revalidated against the
// live frameBase/frameCount. Unit state is pinned only to managed K>0 SPR owners:
// incidental zero-K body owners cannot repeatedly reset an enlarged unit's state.
// Managed owners remain resident until a genuinely different K>0 owner replaces
// them or the UNIT_STATE itself is LRU-recycled; no time-based expiry is used.
//
// Targeted association diagnostics retain the exact ARM -> COMMIT -> PROPAGATE ->
// CAPTURE provenance for each entry mapping. ARM records both the EBP parent unit
// and the current member record's +0x40 owner reference (ownerRef-0x86 trueUnit),
// but diagnostic data does not change which unit receives Stage11 correction.
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
    static const DWORD HOOK_BODY_ARM       = 0x00469C34;
    static const DWORD HOOK_BODY_CLEAR     = 0x00469C3E;
    static const DWORD HOOK_QUEUE_COMMIT   = 0x0045226D;

    static const DWORD RETURN_CLASSIFY     = 0x0042B854;
    static const DWORD RETURN_ENTRY        = 0x0042B986;
    static const DWORD RETURN_RENDER       = 0x00442B4E;
    static const DWORD RETURN_PROJECTION_E = 0x0043FD32;
    static const DWORD RETURN_PROJECTION_F = 0x0043FE2A;
    static const DWORD RETURN_PROJECTION_G = 0x0045042C;
    static const DWORD RETURN_ANCHOR       = 0x004504BF;
    static const DWORD RETURN_BODY_ARM     = 0x00469C39;
    static const DWORD RETURN_BODY_CLEAR   = 0x00469C48;
    static const DWORD RETURN_QUEUE_COMMIT = 0x00452274;

    static const DWORD CALL_PROJECT_BUFFER = 0x00427C30;
    static const DWORD BOUNDS_REFRESH_COOLDOWN = 0x004E4A00;
    static const DWORD UNIT_ARRAY_STRIDE = 0x628;

    static const BYTE kOriginalClassify[5] = { 0x8B,0x6F,0x48,0x85,0xED };
    static const BYTE kOriginalEntry[7] = { 0xC7,0x45,0x10,0x00,0x00,0x00,0x00 };
    static const BYTE kOriginalRender[5] = { 0x57,0x55,0x8B,0x4E,0x04 };
    static const BYTE kOriginalProjectionE[6] = { 0xD9,0x81,0x64,0x01,0x00,0x00 };
    static const BYTE kOriginalProjectionF[6] = { 0xD9,0xC9,0xDE,0xC2,0xD9,0xC9 };
    static const BYTE kOriginalProjectionG[5] = { 0xE8,0x04,0x78,0xFD,0xFF };
    static const BYTE kOriginalAnchor[7] = { 0xA1,0x14,0x37,0x50,0x00,0x2B,0xC2 };
    static const BYTE kOriginalBodyArm[5] = { 0x8D,0x44,0x24,0x1C,0x50 };
    static const BYTE kOriginalBodyClear[5] = { 0x83,0xC4,0x04,0xEB,0x05 };
    static const BYTE kOriginalQueueCommit[7] = { 0xA1,0xE4,0x7D,0x56,0x00,0x03,0xF8 };

    static const float ANCHOR_K_MEDIUM = 650.0f;
    static const float ANCHOR_K_MID = 1100.0f;
    static const float ANCHOR_K_LARGE = 1800.0f;
    static const float CONTINUOUS_TOP_MEDIUM = 113.0f;
    static const float CONTINUOUS_TOP_MID = 150.0f;
    static const float CONTINUOUS_TOP_LARGE = 165.0f;
    static const float MEDIUM_BODY_MIN = 99.0f;
    static const float MEDIUM_TOP_MIN = 110.0f;
    static const float PROPORTIONAL_EFFECTIVE_K_PER_TOP = 3.6503f;
    static const int STAGE11_BANNER_EXTRA_RAISE_PX = 20;

    struct UNIT_STATE
    {
        DWORD unit;
        DWORD resourceWidth;
        DWORD resourceHeight;
        float bodyHeightPx;
        float topExtentPx;
        float calibrationTopPx;
        BOOL hasCalibrationTop;
        DWORD spriteOwner;
        float ownerCachedK;
        float ownerRaiseScale;
        BOOL hasOwnerCachedK;
        BOOL loggedClass;
        BOOL loggedExtent;
        BOOL loggedRaise;
        BOOL loggedRawFrame;
        BOOL loggedOwner;
        BOOL loggedImmediatePatch;
        int lastAppliedRaise;
        DWORD projectionWBits;
        DWORD projectionSequence;
        DWORD lastRefreshOwner;
        float lastRefreshK;
        float lastRefreshScale;
        BOOL hasRefreshSignature;
        DWORD lastTouch;
    };

    struct ENTRY_UNIT_LINK
    {
        DWORD entry;
        DWORD unit;
        DWORD lastTouch;
        DWORD sourceEntry;
        DWORD commitUnit;
        DWORD commitSequence;
        DWORD propagateSequence;
        DWORD armSequence;
        DWORD armParentUnit;
        DWORD armMember;
        DWORD armOwnerRef;
        DWORD armTrueUnit;
    };

    struct OWNER_K_CACHE
    {
        DWORD owner;
        DWORD frameBase;
        DWORD frameCount;
        DWORD maxNativeWidth;
        DWORD maxNativeHeight;
        float maxTopExtentPx;
        float cachedK;
        float raiseScale;
        BOOL valid;
        BOOL logged;
        DWORD lastTouch;
    };

    static UNIT_STATE g_units[400];
    static ENTRY_UNIT_LINK g_entryToUnit[400];
    static OWNER_K_CACHE g_ownerK[256];
    static BYTE* g_caves = NULL;
    static BOOL g_loaded = FALSE;
    static volatile DWORD g_pendingBodyUnit = 0;
    static volatile DWORD g_pendingArmSequence = 0;
    static volatile DWORD g_pendingArmMember = 0;
    static volatile DWORD g_pendingArmOwnerRef = 0;
    static volatile DWORD g_pendingArmTrueUnit = 0;
    static volatile DWORD g_projectionWScratchBits = 0;
    static volatile DWORD g_currentProjectionUnit = 0;
    static volatile DWORD g_lastProjectionUnit = 0;
    static volatile DWORD g_lastProjectionWBits = 0;
    static volatile DWORD g_projectionSequence = 0;
    static DWORD g_touchCounter = 0;
    static DWORD g_assocSequence = 0;
    static DWORD g_firstManagedUnit = 0;

    static float ContinuousAnchorK(float top);
    static float GetAnchorK(const UNIT_STATE* state);
    static float GetRaiseScale(const UNIT_STATE* state);
    static void RefreshCachedAnchor(UNIT_STATE* state);

    static void FlushTrace()
    {
        if (darkomen::detour::traceFile != NULL) fflush(darkomen::detour::traceFile);
    }

    static DWORD NextTouchStamp()
    {
        ++g_touchCounter;
        if (g_touchCounter == 0) ++g_touchCounter;
        return g_touchCounter;
    }

    static DWORD NextAssocSequence()
    {
        ++g_assocSequence;
        if (g_assocSequence == 0) ++g_assocSequence;
        return g_assocSequence;
    }

    static LONG UnitStrideDelta(DWORD unit)
    {
        if (g_firstManagedUnit == 0 || unit == 0) return 0x7FFFFFFF;
        const LONG deltaBytes = (LONG)(unit - g_firstManagedUnit);
        if ((deltaBytes % (LONG)UNIT_ARRAY_STRIDE) != 0) return 0x7FFFFFFF;
        return deltaBytes / (LONG)UNIT_ARRAY_STRIDE;
    }

    static BOOL IsNearManagedUnit(DWORD unit)
    {
        const LONG stride = UnitStrideDelta(unit);
        return stride != 0x7FFFFFFF && stride >= -8 && stride <= 8;
    }

    static void ForceBoundsRefresh(DWORD unit, const char* reason)
    {
        volatile LONG* cooldown = (volatile LONG*)BOUNDS_REFRESH_COOLDOWN;
        const LONG before = *cooldown;
        *cooldown = 0;
        darkomen::detour::trace("Stage11 forceBoundsRefresh unit=%08lX reason=%s cooldown=%ld->0", unit, reason, before);
        FlushTrace();
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
        DWORD freeSlot = _countof(g_units), lruSlot = 0, lruStamp = 0xFFFFFFFF;
        for (DWORD i = 0; i < _countof(g_units); ++i)
        {
            if (g_units[i].unit == unit)
            {
                g_units[i].lastTouch = NextTouchStamp();
                return &g_units[i];
            }
            if (freeSlot == _countof(g_units) && g_units[i].unit == 0) freeSlot = i;
            if (g_units[i].unit != 0 && g_units[i].lastTouch < lruStamp)
            {
                lruStamp = g_units[i].lastTouch;
                lruSlot = i;
            }
        }
        DWORD slot = freeSlot;
        if (slot >= _countof(g_units))
        {
            slot = lruSlot;
            darkomen::detour::trace("Stage11 unitState recycle slot=%lu oldUnit=%08lX newUnit=%08lX", slot, g_units[slot].unit, unit);
            FlushTrace();
        }
        memset(&g_units[slot], 0, sizeof(g_units[slot]));
        g_units[slot].unit = unit;
        g_units[slot].ownerRaiseScale = 1.0f;
        g_units[slot].lastTouch = NextTouchStamp();
        return &g_units[slot];
    }

    static OWNER_K_CACHE* FindOwnerCache(DWORD owner)
    {
        DWORD freeSlot = _countof(g_ownerK), lruSlot = 0, lruStamp = 0xFFFFFFFF;
        for (DWORD i = 0; i < _countof(g_ownerK); ++i)
        {
            if (g_ownerK[i].owner == owner)
            {
                g_ownerK[i].lastTouch = NextTouchStamp();
                return &g_ownerK[i];
            }
            if (freeSlot == _countof(g_ownerK) && g_ownerK[i].owner == 0) freeSlot = i;
            if (g_ownerK[i].owner != 0 && g_ownerK[i].lastTouch < lruStamp)
            {
                lruStamp = g_ownerK[i].lastTouch;
                lruSlot = i;
            }
        }
        DWORD slot = freeSlot;
        if (slot >= _countof(g_ownerK))
        {
            slot = lruSlot;
            darkomen::detour::trace("Stage11 ownerCache recycle slot=%lu oldOwner=%08lX newOwner=%08lX", slot, g_ownerK[slot].owner, owner);
            FlushTrace();
        }
        memset(&g_ownerK[slot], 0, sizeof(g_ownerK[slot]));
        g_ownerK[slot].owner = owner;
        g_ownerK[slot].raiseScale = 1.0f;
        g_ownerK[slot].lastTouch = NextTouchStamp();
        return &g_ownerK[slot];
    }

    static OWNER_K_CACHE* ScanOwnerK(DWORD owner)
    {
        if (owner == 0) return NULL;
        OWNER_K_CACHE* cache = FindOwnerCache(owner);
        if (cache == NULL) return NULL;
        const DWORD frameCount = *((DWORD*)(owner + 0x08));
        const DWORD frameBase = *((DWORD*)(owner + 0x10));
        if (frameCount == 0 || frameCount > 4096 || frameBase == 0) return NULL;
        if (cache->valid)
        {
            if (cache->frameBase == frameBase && cache->frameCount == frameCount) return cache;
            darkomen::detour::trace("Stage11 ownerCache invalidate owner=%08lX oldFrameBase=%08lX oldFrames=%lu newFrameBase=%08lX newFrames=%lu", owner, cache->frameBase, cache->frameCount, frameBase, frameCount);
            FlushTrace();
            const DWORD touch = cache->lastTouch;
            memset(cache, 0, sizeof(*cache));
            cache->owner = owner;
            cache->raiseScale = 1.0f;
            cache->lastTouch = touch;
        }
        DWORD maxWidth = 0, maxHeight = 0;
        float maxTop = 0.0f;
        for (DWORD i = 0; i < frameCount; ++i)
        {
            const DWORD frameRecord = frameBase + (i * 0x2C);
            const DWORD nativeWidth = *((DWORD*)(frameRecord + 0x24));
            const DWORD nativeHeight = *((DWORD*)(frameRecord + 0x28));
            float yOffsetRatio = *((float*)(frameRecord + 0x1C));
            if (yOffsetRatio < 0.0f) yOffsetRatio = -yOffsetRatio;
            if (nativeWidth == 0 || nativeWidth >= 1024 || nativeHeight == 0 || nativeHeight >= 1024) continue;
            const float topExtent = yOffsetRatio * (float)nativeHeight;
            if (!(topExtent > 0.0f && topExtent < 1024.0f)) continue;
            if (nativeWidth > maxWidth) maxWidth = nativeWidth;
            if (nativeHeight > maxHeight) maxHeight = nativeHeight;
            if (topExtent > maxTop) maxTop = topExtent;
        }
        float k = 0.0f;
        if (maxHeight >= (DWORD)MEDIUM_BODY_MIN && maxTop >= MEDIUM_TOP_MIN) k = ContinuousAnchorK(maxTop);
        float raiseScale = 1.0f;
        if (k > 0.0f && maxTop > 0.0f) raiseScale = (maxTop * PROPORTIONAL_EFFECTIVE_K_PER_TOP) / k;
        cache->frameBase = frameBase;
        cache->frameCount = frameCount;
        cache->maxNativeWidth = maxWidth;
        cache->maxNativeHeight = maxHeight;
        cache->maxTopExtentPx = maxTop;
        cache->cachedK = k;
        cache->raiseScale = raiseScale;
        cache->valid = TRUE;
        if (!cache->logged)
        {
            darkomen::detour::trace("Stage11 ownerScan owner=%08lX frameBase=%08lX frames=%lu maxNative=%lux%lu maxTop=%.1f K=%.1f scale=%.2f", owner, frameBase, frameCount, maxWidth, maxHeight, maxTop, k, raiseScale);
            FlushTrace();
            cache->logged = TRUE;
        }
        return cache;
    }

    static void __cdecl MarkUnitClass(DWORD unit, DWORD templateEntry)
    {
        if (unit == 0 || templateEntry == 0) return;
        const DWORD width = *((DWORD*)(templateEntry + 0x18));
        const DWORD height = *((DWORD*)(templateEntry + 0x1C));
        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return;
        const BOOL changed = (state->resourceWidth != width || state->resourceHeight != height);
        state->resourceWidth = width;
        state->resourceHeight = height;
        if (changed && !state->loggedClass)
        {
            darkomen::detour::trace("Stage11 resourceClass observed unit=%08lX template=%08lX width=%lu height=%lu (owner K preserved)", unit, templateEntry, width, height);
            FlushTrace();
            state->loggedClass = TRUE;
        }
    }

    static ENTRY_UNIT_LINK* FindEntryLink(DWORD entry)
    {
        if (entry == 0) return NULL;
        for (DWORD i = 0; i < _countof(g_entryToUnit); ++i)
        {
            if (g_entryToUnit[i].entry == entry)
            {
                g_entryToUnit[i].lastTouch = NextTouchStamp();
                return &g_entryToUnit[i];
            }
        }
        return NULL;
    }

    static ENTRY_UNIT_LINK* RecordEntryUnit(DWORD entry, DWORD unit)
    {
        if (entry == 0 || unit == 0) return NULL;
        DWORD freeSlot = _countof(g_entryToUnit), lruSlot = 0, lruStamp = 0xFFFFFFFF;
        for (DWORD i = 0; i < _countof(g_entryToUnit); ++i)
        {
            if (g_entryToUnit[i].entry == entry)
            {
                g_entryToUnit[i].unit = unit;
                g_entryToUnit[i].lastTouch = NextTouchStamp();
                return &g_entryToUnit[i];
            }
            if (freeSlot == _countof(g_entryToUnit) && g_entryToUnit[i].entry == 0) freeSlot = i;
            if (g_entryToUnit[i].entry != 0 && g_entryToUnit[i].lastTouch < lruStamp)
            {
                lruStamp = g_entryToUnit[i].lastTouch;
                lruSlot = i;
            }
        }
        DWORD slot = freeSlot;
        if (slot >= _countof(g_entryToUnit))
        {
            slot = lruSlot;
            darkomen::detour::trace("Stage11 entryMap recycle slot=%lu oldEntry=%08lX oldUnit=%08lX newEntry=%08lX newUnit=%08lX", slot, g_entryToUnit[slot].entry, g_entryToUnit[slot].unit, entry, unit);
            FlushTrace();
        }
        memset(&g_entryToUnit[slot], 0, sizeof(g_entryToUnit[slot]));
        g_entryToUnit[slot].entry = entry;
        g_entryToUnit[slot].unit = unit;
        g_entryToUnit[slot].lastTouch = NextTouchStamp();
        return &g_entryToUnit[slot];
    }

    static void ClearPendingArm()
    {
        g_pendingBodyUnit = 0;
        g_pendingArmSequence = 0;
        g_pendingArmMember = 0;
        g_pendingArmOwnerRef = 0;
        g_pendingArmTrueUnit = 0;
    }

    static void __cdecl ArmBodyUnit(DWORD parentUnit, DWORD memberRecord)
    {
        ClearPendingArm();
        if (parentUnit == 0) return;

        DWORD ownerRef = 0;
        DWORD trueUnit = 0;
        if (memberRecord >= 0x10000)
        {
            ownerRef = *((DWORD*)(memberRecord + 0x40));
            if (ownerRef >= 0x86)
                trueUnit = ownerRef - 0x86;
        }

        g_pendingBodyUnit = parentUnit;
        g_pendingArmSequence = NextAssocSequence();
        g_pendingArmMember = memberRecord;
        g_pendingArmOwnerRef = ownerRef;
        g_pendingArmTrueUnit = trueUnit;

        if ((g_firstManagedUnit != 0 &&
             (IsNearManagedUnit(parentUnit) || IsNearManagedUnit(trueUnit))) ||
            (trueUnit != 0 && trueUnit != parentUnit))
        {
            darkomen::detour::trace(
                "Stage11 assoc ARM seq=%lu parentUnit=%08lX parentStride=%ld member=%08lX ownerRef=%08lX trueUnit=%08lX trueStride=%ld",
                (DWORD)g_pendingArmSequence, parentUnit, UnitStrideDelta(parentUnit),
                memberRecord, ownerRef, trueUnit, UnitStrideDelta(trueUnit));
            FlushTrace();
        }
    }

    static void __cdecl CommitBodyEntry(DWORD entry)
    {
        const DWORD unit = g_pendingBodyUnit;
        const DWORD armSequence = g_pendingArmSequence;
        const DWORD armMember = g_pendingArmMember;
        const DWORD armOwnerRef = g_pendingArmOwnerRef;
        const DWORD armTrueUnit = g_pendingArmTrueUnit;
        ClearPendingArm();
        if (entry == 0 || unit == 0) return;
        ENTRY_UNIT_LINK* link = RecordEntryUnit(entry, unit);
        if (link == NULL) return;
        link->sourceEntry = entry;
        link->commitUnit = unit;
        link->commitSequence = NextAssocSequence();
        link->propagateSequence = 0;
        link->armSequence = armSequence;
        link->armParentUnit = unit;
        link->armMember = armMember;
        link->armOwnerRef = armOwnerRef;
        link->armTrueUnit = armTrueUnit;
        if (IsNearManagedUnit(unit) || IsNearManagedUnit(armTrueUnit))
        {
            darkomen::detour::trace(
                "Stage11 assoc COMMIT seq=%lu entry=%08lX unit=%08lX stride=%ld armSeq=%lu member=%08lX trueUnit=%08lX trueStride=%ld",
                link->commitSequence, entry, unit, UnitStrideDelta(unit),
                link->armSequence, link->armMember, link->armTrueUnit,
                UnitStrideDelta(link->armTrueUnit));
            FlushTrace();
        }
    }

    static void __cdecl ClearPendingBodyUnit() { ClearPendingArm(); }

    static DWORD FindUnitForEntry(DWORD entry)
    {
        ENTRY_UNIT_LINK* link = FindEntryLink(entry);
        return (link != NULL) ? link->unit : 0;
    }

    static void __cdecl MarkTrustedUnitClass(DWORD sourceEntry, DWORD templateEntry)
    {
        const DWORD unit = FindUnitForEntry(sourceEntry);
        if (unit != 0) MarkUnitClass(unit, templateEntry);
    }

    static void __cdecl PropagateTrustedEntry(DWORD bodyEntry, DWORD sourceEntry)
    {
        ENTRY_UNIT_LINK* source = FindEntryLink(sourceEntry);
        const DWORD unit = (source != NULL) ? source->unit : 0;
        if (unit == 0 || bodyEntry == 0) return;
        ENTRY_UNIT_LINK* body = RecordEntryUnit(bodyEntry, unit);
        if (body == NULL) return;
        body->sourceEntry = sourceEntry;
        body->commitUnit = (source->commitUnit != 0) ? source->commitUnit : source->unit;
        body->commitSequence = source->commitSequence;
        body->propagateSequence = NextAssocSequence();
        body->armSequence = source->armSequence;
        body->armParentUnit = source->armParentUnit;
        body->armMember = source->armMember;
        body->armOwnerRef = source->armOwnerRef;
        body->armTrueUnit = source->armTrueUnit;
        if (IsNearManagedUnit(unit) || IsNearManagedUnit(body->armTrueUnit))
        {
            darkomen::detour::trace(
                "Stage11 assoc PROP seq=%lu source=%08lX mappedUnit=%08lX stride=%ld body=%08lX commitSeq=%lu commitUnit=%08lX armSeq=%lu parent=%08lX member=%08lX trueUnit=%08lX trueStride=%ld",
                body->propagateSequence, sourceEntry, unit, UnitStrideDelta(unit),
                bodyEntry, body->commitSequence, body->commitUnit,
                body->armSequence, body->armParentUnit, body->armMember,
                body->armTrueUnit, UnitStrideDelta(body->armTrueUnit));
            FlushTrace();
        }
    }

    static void ResetUnitStateForOwnerReuse(UNIT_STATE* state, DWORD newOwner)
    {
        if (state == NULL) return;
        const DWORD unit = state->unit;
        const DWORD resourceWidth = state->resourceWidth;
        const DWORD resourceHeight = state->resourceHeight;
        const DWORD oldOwner = state->spriteOwner;
        const DWORD touch = state->lastTouch;
        darkomen::detour::trace("Stage11 unitState managedOwnerReuse unit=%08lX oldOwner=%08lX newOwner=%08lX", unit, oldOwner, newOwner);
        FlushTrace();
        memset(state, 0, sizeof(*state));
        state->unit = unit;
        state->resourceWidth = resourceWidth;
        state->resourceHeight = resourceHeight;
        state->ownerRaiseScale = 1.0f;
        state->lastTouch = touch;
    }

    static void __cdecl CaptureBodyTopExtent(DWORD entry)
    {
        ENTRY_UNIT_LINK* bodyLink = FindEntryLink(entry);
        const DWORD unit = (bodyLink != NULL) ? bodyLink->unit : 0;
        if (unit == 0 || entry == 0) return;
        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return;
        const DWORD owner = *((DWORD*)entry);
        if (owner == 0) return;
        const DWORD frameBase = *((DWORD*)(owner + 0x10));
        if (frameBase == 0) return;
        const LONG frameIndex = *((LONG*)(entry + 0x04));
        if (frameIndex < 0 || frameIndex > 4096) return;
        OWNER_K_CACHE* ownerCache = ScanOwnerK(owner);
        if (ownerCache == NULL || !ownerCache->valid) return;
        if (ownerCache->cachedK <= 0.0f) return;

        if (g_firstManagedUnit == 0) g_firstManagedUnit = unit;
        const DWORD captureSequence = NextAssocSequence();
        darkomen::detour::trace(
            "Stage11 assoc CAPTURE seq=%lu body=%08lX mappedUnit=%08lX stride=%ld owner=%08lX source=%08lX commitSeq=%lu commitUnit=%08lX propSeq=%lu armSeq=%lu parent=%08lX parentStride=%ld member=%08lX ownerRef=%08lX trueUnit=%08lX trueStride=%ld",
            captureSequence, entry, unit, UnitStrideDelta(unit), owner,
            (bodyLink != NULL) ? bodyLink->sourceEntry : 0,
            (bodyLink != NULL) ? bodyLink->commitSequence : 0,
            (bodyLink != NULL) ? bodyLink->commitUnit : 0,
            (bodyLink != NULL) ? bodyLink->propagateSequence : 0,
            (bodyLink != NULL) ? bodyLink->armSequence : 0,
            (bodyLink != NULL) ? bodyLink->armParentUnit : 0,
            (bodyLink != NULL) ? UnitStrideDelta(bodyLink->armParentUnit) : 0x7FFFFFFF,
            (bodyLink != NULL) ? bodyLink->armMember : 0,
            (bodyLink != NULL) ? bodyLink->armOwnerRef : 0,
            (bodyLink != NULL) ? bodyLink->armTrueUnit : 0,
            (bodyLink != NULL) ? UnitStrideDelta(bodyLink->armTrueUnit) : 0x7FFFFFFF);
        FlushTrace();

        if (state->spriteOwner != 0 && state->spriteOwner != owner)
            ResetUnitStateForOwnerReuse(state, owner);

        state->spriteOwner = owner;
        state->ownerCachedK = ownerCache->cachedK;
        state->ownerRaiseScale = ownerCache->raiseScale;
        state->hasOwnerCachedK = TRUE;
        state->bodyHeightPx = (float)ownerCache->maxNativeHeight;
        state->topExtentPx = ownerCache->maxTopExtentPx;

        if (!state->loggedOwner)
        {
            const DWORD* o = (const DWORD*)owner;
            darkomen::detour::trace("Stage11 ownerRaw unit=%08lX owner=%08lX frameBase=%08lX frame=%ld o00=%08lX o04=%08lX o08=%08lX o0C=%08lX o10=%08lX o14=%08lX o18=%08lX o1C=%08lX o20=%08lX o24=%08lX o28=%08lX", unit, owner, frameBase, frameIndex, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10]);
            FlushTrace();
            state->loggedOwner = TRUE;
        }

        const DWORD frameRecord = frameBase + ((DWORD)frameIndex * 0x2C);
        if (!state->loggedRawFrame)
        {
            const DWORD* raw = (const DWORD*)frameRecord;
            darkomen::detour::trace("Stage11 frameRaw unit=%08lX resource=%lux%lu frame=%ld rec=%08lX d00=%08lX d04=%08lX d08=%08lX d0C=%08lX d10=%08lX d14=%08lX d18=%08lX d1C=%08lX d20=%08lX d24=%08lX d28=%08lX", unit, state->resourceWidth, state->resourceHeight, frameIndex, frameRecord, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10]);
            FlushTrace();
            state->loggedRawFrame = TRUE;
        }

        const DWORD nativeHeight = *((DWORD*)(frameRecord + 0x28));
        float yOffsetRatio = *((float*)(frameRecord + 0x1C));
        if (yOffsetRatio < 0.0f) yOffsetRatio = -yOffsetRatio;
        if (nativeHeight > 0 && nativeHeight < 1024)
        {
            const float topExtent = yOffsetRatio * (float)nativeHeight;
            if (topExtent > 0.0f && topExtent < 1024.0f)
            {
                state->calibrationTopPx = topExtent;
                state->hasCalibrationTop = TRUE;
            }
        }
        if (!state->loggedExtent)
        {
            darkomen::detour::trace("Stage11 bodyProxy unit=%08lX resource=%lux%lu bodyH=%.1f top=%.1f calibrationTop=%.1f owner=%08lX ownerK=%.1f scale=%.2f", unit, state->resourceWidth, state->resourceHeight, state->bodyHeightPx, state->topExtentPx, state->calibrationTopPx, state->spriteOwner, state->ownerCachedK, state->ownerRaiseScale);
            FlushTrace();
            state->loggedExtent = TRUE;
        }

        const float newK = GetAnchorK(state);
        const float newScale = GetRaiseScale(state);
        const BOOL refreshChanged = !state->hasRefreshSignature || state->lastRefreshOwner != state->spriteOwner || state->lastRefreshK != newK || state->lastRefreshScale != newScale;
        if (newK > 0.0f && refreshChanged)
        {
            state->lastRefreshOwner = state->spriteOwner;
            state->lastRefreshK = newK;
            state->lastRefreshScale = newScale;
            state->hasRefreshSignature = TRUE;
            state->loggedRaise = FALSE;
            state->loggedImmediatePatch = FALSE;
            ForceBoundsRefresh(unit, "ownerGeometryChanged");
        }
        RefreshCachedAnchor(state);
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
    static void __cdecl RecordProjectionW(DWORD unit) { PublishProjectionSample(unit); }

    static float ContinuousAnchorK(float top)
    {
        if (top <= CONTINUOUS_TOP_MEDIUM) return ANCHOR_K_MEDIUM;
        if (top < CONTINUOUS_TOP_MID)
        {
            const float scale = (top - CONTINUOUS_TOP_MEDIUM) / (CONTINUOUS_TOP_MID - CONTINUOUS_TOP_MEDIUM);
            return ANCHOR_K_MEDIUM + scale * (ANCHOR_K_MID - ANCHOR_K_MEDIUM);
        }
        if (top < CONTINUOUS_TOP_LARGE)
        {
            const float scale = (top - CONTINUOUS_TOP_MID) / (CONTINUOUS_TOP_LARGE - CONTINUOUS_TOP_MID);
            return ANCHOR_K_MID + scale * (ANCHOR_K_LARGE - ANCHOR_K_MID);
        }
        return ANCHOR_K_LARGE;
    }

    static float GetAnchorK(const UNIT_STATE* state) { return (state == NULL || !state->hasOwnerCachedK) ? 0.0f : state->ownerCachedK; }
    static float GetRaiseScale(const UNIT_STATE* state) { return (state == NULL || !state->hasOwnerCachedK) ? 1.0f : state->ownerRaiseScale; }

    static int CalculateRaise(const UNIT_STATE* state)
    {
        if (state == NULL || state->projectionWBits == 0) return 0;
        const float anchorK = GetAnchorK(state);
        if (anchorK <= 0.0f) return 0;
        union FLOAT_BITS { DWORD bits; float value; } wBits;
        wBits.bits = state->projectionWBits;
        const float w = wBits.value;
        const float absW = (w < 0.0f) ? -w : w;
        if (!(absW > 0.0001f && absW < 1000000.0f)) return 0;
        const float raiseScale = GetRaiseScale(state);
        const int proportionalRaise = (int)(((anchorK / absW) * raiseScale) + 0.5f);
        const int raise = proportionalRaise + STAGE11_BANNER_EXTRA_RAISE_PX;
        return (raise > 0 && raise < 1024) ? raise : 0;
    }

    static void RefreshCachedAnchor(UNIT_STATE* state)
    {
        if (state == NULL || state->unit == 0) return;
        const int desiredRaise = CalculateRaise(state);
        const int delta = desiredRaise - state->lastAppliedRaise;
        if (delta == 0) return;
        LONG* cachedY = (LONG*)(state->unit + 0x6C);
        const LONG before = *cachedY;
        if (before < -8192 || before > 8192) return;
        *cachedY = before - delta;
        state->lastAppliedRaise = desiredRaise;
        if (!state->loggedImmediatePatch)
        {
            darkomen::detour::trace("Stage11 immediateAnchorPatch unit=%08lX oldRaise=%d newRaise=%d delta=%d y=%ld->%ld", state->unit, desiredRaise - delta, desiredRaise, delta, before, *cachedY);
            FlushTrace();
            state->loggedImmediatePatch = TRUE;
        }
    }

    static int __cdecl GetUnitAnchorRaise(DWORD unit)
    {
        UNIT_STATE* state = FindOrCreateUnitState(unit);
        if (state == NULL) return 0;
        const DWORD sequence = g_projectionSequence;
        if (g_lastProjectionUnit == unit && g_lastProjectionWBits != 0 && sequence != state->projectionSequence)
        {
            state->projectionWBits = g_lastProjectionWBits;
            state->projectionSequence = sequence;
        }
        const float anchorK = GetAnchorK(state);
        if (anchorK <= 0.0f || state->projectionWBits == 0)
        {
            state->lastAppliedRaise = 0;
            return 0;
        }
        union FLOAT_BITS { DWORD bits; float value; } wBits;
        wBits.bits = state->projectionWBits;
        const float w = wBits.value;
        const float absW = (w < 0.0f) ? -w : w;
        if (!(absW > 0.0001f && absW < 1000000.0f))
        {
            state->lastAppliedRaise = 0;
            return 0;
        }
        const float raiseScale = GetRaiseScale(state);
        const int proportionalRaise = (int)(((anchorK / absW) * raiseScale) + 0.5f);
        const int raise = proportionalRaise + STAGE11_BANNER_EXTRA_RAISE_PX;
        const int safeRaise = (raise > 0 && raise < 1024) ? raise : 0;
        state->lastAppliedRaise = safeRaise;
        if (safeRaise > 0 && !state->loggedRaise)
        {
            darkomen::detour::trace("Stage11 anchorRaiseK unit=%08lX resource=%lux%lu bodyH=%.1f top=%.1f calibrationTop=%.1f owner=%08lX ownerK=%.1f W=%.6f K=%.1f scale=%.2f raise=%d", unit, state->resourceWidth, state->resourceHeight, state->bodyHeightPx, state->topExtentPx, state->calibrationTopPx, state->spriteOwner, state->ownerCachedK, w, anchorK, raiseScale, safeRaise);
            FlushTrace();
            state->loggedRaise = TRUE;
        }
        return safeRaise;
    }

    static BOOL BuildCaves()
    {
        g_caves = (BYTE*)VirtualAlloc(NULL, 0x300, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (g_caves == NULL) return FALSE;
        BYTE* a = g_caves + 0x00; DWORD n = 0;
        a[n++] = 0x60; a[n++] = 0x8B; a[n++] = 0x44; a[n++] = 0x24; a[n++] = 0x30; a[n++] = 0x57; a[n++] = 0x50;
        DWORD callA = n; a[n++] = 0xE8; n += 4; a[n++] = 0x83; a[n++] = 0xC4; a[n++] = 0x08; a[n++] = 0x61; a[n++] = 0x8B; a[n++] = 0x6F; a[n++] = 0x48; a[n++] = 0x85; a[n++] = 0xED;
        DWORD jumpA = n; a[n++] = 0xE9; n += 4; WriteRel32(a + callA, (DWORD)&MarkTrustedUnitClass); WriteRel32(a + jumpA, RETURN_CLASSIFY);
        BYTE* c = g_caves + 0x40; n = 0;
        c[n++] = 0xC7; c[n++] = 0x45; c[n++] = 0x10; *((DWORD*)(c + n)) = 0; n += 4; c[n++] = 0x9C; c[n++] = 0x60; c[n++] = 0x8B; c[n++] = 0x44; c[n++] = 0x24; c[n++] = 0x34; c[n++] = 0x50; c[n++] = 0x55;
        DWORD callC = n; c[n++] = 0xE8; n += 4; c[n++] = 0x83; c[n++] = 0xC4; c[n++] = 0x08; c[n++] = 0x61; c[n++] = 0x9D; DWORD jumpC = n; c[n++] = 0xE9; n += 4; WriteRel32(c + callC, (DWORD)&PropagateTrustedEntry); WriteRel32(c + jumpC, RETURN_ENTRY);
        BYTE* d = g_caves + 0x80; n = 0;
        d[n++] = 0x57; d[n++] = 0x55; d[n++] = 0x8B; d[n++] = 0x4E; d[n++] = 0x04; d[n++] = 0x9C; d[n++] = 0x60; d[n++] = 0x56; DWORD callD = n; d[n++] = 0xE8; n += 4; d[n++] = 0x83; d[n++] = 0xC4; d[n++] = 0x04; d[n++] = 0x61; d[n++] = 0x9D; DWORD jumpD = n; d[n++] = 0xE9; n += 4; WriteRel32(d + callD, (DWORD)&CaptureBodyTopExtent); WriteRel32(d + jumpD, RETURN_RENDER);
        BYTE* b = g_caves + 0xC0; n = 0;
        b[n++] = 0xA1; *((DWORD*)(b + n)) = 0x00503714; n += 4; b[n++] = 0x2B; b[n++] = 0xC2; b[n++] = 0x9C; b[n++] = 0x51; b[n++] = 0x52; b[n++] = 0x50; b[n++] = 0x56; DWORD callB = n; b[n++] = 0xE8; n += 4; b[n++] = 0x83; b[n++] = 0xC4; b[n++] = 0x04; b[n++] = 0x8B; b[n++] = 0xD0; b[n++] = 0x58; b[n++] = 0x2B; b[n++] = 0xC2; b[n++] = 0x5A; b[n++] = 0x59; b[n++] = 0x9D; DWORD jumpB = n; b[n++] = 0xE9; n += 4; WriteRel32(b + callB, (DWORD)&GetUnitAnchorRaise); WriteRel32(b + jumpB, RETURN_ANCHOR);
        BYTE* e = g_caves + 0x100; n = 0;
        e[n++] = 0xD9; e[n++] = 0x15; *((DWORD*)(e + n)) = (DWORD)&g_projectionWScratchBits; n += 4; e[n++] = 0xD9; e[n++] = 0x81; e[n++] = 0x64; e[n++] = 0x01; e[n++] = 0x00; e[n++] = 0x00; e[n++] = 0x9C; e[n++] = 0x60; e[n++] = 0x56; DWORD callE = n; e[n++] = 0xE8; n += 4; e[n++] = 0x83; e[n++] = 0xC4; e[n++] = 0x04; e[n++] = 0x61; e[n++] = 0x9D; DWORD jumpE = n; e[n++] = 0xE9; n += 4; WriteRel32(e + callE, (DWORD)&RecordProjectionW); WriteRel32(e + jumpE, RETURN_PROJECTION_E);
        BYTE* f = g_caves + 0x140; n = 0;
        f[n++] = 0xD9; f[n++] = 0x15; *((DWORD*)(f + n)) = (DWORD)&g_projectionWScratchBits; n += 4; f[n++] = 0xD9; f[n++] = 0xC9; f[n++] = 0xDE; f[n++] = 0xC2; f[n++] = 0xD9; f[n++] = 0xC9; f[n++] = 0x9C; f[n++] = 0x60; f[n++] = 0x56; DWORD callF = n; f[n++] = 0xE8; n += 4; f[n++] = 0x83; f[n++] = 0xC4; f[n++] = 0x04; f[n++] = 0x61; f[n++] = 0x9D; DWORD jumpF = n; f[n++] = 0xE9; n += 4; WriteRel32(f + callF, (DWORD)&RecordProjectionW); WriteRel32(f + jumpF, RETURN_PROJECTION_F);
        BYTE* g = g_caves + 0x180; n = 0;
        g[n++] = 0x89; g[n++] = 0x35; *((DWORD*)(g + n)) = (DWORD)&g_currentProjectionUnit; n += 4; DWORD callG = n; g[n++] = 0xE8; n += 4; DWORD jumpG = n; g[n++] = 0xE9; n += 4; WriteRel32(g + callG, CALL_PROJECT_BUFFER); WriteRel32(g + jumpG, RETURN_PROJECTION_G);
        BYTE* arm = g_caves + 0x1C0; n = 0;
        arm[n++] = 0x8D; arm[n++] = 0x44; arm[n++] = 0x24; arm[n++] = 0x1C; arm[n++] = 0x50; arm[n++] = 0x9C; arm[n++] = 0x60; arm[n++] = 0x56; arm[n++] = 0x55; DWORD callArm = n; arm[n++] = 0xE8; n += 4; arm[n++] = 0x83; arm[n++] = 0xC4; arm[n++] = 0x08; arm[n++] = 0x61; arm[n++] = 0x9D; DWORD jumpArm = n; arm[n++] = 0xE9; n += 4; WriteRel32(arm + callArm, (DWORD)&ArmBodyUnit); WriteRel32(arm + jumpArm, RETURN_BODY_ARM);
        BYTE* clear = g_caves + 0x200; n = 0;
        clear[n++] = 0x83; clear[n++] = 0xC4; clear[n++] = 0x04; clear[n++] = 0x9C; clear[n++] = 0x60; DWORD callClear = n; clear[n++] = 0xE8; n += 4; clear[n++] = 0x61; clear[n++] = 0x9D; DWORD jumpClear = n; clear[n++] = 0xE9; n += 4; WriteRel32(clear + callClear, (DWORD)&ClearPendingBodyUnit); WriteRel32(clear + jumpClear, RETURN_BODY_CLEAR);
        BYTE* commit = g_caves + 0x240; n = 0;
        commit[n++] = 0xA1; *((DWORD*)(commit + n)) = 0x00567DE4; n += 4; commit[n++] = 0x03; commit[n++] = 0xF8; commit[n++] = 0x9C; commit[n++] = 0x60; commit[n++] = 0x57; DWORD callCommit = n; commit[n++] = 0xE8; n += 4; commit[n++] = 0x83; commit[n++] = 0xC4; commit[n++] = 0x04; commit[n++] = 0x61; commit[n++] = 0x9D; DWORD jumpCommit = n; commit[n++] = 0xE9; n += 4; WriteRel32(commit + callCommit, (DWORD)&CommitBodyEntry); WriteRel32(commit + jumpCommit, RETURN_QUEUE_COMMIT);
        FlushInstructionCache(GetCurrentProcess(), g_caves, 0x300);
        return TRUE;
    }

    void Load()
    {
        if (g_loaded) return;
        if (!BytesEqual(HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify)) || !BytesEqual(HOOK_ENTRY, kOriginalEntry, sizeof(kOriginalEntry)) || !BytesEqual(HOOK_RENDER, kOriginalRender, sizeof(kOriginalRender)) || !BytesEqual(HOOK_PROJECTION_E, kOriginalProjectionE, sizeof(kOriginalProjectionE)) || !BytesEqual(HOOK_PROJECTION_F, kOriginalProjectionF, sizeof(kOriginalProjectionF)) || !BytesEqual(HOOK_PROJECTION_G, kOriginalProjectionG, sizeof(kOriginalProjectionG)) || !BytesEqual(HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor)) || !BytesEqual(HOOK_BODY_ARM, kOriginalBodyArm, sizeof(kOriginalBodyArm)) || !BytesEqual(HOOK_BODY_CLEAR, kOriginalBodyClear, sizeof(kOriginalBodyClear)) || !BytesEqual(HOOK_QUEUE_COMMIT, kOriginalQueueCommit, sizeof(kOriginalQueueCommit)))
        {
            darkomen::detour::trace("Stage11 install FAIL byte guard");
            FlushTrace();
            return;
        }
        memset(g_units, 0, sizeof(g_units));
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));
        memset(g_ownerK, 0, sizeof(g_ownerK));
        ClearPendingArm();
        g_projectionWScratchBits = 0;
        g_currentProjectionUnit = 0;
        g_lastProjectionUnit = 0;
        g_lastProjectionWBits = 0;
        g_projectionSequence = 0;
        g_touchCounter = 0;
        g_assocSequence = 0;
        g_firstManagedUnit = 0;
        if (!BuildCaves()) return;
        WriteJump(HOOK_CLASSIFY, (DWORD)(g_caves + 0x00), 5);
        WriteJump(HOOK_ENTRY, (DWORD)(g_caves + 0x40), 7);
        WriteJump(HOOK_RENDER, (DWORD)(g_caves + 0x80), 5);
        WriteJump(HOOK_PROJECTION_E, (DWORD)(g_caves + 0x100), 6);
        WriteJump(HOOK_PROJECTION_F, (DWORD)(g_caves + 0x140), 6);
        WriteJump(HOOK_PROJECTION_G, (DWORD)(g_caves + 0x180), 5);
        WriteJump(HOOK_ANCHOR, (DWORD)(g_caves + 0xC0), 7);
        WriteJump(HOOK_BODY_ARM, (DWORD)(g_caves + 0x1C0), 5);
        WriteJump(HOOK_BODY_CLEAR, (DWORD)(g_caves + 0x200), 5);
        WriteJump(HOOK_QUEUE_COMMIT, (DWORD)(g_caves + 0x240), 7);
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        g_loaded = TRUE;
        darkomen::detour::trace("Stage11 installed: trusted source-to-body association + owner-only native K + proportional 3.6503 effective-K/top spacing + 20px enlarged-sprite lift + refresh-signature suppression + lifecycle LRU recycling + managed-owner churn suppression + no managed-owner timeout + ARM member-owner back-pointer diagnostics");
        FlushTrace();
    }

    void Unload()
    {
        if (!g_loaded) return;
        memcpy((void*)HOOK_CLASSIFY, kOriginalClassify, sizeof(kOriginalClassify));
        memcpy((void*)HOOK_ENTRY, kOriginalEntry, sizeof(kOriginalEntry));
        memcpy((void*)HOOK_RENDER, kOriginalRender, sizeof(kOriginalRender));
        memcpy((void*)HOOK_PROJECTION_E, kOriginalProjectionE, sizeof(kOriginalProjectionE));
        memcpy((void*)HOOK_PROJECTION_F, kOriginalProjectionF, sizeof(kOriginalProjectionF));
        memcpy((void*)HOOK_PROJECTION_G, kOriginalProjectionG, sizeof(kOriginalProjectionG));
        memcpy((void*)HOOK_ANCHOR, kOriginalAnchor, sizeof(kOriginalAnchor));
        memcpy((void*)HOOK_BODY_ARM, kOriginalBodyArm, sizeof(kOriginalBodyArm));
        memcpy((void*)HOOK_BODY_CLEAR, kOriginalBodyClear, sizeof(kOriginalBodyClear));
        memcpy((void*)HOOK_QUEUE_COMMIT, kOriginalQueueCommit, sizeof(kOriginalQueueCommit));
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
        if (g_caves != NULL) VirtualFree(g_caves, 0, MEM_RELEASE);
        g_caves = NULL;
        memset(g_units, 0, sizeof(g_units));
        memset(g_entryToUnit, 0, sizeof(g_entryToUnit));
        memset(g_ownerK, 0, sizeof(g_ownerK));
        ClearPendingArm();
        g_projectionWScratchBits = 0;
        g_currentProjectionUnit = 0;
        g_lastProjectionUnit = 0;
        g_lastProjectionWBits = 0;
        g_projectionSequence = 0;
        g_touchCounter = 0;
        g_assocSequence = 0;
        g_firstManagedUnit = 0;
        g_loaded = FALSE;
    }
}
