#include "cascade_patch.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace CascadePatch
{
    // =========================================================================
    // State
    // =========================================================================
    static uintptr_t g_moduleBase = 0;
    static volatile long g_logInitialized = 0;
    static volatile long g_textDecrypted = 0;  // SteamStub decryption detected
    static volatile long g_countReadsPatched = 0; // MOV instructions patched to load 4
    static volatile long g_shaderPatched = 0;  // Shader constructor patched
    static volatile long g_maskSafe = 0;       // Mask writer patched to 0x3 (safe mode)
    static volatile long g_vrExpanded = 0;     // VR array expanded to 4
    static volatile long g_maskRestored = 0;   // Mask writer restored to full rotation
    static volatile long g_timerStarted = 0;   // Expansion timer created
    static HANDLE g_timerHandle = nullptr;      // Timer queue timer handle
    static FILE* g_logFile = nullptr;
    static CRITICAL_SECTION g_logLock;
    static bool g_logLockReady = false;

    // =========================================================================
    // Logging
    // =========================================================================
    void Log(const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        if (g_logLockReady) EnterCriticalSection(&g_logLock);

        OutputDebugStringA("[VRShadowCascade] ");
        OutputDebugStringA(buffer);
        OutputDebugStringA("\n");

        if (g_logFile) {
            fprintf(g_logFile, "%s\n", buffer);
            fflush(g_logFile);
        }

        if (g_logLockReady) LeaveCriticalSection(&g_logLock);
    }

    uintptr_t GetModuleBase()
    {
        if (g_moduleBase == 0) {
            g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        }
        return g_moduleBase;
    }

    bool IsFullyActive()
    {
        return g_maskRestored != 0;
    }

    // =========================================================================
    // Code Patching Utility
    // =========================================================================
    static bool PatchByte(uintptr_t addr, uint8_t expectedVal, uint8_t newVal, const char* desc)
    {
        __try {
            uint8_t* pByte = reinterpret_cast<uint8_t*>(addr);

            if (*pByte != expectedVal) {
                Log("  SKIP %s: found 0x%02X, expected 0x%02X", desc, *pByte, expectedVal);
                return false;
            }

            DWORD oldProtect;
            if (!VirtualProtect(pByte, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                Log("  FAIL %s: VirtualProtect error %u", desc, GetLastError());
                return false;
            }

            *pByte = newVal;
            VirtualProtect(pByte, 1, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), pByte, 1);

            Log("  OK   %s: 0x%02X -> 0x%02X", desc, expectedVal, newVal);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("  FAIL %s: exception during patch", desc);
            return false;
        }
    }

    // =========================================================================
    // Patch a MOV reg, [RIP+disp32] instruction to MOV reg, imm32
    // Detects instruction format, verifies displacement, replaces with immediate
    // =========================================================================
    static bool PatchMovRipToImm(uintptr_t instrRVA, uintptr_t globalRVA, uint32_t newValue, const char* desc)
    {
        uintptr_t base = GetModuleBase();
        uint8_t* ip = reinterpret_cast<uint8_t*>(base + instrRVA);

        __try {
            // Detect optional REX prefix (0x40-0x4F)
            bool hasRex = false;
            uint8_t rexByte = 0;
            int opcodeIdx = 0;

            if ((ip[0] & 0xF0) == 0x40) {
                hasRex = true;
                rexByte = ip[0];
                opcodeIdx = 1;
            }

            // Verify opcode is 0x8B (MOV r32, r/m32)
            if (ip[opcodeIdx] != 0x8B) {
                Log("  SKIP %s: opcode 0x%02X != 0x8B", desc, ip[opcodeIdx]);
                return false;
            }

            // Verify ModRM: mod=00, rm=101 (RIP-relative)
            uint8_t modrm = ip[opcodeIdx + 1];
            if ((modrm & 0xC7) != 0x05) {
                Log("  SKIP %s: ModRM 0x%02X not RIP-relative", desc, modrm);
                return false;
            }

            // Calculate expected displacement
            int instrLen = opcodeIdx + 2 + 4; // [REX] + opcode + ModRM + disp32
            uint32_t expectedDisp = static_cast<uint32_t>(globalRVA - (instrRVA + instrLen));
            uint32_t actualDisp = *reinterpret_cast<uint32_t*>(ip + opcodeIdx + 2);

            if (actualDisp != expectedDisp) {
                Log("  SKIP %s: disp 0x%08X != expected 0x%08X", desc, actualDisp, expectedDisp);
                return false;
            }

            // Extract destination register
            uint8_t reg = (modrm >> 3) & 7;
            bool extReg = hasRex && (rexByte & 0x04); // REX.R extends reg field

            // Build replacement: MOV reg, imm32
            uint8_t newInstr[8];
            int newLen;
            if (extReg) {
                newInstr[0] = 0x41;         // REX.B (for extended registers)
                newInstr[1] = 0xB8 + reg;   // MOV r32, imm32
                memcpy(newInstr + 2, &newValue, 4);
                newLen = 6;
            } else {
                newInstr[0] = 0xB8 + reg;   // MOV r32, imm32
                memcpy(newInstr + 1, &newValue, 4);
                newLen = 5;
            }

            // NOP-pad remaining bytes
            for (int i = newLen; i < instrLen; i++) {
                newInstr[i] = 0x90;
            }

            // Apply patch
            DWORD oldProtect;
            if (!VirtualProtect(ip, instrLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                Log("  FAIL %s: VirtualProtect error %u", desc, GetLastError());
                return false;
            }

            memcpy(ip, newInstr, instrLen);
            VirtualProtect(ip, instrLen, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), ip, instrLen);

            const char* regNames[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
            const char* extRegNames[] = {"r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
            const char* regName = extReg ? extRegNames[reg] : regNames[reg];

            Log("  OK   %s: MOV %s, [RIP+0x%X] -> MOV %s, %u (%d->%d bytes)",
                desc, regName, actualDisp, regName, newValue, instrLen, instrLen);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("  FAIL %s: exception", desc);
            return false;
        }
    }

    // =========================================================================
    // Step 1: SteamStub decryption check
    // =========================================================================
    static bool CheckTextDecrypted()
    {
        if (g_textDecrypted) return true;

        uintptr_t base = GetModuleBase();
        __try {
            uint8_t sentinel = *reinterpret_cast<uint8_t*>(base + TextSentinel);
            if (sentinel != TextSentinelExpected) {
                return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        InterlockedExchange(&g_textDecrypted, 1);
        Log("SteamStub decryption detected");
        return true;
    }

    // =========================================================================
    // Step 2: Force cascade count global to 4 (continuous, belt-and-suspenders)
    // =========================================================================
    static volatile long g_countForced = 0;

    static void ForceCascadeCount4()
    {
        if (g_countForced) return;
        uintptr_t base = GetModuleBase();
        __try {
            // .data section is already PAGE_READWRITE — no VirtualProtect needed
            volatile uint32_t* p = reinterpret_cast<volatile uint32_t*>(base + CascadeCountPatch::CountGlobal);
            if (*p != CascadeCountPatch::DesiredValue) {
                *p = CascadeCountPatch::DesiredValue;
            }
            InterlockedExchange(&g_countForced, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // =========================================================================
    // Step 3: Patch MOV instructions that read DAT_143924818 to load immediate 4
    // =========================================================================
    static void PatchCountReadSites()
    {
        if (g_countReadsPatched) return;
        if (!g_textDecrypted) return;

        Log("Patching cascade count read instructions (4 sites)");

        const char* names[] = {
            "ctor read (FUN_1427e8f50)",
            "setup read (FUN_14290dbd0)",
            "render read 1 (FUN_1428a4a60)",
            "render read 2 (FUN_1428a4a60)"
        };

        int n = 0;
        for (int i = 0; i < 4; i++) {
            if (PatchMovRipToImm(
                    CountReadPatch::AllSites[i],
                    CascadeCountPatch::CountGlobal,
                    CascadeCountPatch::DesiredValue,
                    names[i])) {
                n++;
            }
        }

        Log("Count read patches: %d/4 applied", n);

        // v11.0.0: Patch the CMP instruction at setup read site
        // The setup function FUN_14290dbd0 uses CMP [DAT_143924818], 2 to select
        // between 2-cascade (shorter) and 4-cascade (longer) shadow distances.
        // Change immediate from 2 to 0 so the comparison always fails → 4-cascade distance.
        uintptr_t base2 = GetModuleBase();
        if (PatchByte(base2 + CountReadPatch::SetupCmpImm,
                      CountReadPatch::SetupCmpOld, CountReadPatch::SetupCmpNew,
                      "setup CMP imm 2->4 (redirect to .data distance)")) {
            Log("Setup function will read from .data distance (avoids .rdata VirtualProtect)");
        }

        InterlockedExchange(&g_countReadsPatched, 1);
    }

    // =========================================================================
    // Step 4: Apply mask writer safe mode (ALWAYS, immediately after decryption)
    // Forces all mask writes to 0x3 - prevents crash while arrays aren't ready
    // =========================================================================
    static void ApplyMaskSafeMode()
    {
        if (g_maskSafe) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        using namespace MaskWriterPatch;

        Log("Applying mask writer safe mode (force mask=0x3)");

        int n = 0;
        if (PatchByte(base + InitMask_Byte, InitMask_Old, InitMask_New,
                      "initial mask 0xF->0x3")) n++;
        if (PatchByte(base + FallbackMask_Byte, FallbackMask_Old, FallbackMask_New,
                      "fallback mask 0xF->0x3")) n++;
        if (PatchByte(base + ArrayEntry1_Byte, ArrayEntry1_Old, ArrayEntry1_New,
                      "array[1] 0x5->0x3")) n++;
        if (PatchByte(base + ArrayEntry3_Byte, ArrayEntry3_Old, ArrayEntry3_New,
                      "array[3] 0x9->0x3")) n++;

        Log("Mask writer safe mode: %d/4 patches applied", n);
        InterlockedExchange(&g_maskSafe, 1);
    }

    // =========================================================================
    // Step 5: Patch shader constructor (2 -> 4 texture layers)
    // =========================================================================
    static void PatchShaderCtor()
    {
        if (g_shaderPatched) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        using namespace ShaderCtorPatch;

        Log("Patching shader constructor");

        int n = 0;
        if (PatchByte(base + ArrayCap_Byte, ArrayCap_Old, ArrayCap_New,
                      "shader array capacity 2->4")) n++;
        if (PatchByte(base + StoredCount_Byte, StoredCount_Old, StoredCount_New,
                      "shader stored count 2->4")) n++;

        Log("Shader constructor: %d/2 patches applied", n);
        InterlockedExchange(&g_shaderPatched, 1);
    }

    // =========================================================================
    // Step 5b: Fix stereo shadow dispatch (JZ -> JMP at FUN_14281bd40+0xDC)
    // RIGHT eye (flag=1) was skipping geometry marked by LEFT eye's deferred path.
    // LEFT sets bit 53 on geometry it defers; RIGHT checked bit 53 and skipped.
    // Fix: make RIGHT always dispatch by changing conditional JZ to unconditional JMP.
    // =========================================================================
    static volatile long g_stereoFixPatched = 0;

    static void PatchStereoDispatch()
    {
        if (g_stereoFixPatched) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        using namespace StereoDispatchFix;

        Log("Patching stereo dispatch (RIGHT eye bit-53 skip)");
        if (PatchByte(base + JzInstrRVA, JzOpcode, JmpOpcode,
                      "stereo fix JZ->JMP at FUN_14281bd40+0xDC")) {
            InterlockedExchange(&g_stereoFixPatched, 1);
        }
    }

    // =========================================================================
    // Step 5c: Patch 4-cascade shadow distance (.rdata, one-shot)
    // 0x2c7f648 is in .rdata (read-only) — VirtualProtect required.
    // Must happen during initial patching, NOT from timer thread,
    // to avoid BackgroundProcessThread interference during NIF loading.
    // =========================================================================
    static volatile long g_shadowDistPatched = 0;

    static void PatchShadowDistance()
    {
        if (g_shadowDistPatched) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        __try {
            float* pDist4 = reinterpret_cast<float*>(base + ShadowDist4Cascade);
            float dist2 = *reinterpret_cast<float*>(base + ShadowDist2Cascade);

            if (*pDist4 > 1e30f) {
                float newDist = (dist2 > 0.0f) ? dist2 * 5.0f : 15000.0f;
                DWORD oldProtect;
                if (VirtualProtect(pDist4, 4, PAGE_READWRITE, &oldProtect)) {
                    *pDist4 = newDist;
                    VirtualProtect(pDist4, 4, oldProtect, &oldProtect);
                    InterlockedExchange(&g_shadowDistPatched, 1);
                    Log("Patched shadow distance (.rdata): FLT_MAX -> %.1f", newDist);
                }
            } else {
                InterlockedExchange(&g_shadowDistPatched, 1);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // =========================================================================
    // Hex dump helper for VR entry diagnostics
    // =========================================================================
    static void HexDumpEntry(const char* label, uintptr_t addr, size_t size)
    {
        Log("=== %s (0x%llX, %zu bytes) ===", label, addr, size);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
        char line[128];
        for (size_t off = 0; off < size; off += 16) {
            int n = snprintf(line, sizeof(line), "  +%03X:", (uint32_t)off);
            for (size_t j = 0; j < 16 && (off + j) < size; j++) {
                n += snprintf(line + n, sizeof(line) - n, " %02X", p[off + j]);
            }
            Log("%s", line);
        }
    }

    // =========================================================================
    // Step 6: Expand VR cascade array (2 -> 4 entries)
    // Uses template copy from entry 0 to properly initialize entries 2-3
    // =========================================================================
    static void TryExpandVRArray()
    {
        if (g_vrExpanded) return;

        uintptr_t base = GetModuleBase();
        using namespace VRArrayExpansion;

        __try {
            uintptr_t* pArrayPtr = reinterpret_cast<uintptr_t*>(base + ArrayPtr);
            uint32_t* pCapacity = reinterpret_cast<uint32_t*>(base + ArrayPtr + 8);
            uint32_t* pCount = reinterpret_cast<uint32_t*>(base + ArrayCount);
            uintptr_t buf = *pArrayPtr;
            uint32_t capacity = *pCapacity;
            uint32_t count = *pCount;

            if (buf == 0) return;  // VR array not allocated yet

            // Log VR array state once (no hex dump — minimize heap reads during loading)
            static volatile long s_dumpOnce = 0;
            if (InterlockedCompareExchange(&s_dumpOnce, 1, 0) == 0) {
                Log("VR array: ptr=0x%llX, capacity=%u, count=%u", buf, capacity, count);
            }

            if (capacity >= TargetCount) {
                // Template copy: use entry 0 as template for entries 2 and 3
                // Entry 0 is fully initialized by the game; entries 2-3 may have
                // incomplete pool metadata causing render node corruption.
                uintptr_t templateEntry = buf + 0 * EntrySize;
                int entriesFixed = 0;

                for (uint32_t i = 2; i < TargetCount; i++) {
                    uintptr_t dst = buf + i * EntrySize;

                    // Copy full entry from template (entry 0)
                    memcpy(reinterpret_cast<void*>(dst),
                           reinterpret_cast<void*>(templateEntry), EntrySize);

                    // Reset per-entry pool self-ref pointers (must point to OWN entry)
                    for (size_t poolOff : PoolOffsets) {
                        // Clear pool head (no allocated nodes)
                        *reinterpret_cast<uintptr_t*>(dst + poolOff) = 0;
                        // Set tail -> head (empty list marker)
                        *reinterpret_cast<uintptr_t*>(dst + poolOff + 8) = dst + poolOff;
                    }

                    // Clear spinlock (thread ID + lock count)
                    *reinterpret_cast<uint32_t*>(dst + 0x00) = 0;
                    *reinterpret_cast<uint32_t*>(dst + 0x04) = 0;

                    entriesFixed++;
                }

                Log("VR array: template-copied entry 0 -> entries 2-%u (%d entries fixed)",
                    TargetCount - 1, entriesFixed);

                // Also ensure entries 0-1 have valid pool pointers
                for (uint32_t i = 0; i < 2; i++) {
                    uintptr_t entryBase = buf + i * EntrySize;
                    for (size_t poolOff : PoolOffsets) {
                        uintptr_t* pTail = reinterpret_cast<uintptr_t*>(entryBase + poolOff + 8);
                        if (*pTail == 0) {
                            *pTail = entryBase + poolOff;
                        }
                    }
                }

                if (count < TargetCount) {
                    *pCount = TargetCount;
                    Log("VR array: set count %u -> %u (capacity already %u)",
                        count, TargetCount, capacity);
                } else {
                    Log("VR array already has %u entries", count);
                }
                InterlockedExchange(&g_vrExpanded, 1);
                return;
            }

            // Capacity < 4: need to actually expand (shouldn't happen with count=4 patches)
            size_t newSize = TargetCount * EntrySize;
            void* newBuf = VirtualAlloc(nullptr, newSize,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!newBuf) {
                Log("VR array VirtualAlloc failed, error %u", GetLastError());
                return;
            }

            // Copy ALL existing entries (use capacity, not count)
            size_t copySize = capacity * EntrySize;
            memcpy(newBuf, reinterpret_cast<void*>(buf), copySize);

            // Template-copy entry 0 to remaining entries
            uintptr_t templateSrc = reinterpret_cast<uintptr_t>(newBuf);
            for (uint32_t i = capacity; i < TargetCount; i++) {
                uintptr_t dst = reinterpret_cast<uintptr_t>(newBuf) + i * EntrySize;
                memcpy(reinterpret_cast<void*>(dst),
                       reinterpret_cast<void*>(templateSrc), EntrySize);

                // Reset per-entry pool self-ref pointers
                for (size_t poolOff : PoolOffsets) {
                    *reinterpret_cast<uintptr_t*>(dst + poolOff) = 0;
                    *reinterpret_cast<uintptr_t*>(dst + poolOff + 8) = dst + poolOff;
                }
                // Clear spinlock
                *reinterpret_cast<uint32_t*>(dst + 0x00) = 0;
                *reinterpret_cast<uint32_t*>(dst + 0x04) = 0;
            }

            *pArrayPtr = reinterpret_cast<uintptr_t>(newBuf);
            *pCount = TargetCount;

            Log("VR cascade array expanded: cap %u -> 4 entries (template copy)", capacity);
            Log("  Old buffer: 0x%llX, New buffer: 0x%llX", buf, (uintptr_t)newBuf);
            InterlockedExchange(&g_vrExpanded, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("VR array expansion: exception caught");
        }
    }

    // =========================================================================
    // Step 6b: Refresh VR array entries 2-3 from populated entries 0-1
    // The game only populates VR entries for 2 cascades (the original VR limit).
    // Entries 2-3 (far cascades) remain as our initial template copy (all zeros).
    // This causes RIGHT eye far shadows to have zero projection matrices,
    // making them appear "attached to the headset" instead of world-fixed.
    // Fix: after the game has populated entries 0-1 with valid per-eye data,
    // re-copy entry 0→2 and entry 1→3 to give far cascades valid RIGHT eye data.
    // The matrices will be for near cascade distances, but at least shadows will
    // be correctly positioned per-eye. The game updates them each frame.
    // =========================================================================
    static volatile long g_vrEntriesRefreshed = 0;

    static void RefreshVRArrayEntries()
    {
        if (g_vrEntriesRefreshed) return;
        if (!g_vrExpanded) return;

        uintptr_t base = GetModuleBase();
        using namespace VRArrayExpansion;

        __try {
            uintptr_t buf = *reinterpret_cast<uintptr_t*>(base + ArrayPtr);
            if (buf == 0) return;

            // Check if entry 0 has more non-zero bytes than our template copy had.
            // Our template copy set: +0x00 (spinlock=1, 4 bytes), +0x18 (ptr, 8 bytes),
            // and pool tail pointers. Count non-zero bytes in entry 0 vs entry 2.
            // If entry 0 has MORE non-zero bytes, the game has written additional data.
            const uint8_t* e0 = reinterpret_cast<const uint8_t*>(buf);
            const uint8_t* e2 = reinterpret_cast<const uint8_t*>(buf + 2 * EntrySize);
            int nz0 = 0, nz2 = 0;
            for (size_t off = 0; off < EntrySize; off++) {
                if (e0[off] != 0) nz0++;
                if (e2[off] != 0) nz2++;
            }

            // If entry 0 doesn't have significantly more data than entry 2 (our copy),
            // the game hasn't populated it yet
            if (nz0 <= nz2 + 2) return;

            Log("=== VR array entry refresh (game has populated entries 0-1) ===");

            // Log first 64 bytes of entries 0-3 for comparison
            for (uint32_t i = 0; i < 4; i++) {
                uintptr_t entry = buf + i * EntrySize;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(entry);
                // Count non-zero bytes to show how populated each entry is
                int nonZero = 0;
                for (size_t off = 0; off < EntrySize; off++) {
                    if (p[off] != 0) nonZero++;
                }
                Log("  VR entry[%u]: %d/%zu non-zero bytes", i, nonZero, EntrySize);
            }

            // Re-copy populated entry data: entry 0→2, entry 1→3
            // This gives far cascades the same per-eye rendering parameters as near cascades.
            // The game's per-frame update will then adjust cascade-specific parameters.
            for (uint32_t i = 2; i < TargetCount; i++) {
                uint32_t src = i - 2;  // 0→2, 1→3
                uintptr_t srcEntry = buf + src * EntrySize;
                uintptr_t dstEntry = buf + i * EntrySize;

                memcpy(reinterpret_cast<void*>(dstEntry),
                       reinterpret_cast<void*>(srcEntry), EntrySize);

                // Reset per-entry pool self-ref pointers (must point to OWN entry, not source)
                for (size_t poolOff : PoolOffsets) {
                    *reinterpret_cast<uintptr_t*>(dstEntry + poolOff) = 0;
                    *reinterpret_cast<uintptr_t*>(dstEntry + poolOff + 8) = dstEntry + poolOff;
                }

                // Clear spinlock
                *reinterpret_cast<uint32_t*>(dstEntry + 0x00) = 0;
                *reinterpret_cast<uint32_t*>(dstEntry + 0x04) = 0;
            }

            Log("VR array: refreshed entries 2-3 from populated entries 0-1");

            // Log result
            for (uint32_t i = 0; i < 4; i++) {
                uintptr_t entry = buf + i * EntrySize;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(entry);
                int nonZero = 0;
                for (size_t off = 0; off < EntrySize; off++) {
                    if (p[off] != 0) nonZero++;
                }
                Log("  VR entry[%u] after refresh: %d/%zu non-zero bytes", i, nonZero, EntrySize);
            }

            InterlockedExchange(&g_vrEntriesRefreshed, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("RefreshVRArrayEntries: exception caught");
        }
    }

    // =========================================================================
    // Step 7: Null safety patch for FUN_142813740 crash
    // The function reads lVar2 = *(param_2 + 0x180) then dereferences lVar2+0x38.
    // Two bugs: (1) param_2 can be NULL, (2) *(param_2+0x180) can be garbage.
    // We redirect the 7-byte mov instruction through a code cave trampoline that
    // checks both: r10==0 (null param_2) and rbp has sign bit set (invalid ptr).
    // =========================================================================
    static volatile long g_nullSafePatched = 0;
    static void* g_codeCave = nullptr;

    static void* AllocateNearby(uintptr_t target, size_t size)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t granularity = si.dwAllocationGranularity;

        // Scan outward from target in both directions, stay within ±2GB
        for (uintptr_t offset = granularity; offset < 0x7F000000; offset += granularity) {
            // Try above
            void* p = VirtualAlloc(reinterpret_cast<void*>(target + offset),
                                   size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;

            // Try below
            if (target > offset) {
                p = VirtualAlloc(reinterpret_cast<void*>(target - offset),
                                 size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (p) return p;
            }
        }
        return nullptr;
    }

    static void PatchNullSafetyCheck()
    {
        if (g_nullSafePatched) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        using namespace NullSafetyPatch;

        const uint8_t expectedBytes[] = { 0x49, 0x8B, 0xAA, 0x80, 0x01, 0x00, 0x00 };
        uint8_t* crashAddr = reinterpret_cast<uint8_t*>(base + CrashInstrRVA);
        uintptr_t returnAddr = reinterpret_cast<uintptr_t>(crashAddr) + InstrSize;

        __try {
            // Verify instruction bytes
            if (memcmp(crashAddr, expectedBytes, InstrSize) != 0) {
                Log("SKIP null safety: bytes mismatch at RVA 0x%X", (uint32_t)CrashInstrRVA);
                Log("  Expected: 49 8B AA 80 01 00 00");
                Log("  Found:    %02X %02X %02X %02X %02X %02X %02X",
                    crashAddr[0], crashAddr[1], crashAddr[2], crashAddr[3],
                    crashAddr[4], crashAddr[5], crashAddr[6]);
                return;
            }

            // Allocate code cave within ±2GB of crash site (required for jmp rel32)
            g_codeCave = AllocateNearby(reinterpret_cast<uintptr_t>(crashAddr), 64);
            if (!g_codeCave) {
                Log("FAIL null safety: could not allocate code cave near 0x%llX",
                    (uintptr_t)crashAddr);
                return;
            }

            uint8_t* cave = reinterpret_cast<uint8_t*>(g_codeCave);
            int pos = 0;

            // Code cave with null check AND pointer validation:
            //   [0]  test r10, r10          ; 3 bytes - null check param_2
            //   [3]  jz null_case           ; 2 bytes
            //   [5]  mov rbp, [r10+0x180]   ; 7 bytes - load sub-object pointer
            //   [12] test rbp, rbp          ; 3 bytes - null/sign check
            //   [15] jz done               ; 2 bytes - null is OK, original handles
            //   [17] js null_case           ; 2 bytes - bit 63 set = invalid ptr
            //   [19] done: jmp return_addr  ; 5 bytes
            //   [24] null_case: xor ebp,ebp ; 2 bytes - force null
            //   [26] jmp return_addr        ; 5 bytes

            // [0] test r10, r10
            cave[pos++] = 0x4D; cave[pos++] = 0x85; cave[pos++] = 0xD2;

            // [3] jz null_case (target at offset 24, rel8 = 24-5 = 19 = 0x13)
            cave[pos++] = 0x74; cave[pos++] = 0x13;

            // [5] mov rbp, [r10+0x180] (original instruction)
            memcpy(cave + pos, expectedBytes, InstrSize);
            pos += (int)InstrSize;

            // [12] test rbp, rbp
            cave[pos++] = 0x48; cave[pos++] = 0x85; cave[pos++] = 0xED;

            // [15] jz done (target at offset 19, rel8 = 19-17 = 2)
            cave[pos++] = 0x74; cave[pos++] = 0x02;

            // [17] js null_case (target at offset 24, rel8 = 24-19 = 5)
            cave[pos++] = 0x78; cave[pos++] = 0x05;

            // [19] done: jmp return_addr
            cave[pos++] = 0xE9;
            int32_t rel1 = static_cast<int32_t>(
                (intptr_t)returnAddr - (intptr_t)(reinterpret_cast<uintptr_t>(cave + pos) + 4));
            memcpy(cave + pos, &rel1, 4);
            pos += 4;

            // [24] null_case: xor ebp, ebp
            cave[pos++] = 0x31; cave[pos++] = 0xED;

            // [26] jmp return_addr
            cave[pos++] = 0xE9;
            int32_t rel2 = static_cast<int32_t>(
                (intptr_t)returnAddr - (intptr_t)(reinterpret_cast<uintptr_t>(cave + pos) + 4));
            memcpy(cave + pos, &rel2, 4);
            pos += 4;

            // Patch original: jmp code_cave (5 bytes) + 2 NOPs
            uint8_t patch[7];
            patch[0] = 0xE9;
            int32_t jmpRel = static_cast<int32_t>(
                (intptr_t)reinterpret_cast<uintptr_t>(cave) -
                (intptr_t)(reinterpret_cast<uintptr_t>(crashAddr) + 5));
            memcpy(patch + 1, &jmpRel, 4);
            patch[5] = 0x90;
            patch[6] = 0x90;

            DWORD oldProtect;
            if (!VirtualProtect(crashAddr, InstrSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                Log("FAIL null safety: VirtualProtect error %u", GetLastError());
                VirtualFree(g_codeCave, 0, MEM_RELEASE);
                g_codeCave = nullptr;
                return;
            }

            memcpy(crashAddr, patch, InstrSize);
            VirtualProtect(crashAddr, InstrSize, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), crashAddr, InstrSize);

            Log("Null safety patch applied at RVA 0x%X -> code cave 0x%llX",
                (uint32_t)CrashInstrRVA, (uintptr_t)g_codeCave);
            InterlockedExchange(&g_nullSafePatched, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("FAIL null safety: exception during patch");
        }
    }

    // =========================================================================
    // Step 8: Patch node allocator to clear ->next pointer on reuse
    // FUN_14278e610 reuses render pass nodes without clearing +0x40 (->next).
    // We redirect function entry through a code cave that clears +0x40 when
    // param_2 (RDX) is non-null, then falls through to the original code.
    // =========================================================================
    static volatile long g_nodeAllocPatched = 0;
    static void* g_nodeAllocCave = nullptr;

    static void PatchNodeAllocator()
    {
        if (g_nodeAllocPatched) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        uint8_t* funcAddr = reinterpret_cast<uint8_t*>(base + NodeAllocPatch::FuncRVA);

        __try {
            // Log the first 16 bytes for diagnostics
            Log("NodeAlloc bytes: %02X %02X %02X %02X %02X %02X %02X %02X"
                " %02X %02X %02X %02X %02X %02X %02X %02X",
                funcAddr[0], funcAddr[1], funcAddr[2], funcAddr[3],
                funcAddr[4], funcAddr[5], funcAddr[6], funcAddr[7],
                funcAddr[8], funcAddr[9], funcAddr[10], funcAddr[11],
                funcAddr[12], funcAddr[13], funcAddr[14], funcAddr[15]);

            // Actual prologue: sub rsp, 0x68 (48 83 EC 68) + mov r10, r9 (4D 8B D1)
            // = 7 bytes total, two complete instructions we can safely relocate
            const uint8_t expectedPrologue[] = { 0x48, 0x83, 0xEC, 0x68, 0x4D, 0x8B, 0xD1 };
            constexpr size_t prologueSize = 7;

            if (memcmp(funcAddr, expectedPrologue, prologueSize) != 0) {
                Log("SKIP node alloc patch: prologue mismatch");
                return;
            }

            uintptr_t returnAddr = reinterpret_cast<uintptr_t>(funcAddr + prologueSize);

            // Allocate code cave near function
            g_nodeAllocCave = AllocateNearby(reinterpret_cast<uintptr_t>(funcAddr), 64);
            if (!g_nodeAllocCave) {
                Log("FAIL node alloc patch: could not allocate code cave");
                return;
            }

            uint8_t* cave = reinterpret_cast<uint8_t*>(g_nodeAllocCave);
            int pos = 0;

            // Code cave: clear +0x40 if rdx non-null, then execute relocated prologue
            //   [0]  test rdx, rdx           ; 3 bytes - null check param_2
            //   [3]  jz skip_clear           ; 2 bytes
            //   [5]  mov qword [rdx+0x40], 0 ; 8 bytes - CLEAR ->next pointer
            //   [13] skip_clear:
            //   [13] sub rsp, 0x68           ; 4 bytes (original)
            //   [17] mov r10, r9             ; 3 bytes (original)
            //   [20] jmp returnAddr          ; 5 bytes

            // [0] test rdx, rdx
            cave[pos++] = 0x48; cave[pos++] = 0x85; cave[pos++] = 0xD2;

            // [3] jz skip_clear (target at offset 13, rel8 = 13-5 = 8)
            cave[pos++] = 0x74; cave[pos++] = 0x08;

            // [5] mov qword ptr [rdx+0x40], 0
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x42; cave[pos++] = 0x40;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [13] sub rsp, 0x68 (relocated original)
            cave[pos++] = 0x48; cave[pos++] = 0x83; cave[pos++] = 0xEC; cave[pos++] = 0x68;

            // [17] mov r10, r9 (relocated original)
            cave[pos++] = 0x4D; cave[pos++] = 0x8B; cave[pos++] = 0xD1;

            // [20] jmp returnAddr (funcAddr + 7)
            cave[pos++] = 0xE9;
            int32_t rel1 = static_cast<int32_t>(
                (intptr_t)returnAddr - (intptr_t)(reinterpret_cast<uintptr_t>(cave + pos) + 4));
            memcpy(cave + pos, &rel1, 4);
            pos += 4;

            // Patch original function: replace first 7 bytes with jmp cave + 2 NOPs
            uint8_t patch[7];
            patch[0] = 0xE9;
            int32_t jmpRel = static_cast<int32_t>(
                (intptr_t)reinterpret_cast<uintptr_t>(cave) -
                (intptr_t)(reinterpret_cast<uintptr_t>(funcAddr) + 5));
            memcpy(patch + 1, &jmpRel, 4);
            patch[5] = 0x90;
            patch[6] = 0x90;

            DWORD oldProtect;
            if (!VirtualProtect(funcAddr, prologueSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                Log("FAIL node alloc patch: VirtualProtect error %u", GetLastError());
                VirtualFree(g_nodeAllocCave, 0, MEM_RELEASE);
                g_nodeAllocCave = nullptr;
                return;
            }

            memcpy(funcAddr, patch, prologueSize);
            VirtualProtect(funcAddr, prologueSize, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), funcAddr, prologueSize);

            Log("Node alloc patch applied: +0x40 clear on reuse (cave 0x%llX)",
                (uintptr_t)g_nodeAllocCave);
            InterlockedExchange(&g_nodeAllocPatched, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("FAIL node alloc patch: exception");
        }
    }

    // =========================================================================
    // Step 9: Zero-init BOTH tag entry and data entry in lookup function
    // FUN_1427a51e0's "not found" path writes a tag but leaves BOTH the tag
    // entry fields (+0x08 through +0x1F = linked list heads, flags) AND the
    // returned data entry (entry[slot+5], 0x20 bytes of per-cascade pointers)
    // uninitialized. Garbage in the tag entry's +0x08 field (linked list head)
    // crashes FUN_14278e4f0 when it tries to traverse the linked list.
    // This is the ROOT CAUSE fix — all other patches are defense-in-depth.
    // =========================================================================
    static volatile long g_entryZeroInitPatched = 0;
    static void* g_entryZeroInitCave = nullptr;

    static void PatchCascadeEntryZeroInit()
    {
        if (g_entryZeroInitPatched) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        using namespace CascadeEntryZeroInit;

        uint8_t* patchAddr = reinterpret_cast<uint8_t*>(base + TagWriteRVA);
        uintptr_t returnAddr = base + ReturnRVA;

        // Expected bytes: 4A 89 94 10 90 00 00 00 (mov [rax+r10+0x90], rdx)
        const uint8_t expectedBytes[] = { 0x4A, 0x89, 0x94, 0x10, 0x90, 0x00, 0x00, 0x00 };

        __try {
            if (memcmp(patchAddr, expectedBytes, InstrSize) != 0) {
                Log("SKIP entry zero-init: bytes mismatch at RVA 0x%X", (uint32_t)TagWriteRVA);
                Log("  Expected: 4A 89 94 10 90 00 00 00");
                Log("  Found:    %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3],
                    patchAddr[4], patchAddr[5], patchAddr[6], patchAddr[7]);
                return;
            }

            g_entryZeroInitCave = AllocateNearby(reinterpret_cast<uintptr_t>(patchAddr), 128);
            if (!g_entryZeroInitCave) {
                Log("FAIL entry zero-init: could not allocate code cave");
                return;
            }

            uint8_t* cave = reinterpret_cast<uint8_t*>(g_entryZeroInitCave);
            int pos = 0;

            // Code cave layout:
            // At entry: rax = slot*32, r10 = BSLightingShaderProperty, rdx = shadow_tag
            // Tag entry = r10 + rax + 0x90  (tag at +0x00, linked list head at +0x08)
            // Data entry = r10 + rax + 0x130 (4 cascade pointers)
            //
            // We zero tag entry fields +0x08..+0x1F (skip +0x00, overwritten by tag write)
            // and data entry fields +0x00..+0x1F (all 4 cascade pointers).
            //
            //   [0]  push rcx                          ; 1
            //   [1]  lea rcx, [rax+r10+0x90]           ; 8  - tag entry base
            //   [9]  mov qword [rcx+0x08], 0           ; 8  - zero linked list head
            //   [17] mov qword [rcx+0x10], 0           ; 8  - zero tag field 2
            //   [25] mov qword [rcx+0x18], 0           ; 8  - zero tag field 3
            //   [33] lea rcx, [rax+r10+0x130]          ; 8  - data entry base
            //   [41] mov qword [rcx], 0                ; 7  - zero cascade ptr 0
            //   [48] mov qword [rcx+0x08], 0           ; 8  - zero cascade ptr 1
            //   [56] mov qword [rcx+0x10], 0           ; 8  - zero cascade ptr 2
            //   [64] mov qword [rcx+0x18], 0           ; 8  - zero cascade ptr 3
            //   [72] pop rcx                           ; 1
            //   [73] mov [rax+r10+0x90], rdx           ; 8  - original tag write
            //   [81] jmp returnAddr                    ; 5
            //   Total: 86 bytes

            // [0] push rcx
            cave[pos++] = 0x51;

            // [1] lea rcx, [rax + r10 + 0x90]
            // Encoding: 4A 8D 8C 10 90 00 00 00
            cave[pos++] = 0x4A; cave[pos++] = 0x8D; cave[pos++] = 0x8C; cave[pos++] = 0x10;
            cave[pos++] = 0x90; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [9] mov qword [rcx+0x08], 0 — zero linked list head (crash field!)
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x41; cave[pos++] = 0x08;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [17] mov qword [rcx+0x10], 0
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x41; cave[pos++] = 0x10;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [25] mov qword [rcx+0x18], 0
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x41; cave[pos++] = 0x18;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [33] lea rcx, [rax + r10 + 0x130]
            // Encoding: 4A 8D 8C 10 30 01 00 00
            cave[pos++] = 0x4A; cave[pos++] = 0x8D; cave[pos++] = 0x8C; cave[pos++] = 0x10;
            cave[pos++] = 0x30; cave[pos++] = 0x01; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [41] mov qword [rcx], 0
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x01;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [48] mov qword [rcx+0x08], 0
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x41; cave[pos++] = 0x08;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [56] mov qword [rcx+0x10], 0
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x41; cave[pos++] = 0x10;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [64] mov qword [rcx+0x18], 0
            cave[pos++] = 0x48; cave[pos++] = 0xC7; cave[pos++] = 0x41; cave[pos++] = 0x18;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [72] pop rcx
            cave[pos++] = 0x59;

            // [73] mov [rax+r10+0x90], rdx (original instruction, relocated)
            memcpy(cave + pos, expectedBytes, InstrSize);
            pos += (int)InstrSize;

            // [81] jmp returnAddr
            cave[pos++] = 0xE9;
            int32_t relReturn = static_cast<int32_t>(
                (intptr_t)returnAddr - (intptr_t)(reinterpret_cast<uintptr_t>(cave + pos) + 4));
            memcpy(cave + pos, &relReturn, 4);
            pos += 4;

            // Patch original: jmp code_cave (5 bytes) + 3 NOPs
            uint8_t patch[8];
            patch[0] = 0xE9;
            int32_t jmpRel = static_cast<int32_t>(
                (intptr_t)reinterpret_cast<uintptr_t>(cave) -
                (intptr_t)(reinterpret_cast<uintptr_t>(patchAddr) + 5));
            memcpy(patch + 1, &jmpRel, 4);
            patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90;

            DWORD oldProtect;
            if (!VirtualProtect(patchAddr, InstrSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                Log("FAIL entry zero-init: VirtualProtect error %u", GetLastError());
                VirtualFree(g_entryZeroInitCave, 0, MEM_RELEASE);
                g_entryZeroInitCave = nullptr;
                return;
            }

            memcpy(patchAddr, patch, InstrSize);
            VirtualProtect(patchAddr, InstrSize, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchAddr, InstrSize);

            Log("Entry zero-init patch at RVA 0x%X -> cave 0x%llX (%d bytes, tag+data)",
                (uint32_t)TagWriteRVA, (uintptr_t)g_entryZeroInitCave, pos);
            InterlockedExchange(&g_entryZeroInitPatched, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("FAIL entry zero-init: exception during patch");
        }
    }

    // =========================================================================
    // Step 10: Cascade pointer validation for BSLightingShaderProperty render
    // FUN_1427a3f90+0xA53: loads per-cascade ptr from [rax+rdi*8], rdi=cascade idx.
    // Array entry for cascade 3 may be uninitialized garbage (from BSLightingShaderProperty
    // objects that were allocated before our zero-init patch was active).
    // Validation: upper bits check + lower-32-bits-zero check (real ptrs never 4GB-aligned).
    // Self-healing: when garbage detected, zero the slot via R12 and set R14=0 so the
    // NULL fallback path creates a new valid node. Next frame uses the valid pointer.
    // =========================================================================
    static volatile long g_ptrValidationPatched = 0;
    static void* g_ptrValidationCave = nullptr;

    static void PatchCascadePtrValidation()
    {
        if (g_ptrValidationPatched) return;
        if (!g_textDecrypted) return;

        uintptr_t base = GetModuleBase();
        using namespace CascadePtrValidation;

        uint8_t* patchAddr = reinterpret_cast<uint8_t*>(base + TestInstrRVA);
        uintptr_t skipTarget = base + SkipTargetRVA;
        uintptr_t continueAddr = base + ContinueRVA;

        // Expected bytes: test r14,r14 (4D 85 F6) + jz near (0F 84 8A 00 00 00)
        const uint8_t expectedBytes[] = { 0x4D, 0x85, 0xF6, 0x0F, 0x84, 0x8A, 0x00, 0x00, 0x00 };

        __try {
            if (memcmp(patchAddr, expectedBytes, PatchSize) != 0) {
                Log("SKIP cascade ptr validation: bytes mismatch at RVA 0x%X", (uint32_t)TestInstrRVA);
                Log("  Expected: 4D 85 F6 0F 84 8A 00 00 00");
                Log("  Found:    %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3],
                    patchAddr[4], patchAddr[5], patchAddr[6], patchAddr[7], patchAddr[8]);
                return;
            }

            g_ptrValidationCave = AllocateNearby(reinterpret_cast<uintptr_t>(patchAddr), 128);
            if (!g_ptrValidationCave) {
                Log("FAIL cascade ptr validation: could not allocate code cave");
                return;
            }

            uint8_t* cave = reinterpret_cast<uint8_t*>(g_ptrValidationCave);
            int pos = 0;

            // Code cave layout (self-healing pointer validation):
            // Context: R14 = cascade ptr, R12 = &[rax+rdi*8] (slot addr)
            //
            //   [0]  test r14, r14           ; 3  - null check
            //   [3]  jz skip                 ; 2  - null → skip (create node path)
            //   [5]  push rax                ; 1  - save temp
            //   [6]  mov rax, r14            ; 3  - copy pointer
            //   [9]  shr rax, 47             ; 4  - check bits 47-63
            //   [13] test eax, eax           ; 2
            //   [15] jnz pop_fix             ; 2  - high bits set → garbage
            //   [17] mov eax, r14d           ; 3  - get lower 32 bits
            //   [20] test eax, eax           ; 2  - real ptrs never have lower 32 bits = 0
            //   [22] jz pop_fix              ; 2  - lower 32 bits zero → garbage
            //   [24] pop rax                 ; 1  - valid pointer
            //   [25] jmp continue_addr       ; 5  - back to mov edi, [r14+0x48]
            //   [30] pop_fix:
            //   [30] pop rax                 ; 1  - restore rax
            //   [31] mov qword [r12], 0      ; 8  - zero cascade slot (self-heal)
            //   [39] xor r14d, r14d          ; 3  - r14 = 0 → NULL path creates node
            //   [42] skip:
            //   [42] jmp skip_target         ; 5  - to original jz target (0x27A4A6D)
            //   Total: 47 bytes

            // [0] test r14, r14
            cave[pos++] = 0x4D; cave[pos++] = 0x85; cave[pos++] = 0xF6;

            // [3] jz skip (target at offset 42, rel8 = 42-5 = 37 = 0x25)
            cave[pos++] = 0x74; cave[pos++] = 0x25;

            // [5] push rax
            cave[pos++] = 0x50;

            // [6] mov rax, r14  (4C 89 F0)
            cave[pos++] = 0x4C; cave[pos++] = 0x89; cave[pos++] = 0xF0;

            // [9] shr rax, 47  (48 C1 E8 2F)
            cave[pos++] = 0x48; cave[pos++] = 0xC1; cave[pos++] = 0xE8; cave[pos++] = 0x2F;

            // [13] test eax, eax
            cave[pos++] = 0x85; cave[pos++] = 0xC0;

            // [15] jnz pop_fix (target at offset 30, rel8 = 30-17 = 13 = 0x0D)
            cave[pos++] = 0x75; cave[pos++] = 0x0D;

            // [17] mov eax, r14d  (44 89 F0) — lower 32 bits of r14
            cave[pos++] = 0x44; cave[pos++] = 0x89; cave[pos++] = 0xF0;

            // [20] test eax, eax
            cave[pos++] = 0x85; cave[pos++] = 0xC0;

            // [22] jz pop_fix (target at offset 30, rel8 = 30-24 = 6)
            cave[pos++] = 0x74; cave[pos++] = 0x06;

            // [24] pop rax — valid pointer path
            cave[pos++] = 0x58;

            // [25] jmp continue_addr (back to mov edi, [r14+0x48])
            cave[pos++] = 0xE9;
            int32_t relContinue = static_cast<int32_t>(
                (intptr_t)continueAddr - (intptr_t)(reinterpret_cast<uintptr_t>(cave + pos) + 4));
            memcpy(cave + pos, &relContinue, 4);
            pos += 4;

            // [30] pop_fix: pop rax — garbage detected, self-heal
            cave[pos++] = 0x58;

            // [31] mov qword ptr [r12], 0 — zero the cascade slot
            // Encoding: 49 C7 04 24 00 00 00 00
            cave[pos++] = 0x49; cave[pos++] = 0xC7; cave[pos++] = 0x04; cave[pos++] = 0x24;
            cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00; cave[pos++] = 0x00;

            // [39] xor r14d, r14d — set r14 = 0 so NULL path creates new node
            // Encoding: 45 31 F6
            cave[pos++] = 0x45; cave[pos++] = 0x31; cave[pos++] = 0xF6;

            // [42] skip: jmp skip_target (to original jz target, creates new node)
            cave[pos++] = 0xE9;
            int32_t relSkip = static_cast<int32_t>(
                (intptr_t)skipTarget - (intptr_t)(reinterpret_cast<uintptr_t>(cave + pos) + 4));
            memcpy(cave + pos, &relSkip, 4);
            pos += 4;

            // Patch original: jmp code_cave (5 bytes) + 4 NOPs
            uint8_t patch[9];
            patch[0] = 0xE9;
            int32_t jmpRel = static_cast<int32_t>(
                (intptr_t)reinterpret_cast<uintptr_t>(cave) -
                (intptr_t)(reinterpret_cast<uintptr_t>(patchAddr) + 5));
            memcpy(patch + 1, &jmpRel, 4);
            patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90; patch[8] = 0x90;

            DWORD oldProtect;
            if (!VirtualProtect(patchAddr, PatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                Log("FAIL cascade ptr validation: VirtualProtect error %u", GetLastError());
                VirtualFree(g_ptrValidationCave, 0, MEM_RELEASE);
                g_ptrValidationCave = nullptr;
                return;
            }

            memcpy(patchAddr, patch, PatchSize);
            VirtualProtect(patchAddr, PatchSize, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), patchAddr, PatchSize);

            Log("Cascade ptr validation patch at RVA 0x%X -> cave 0x%llX (%d bytes, self-healing)",
                (uint32_t)TestInstrRVA, (uintptr_t)g_ptrValidationCave, pos);
            InterlockedExchange(&g_ptrValidationPatched, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("FAIL cascade ptr validation: exception during patch");
        }
    }

    // =========================================================================
    // Step 10: Restore mask writer to full rotation (only after both arrays ready)
    // =========================================================================
    static void TryRestoreMaskRotation()
    {
        if (g_maskRestored) return;
        if (!g_vrExpanded) return;
        if (!g_maskSafe) return;

        uintptr_t base = GetModuleBase();

        // Verify flat cascade array has 4 valid entries before restoring
        static volatile long g_flatDiagLogged = 0;
        __try {
            uintptr_t sceneNode = *reinterpret_cast<uintptr_t*>(base + ShadowSceneNodePtr);
            if (sceneNode == 0) return;

            uintptr_t cascadeGroup = *reinterpret_cast<uintptr_t*>(sceneNode + CascadeGroupOffset);
            if (cascadeGroup == 0) return;

            uint32_t flatCount = *reinterpret_cast<uint32_t*>(cascadeGroup + FlatCountOffset);
            uintptr_t flatBuf = *reinterpret_cast<uintptr_t*>(cascadeGroup + FlatBufferOffset);

            // Log diagnostics once
            if (InterlockedCompareExchange(&g_flatDiagLogged, 1, 0) == 0) {
                uint32_t currentGlobal = *reinterpret_cast<uint32_t*>(base + CascadeCountPatch::CountGlobal);
                Log("=== Flat array diagnostics ===");
                Log("DAT_143924818 (current) = %u", currentGlobal);
                Log("Scene node: 0x%llX", sceneNode);
                Log("Cascade group: 0x%llX, vtable: 0x%llX",
                    cascadeGroup, *reinterpret_cast<uintptr_t*>(cascadeGroup));
                Log("Flat count (0x190): %u, buffer (0x198): 0x%llX", flatCount, flatBuf);
                uint32_t capacity = *reinterpret_cast<uint32_t*>(cascadeGroup + 0x1A0);
                Log("Flat capacity (0x1A0): %u", capacity);

                if (flatBuf != 0) {
                    for (uint32_t i = 0; i < flatCount && i < 8; i++) {
                        uintptr_t entry = flatBuf + i * FlatEntrySize;
                        Log("  flat[%u]: +0x50=0x%llX +0x58=0x%llX +0xF8=0x%llX",
                            i,
                            *reinterpret_cast<uintptr_t*>(entry + 0x50),
                            *reinterpret_cast<uintptr_t*>(entry + 0x58),
                            *reinterpret_cast<uintptr_t*>(entry + 0xF8));
                    }
                }
            }

            if (flatBuf == 0 || flatCount < 4) {
                return;  // Keep safe mode
            }

            // Check shadow map pointers at +0x50 for all 4 entries
            bool allValid = true;
            for (uint32_t i = 0; i < 4; i++) {
                uintptr_t entry = flatBuf + i * FlatEntrySize;
                uintptr_t shadowMap = *reinterpret_cast<uintptr_t*>(entry + FlatShadowMapOff);
                if (shadowMap == 0) { allValid = false; break; }
            }

            if (!allValid) {
                return;  // Keep safe mode until all shadow maps initialized
            }

            Log("All 4 flat entries valid!");
            // Log additional fields for cascade 3 investigation
            for (uint32_t i = 0; i < 4; i++) {
                uintptr_t entry = flatBuf + i * FlatEntrySize;
                Log("  flat[%u]: +0x40=0x%llX +0x48=0x%llX +0x102=%u",
                    i,
                    *reinterpret_cast<uintptr_t*>(entry + 0x40),
                    *reinterpret_cast<uintptr_t*>(entry + 0x48),
                    (uint32_t)*reinterpret_cast<uint8_t*>(entry + 0x102));
            }

            // ======= v11.0.0: Shadow distance diagnostics =======
            {
                float dist4 = *reinterpret_cast<float*>(base + ShadowDist4Cascade);
                float dist2 = *reinterpret_cast<float*>(base + ShadowDist2Cascade);
                uint32_t countGlobal = *reinterpret_cast<uint32_t*>(base + CascadeCountPatch::CountGlobal);
                Log("=== Shadow distance diagnostics ===");
                Log("DAT_143924818 (cascade count) = %u", countGlobal);
                Log("Shadow dist 4-cascade (0x2c7f648) = %.1f", dist4);
                Log("Shadow dist 2-cascade (0x3924808) = %.1f", dist2);
                Log("Setup CMP patched: %s (should use 4-cascade distance)",
                    *reinterpret_cast<uint8_t*>(base + CountReadPatch::SetupCmpImm) == 0x00 ? "YES" : "NO");
            }

            // ======= Shader and cascade group diagnostics =======
            Log("=== Cascade group & shader diagnostics ===");

            // Read VR flag at cascade_group+0x173
            uint8_t vrFlag = *reinterpret_cast<uint8_t*>(cascadeGroup + CascadeGroupVRFlag);
            Log("cascade_group+0x173 (VR flag): %u", (uint32_t)vrFlag);

            // Read shader object state
            uintptr_t shaderObj = *reinterpret_cast<uintptr_t*>(cascadeGroup + ShaderObjectOffset);
            if (shaderObj != 0) {
                uint32_t shaderField158 = *reinterpret_cast<uint32_t*>(shaderObj + 0x158);
                uint16_t shaderCap = *reinterpret_cast<uint16_t*>(shaderObj + 0x158 + 0x10);
                uint16_t shaderCount = *reinterpret_cast<uint16_t*>(shaderObj + 0x158 + 0x12);
                Log("shader+0x158 (cascade field): %u", shaderField158);
                Log("shader+0x168 (array capacity): %u, +0x16A (array count): %u",
                    (uint32_t)shaderCap, (uint32_t)shaderCount);
                Log("shader+0x11C: %u, shader+0x1D8 (stored count): %u",
                    (uint32_t)*reinterpret_cast<uint8_t*>(shaderObj + 0x11C),
                    *reinterpret_cast<uint32_t*>(shaderObj + 0x1D8));
            } else {
                Log("WARN: shader object is NULL at cascade_group+0x2B8");
            }

            // Force VR flag = 1 so shader processes 4 cascades (not 3)
            if (vrFlag == 0) {
                *reinterpret_cast<uint8_t*>(cascadeGroup + CascadeGroupVRFlag) = 1;
                Log("Forced cascade_group+0x173 = 1 (shader will use 4 cascades)");
            }

            // Shadow map validation
            Log("=== Shadow map validation ===");
            for (uint32_t i = 0; i < 4; i++) {
                uintptr_t entry = flatBuf + i * FlatEntrySize;
                uintptr_t leftMap = *reinterpret_cast<uintptr_t*>(entry + FlatShadowMapOff);
                uintptr_t rightMap = *reinterpret_cast<uintptr_t*>(entry + FlatShadowMapRightOff);
                Log("  cascade[%u]: L=0x%llX R=0x%llX eye_flag: L=%u R=%u",
                    i, leftMap, rightMap,
                    leftMap ? (uint32_t)*reinterpret_cast<uint8_t*>(leftMap + 0xf6dc) : 99,
                    rightMap ? (uint32_t)*reinterpret_cast<uint8_t*>(rightMap + 0xf6dc) : 99);
                // v11.0.0: Log rendering function index and scene node binding
                if (leftMap) {
                    int32_t lFuncIdx = *reinterpret_cast<int32_t*>(leftMap + 0xf688);
                    uintptr_t lSceneNode = *reinterpret_cast<uintptr_t*>(leftMap + 0xf680);
                    Log("    L: funcIdx=%d sceneNode=0x%llX", lFuncIdx, lSceneNode);
                }
                if (rightMap) {
                    int32_t rFuncIdx = *reinterpret_cast<int32_t*>(rightMap + 0xf688);
                    uintptr_t rSceneNode = *reinterpret_cast<uintptr_t*>(rightMap + 0xf680);
                    Log("    R: funcIdx=%d sceneNode=0x%llX", rFuncIdx, rSceneNode);
                }
            }

            // Clear the "last cascade" flag on flat[3]
            {
                // Heap memory is already PAGE_READWRITE — no VirtualProtect needed
                uint8_t* pFlag = reinterpret_cast<uint8_t*>(flatBuf + 3 * FlatEntrySize + 0x102);
                uint8_t oldVal = *pFlag;
                if (oldVal != 0) {
                    *pFlag = 0;
                    Log("Cleared flat[3]+0x102 'last cascade' flag: %u -> 0", (uint32_t)oldVal);
                } else {
                    Log("flat[3]+0x102 already 0, no change needed");
                }
            }

            Log("Enabling 4-cascade mode (mask=0xF ALL frames): cascades 0,1,2,3");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }

        // Apply safety patches BEFORE enabling cascade 3
        PatchCascadeEntryZeroInit();  // ROOT CAUSE: zero per-cascade ptrs on first use
        PatchNodeAllocator();         // Defense: clear ->next on node reuse
        PatchNullSafetyCheck();       // Defense: null check in FUN_142813740
        PatchCascadePtrValidation();  // Defense: pointer range check at crash site
        if (!g_entryZeroInitPatched || !g_nodeAllocPatched || !g_nullSafePatched || !g_ptrValidationPatched) {
            Log("WARN: safety patches incomplete (zeroinit=%ld, node=%ld, null=%ld, ptrval=%ld), staying in safe mode",
                g_entryZeroInitPatched, g_nodeAllocPatched, g_nullSafePatched, g_ptrValidationPatched);
            return;
        }

        // v10.0.0: Force ALL mask values to 0xF — render all 4 cascades EVERY frame.
        // Eliminates temporal rotation {0xF,0x5,0xF,0x9} which caused:
        //   - LEFT eye flickering (cascades missing on non-0xF frames)
        //   - Possible RIGHT eye issues due to stale/missing temporal data
        // Trade-off: ~2x shadow rendering cost, but VR has the GPU headroom.
        using namespace MaskWriterPatch;

        int n = 0;
        if (PatchByte(base + InitMask_Byte, 0x03, 0x0F,
                      "initial mask 0x3->0xF")) n++;
        if (PatchByte(base + FallbackMask_Byte, 0x03, 0x0F,
                      "fallback mask 0x3->0xF")) n++;
        if (PatchByte(base + ArrayEntry1_Byte, 0x03, 0x0F,
                      "array[1] 0x3->0xF")) n++;
        if (PatchByte(base + ArrayEntry3_Byte, 0x03, 0x0F,
                      "array[3] 0x3->0xF")) n++;

        Log("4-cascade mode: %d/4 patches applied (ALL frames render ALL cascades, mask=0xF)", n);
        InterlockedExchange(&g_maskRestored, 1);
    }

    // =========================================================================
    // v13.0.0: Fix setup scene node (DAT_146885d40)
    // The VR engine adds a second scene node pointer for shadow setup but never
    // initializes it. BSShaderManager::SetShadowSceneNode(1, ...) is never called.
    // Fix: copy the render scene node pointer to the setup slot.
    // This mirrors the SE behavior where only one scene node exists.
    // =========================================================================
    static volatile long g_setupNodeFixed = 0;

    static void FixSetupSceneNode()
    {
        if (g_setupNodeFixed) return;

        uintptr_t base = GetModuleBase();
        __try {
            uintptr_t renderNode = *reinterpret_cast<uintptr_t*>(base + ShadowSceneNodePtr);
            if (renderNode == 0) return;  // Render node not yet initialized

            uintptr_t setupNode = *reinterpret_cast<uintptr_t*>(base + ShadowSceneNodePtr2);
            if (setupNode != 0) {
                // Already valid — might have been set by the engine or a previous fix
                InterlockedExchange(&g_setupNodeFixed, 1);
                return;
            }

            // Copy render node to setup node
            // .data section is already PAGE_READWRITE — no VirtualProtect needed
            volatile uintptr_t* pSetup = reinterpret_cast<volatile uintptr_t*>(base + ShadowSceneNodePtr2);
            *pSetup = renderNode;
            Log("Setup scene node fixed: NULL -> 0x%llX (copied from render node)", renderNode);

            // Also verify by reading cascade group from the now-valid setup node
            uintptr_t cg = *reinterpret_cast<uintptr_t*>(renderNode + CascadeGroupOffset);
            Log("  Setup cascade group: 0x%llX (via render node+0x248)", cg);

            InterlockedExchange(&g_setupNodeFixed, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("FixSetupSceneNode: exception caught");
        }
    }

    // =========================================================================
    // v13.0.0: Force shader cascade fields on cascade group's shader object
    // The ISCopy shader at cascade_group+0x2B8 may have been constructed before
    // our constructor patches, leaving capacity/count at 0 or 2.
    // Force fields to 4 so the ISCopy shader processes all 4 cascades.
    // =========================================================================
    static volatile long g_shaderFieldsForced = 0;

    static void ForceShaderFields()
    {
        if (g_shaderFieldsForced) return;
        uintptr_t base = GetModuleBase();
        __try {
            // Process both scene nodes' cascade groups
            uintptr_t nodeAddrs[] = { ShadowSceneNodePtr, ShadowSceneNodePtr2 };
            const char* labels[] = { "RENDER", "SETUP" };

            for (int n = 0; n < 2; n++) {
                uintptr_t sceneNode = *reinterpret_cast<uintptr_t*>(base + nodeAddrs[n]);
                if (sceneNode == 0) continue;

                uintptr_t cg = *reinterpret_cast<uintptr_t*>(sceneNode + CascadeGroupOffset);
                if (cg == 0) continue;

                uintptr_t shader = *reinterpret_cast<uintptr_t*>(cg + ShaderObjectOffset);
                if (shader == 0) continue;

                // Force shader+0x1D8 (stored cascade count)
                uint32_t* pStored = reinterpret_cast<uint32_t*>(shader + 0x1D8);
                if (*pStored < 4) {
                    uint32_t old = *pStored;
                    *pStored = 4;
                    if (InterlockedCompareExchange(&g_shaderFieldsForced, 1, 0) == 0) {
                        Log("Forced %s shader+0x1D8 (stored count): %u -> 4", labels[n], old);
                    }
                }

                // Force shader+0x168 (array capacity) — uint16_t
                uint16_t* pCap = reinterpret_cast<uint16_t*>(shader + 0x168);
                if (*pCap < 4) {
                    uint16_t old = *pCap;
                    *pCap = 4;
                    if (g_shaderFieldsForced == 1) {
                        Log("Forced %s shader+0x168 (array cap): %u -> 4", labels[n], (uint32_t)old);
                    }
                }

                // Force shader+0x16A (array count) — uint16_t
                uint16_t* pCnt = reinterpret_cast<uint16_t*>(shader + 0x16A);
                if (*pCnt < 4) {
                    uint16_t old = *pCnt;
                    *pCnt = 4;
                    if (g_shaderFieldsForced == 1) {
                        Log("Forced %s shader+0x16A (array count): %u -> 4", labels[n], (uint32_t)old);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // =========================================================================
    // v12.0.0: Force cascade_group+0x173 = 1 on BOTH scene nodes' cascade groups
    // Also fix 4-cascade shadow distance if it's FLT_MAX (uninitialized in VR mode)
    // =========================================================================
    static volatile long g_extDiagLogged = 0;

    static volatile long g_cascadeGroupsForced = 0;

    static void ForceBothCascadeGroups()
    {
        if (g_cascadeGroupsForced) return;

        uintptr_t base = GetModuleBase();
        __try {
            // Need both scene nodes to be valid before forcing
            uintptr_t sceneNode1 = *reinterpret_cast<uintptr_t*>(base + ShadowSceneNodePtr);
            if (sceneNode1 == 0) return;
            uintptr_t cg1 = *reinterpret_cast<uintptr_t*>(sceneNode1 + CascadeGroupOffset);
            if (cg1 == 0) return;

            // Force +0x173 on rendering cascade group
            uint8_t* pFlag1 = reinterpret_cast<uint8_t*>(cg1 + CascadeGroupVRFlag);
            if (*pFlag1 == 0) *pFlag1 = 1;

            // Force +0x173 on setup cascade group (same object after FixSetupSceneNode)
            uintptr_t sceneNode2 = *reinterpret_cast<uintptr_t*>(base + ShadowSceneNodePtr2);
            if (sceneNode2 != 0) {
                uintptr_t cg2 = *reinterpret_cast<uintptr_t*>(sceneNode2 + CascadeGroupOffset);
                if (cg2 != 0) {
                    uint8_t* pFlag2 = reinterpret_cast<uint8_t*>(cg2 + CascadeGroupVRFlag);
                    if (*pFlag2 == 0) *pFlag2 = 1;
                }
            }

            InterlockedExchange(&g_cascadeGroupsForced, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // =========================================================================
    // v12.0.0: Extended diagnostics — log shader state after full initialization
    // =========================================================================
    static void LogExtendedDiagnostics()
    {
        if (InterlockedCompareExchange(&g_extDiagLogged, 1, 0) != 0) return;

        uintptr_t base = GetModuleBase();
        __try {
            Log("=== v13.3.0 Extended Diagnostics ===");
            Log("Setup scene node fixed: %s", g_setupNodeFixed ? "YES" : "NO");
            Log("Shader fields forced: %s", g_shaderFieldsForced ? "YES" : "NO");
            Log("VR entries refreshed: %s", g_vrEntriesRefreshed ? "YES" : "NO");

            // Check both scene nodes
            uintptr_t sn1 = *reinterpret_cast<uintptr_t*>(base + ShadowSceneNodePtr);
            uintptr_t sn2 = *reinterpret_cast<uintptr_t*>(base + ShadowSceneNodePtr2);
            Log("Scene node RENDER (0x6879520): 0x%llX", sn1);
            Log("Scene node SETUP  (0x6885d40): 0x%llX", sn2);
            Log("Same object: %s", (sn1 == sn2) ? "YES" : "NO");

            uintptr_t cg1 = 0, cg2 = 0;
            if (sn1) cg1 = *reinterpret_cast<uintptr_t*>(sn1 + CascadeGroupOffset);
            if (sn2) cg2 = *reinterpret_cast<uintptr_t*>(sn2 + CascadeGroupOffset);
            Log("Cascade group RENDER: 0x%llX", cg1);
            Log("Cascade group SETUP:  0x%llX", cg2);
            Log("Same cascade group: %s", (cg1 == cg2) ? "YES" : "NO");

            // Check +0x173 and shader on both cascade groups
            for (int g = 0; g < 2; g++) {
                uintptr_t cg = (g == 0) ? cg1 : cg2;
                const char* label = (g == 0) ? "RENDER" : "SETUP";
                if (cg == 0) { Log("  %s cascade group is NULL", label); continue; }

                uint8_t vrFlag = *reinterpret_cast<uint8_t*>(cg + CascadeGroupVRFlag);
                uintptr_t shaderObj = *reinterpret_cast<uintptr_t*>(cg + ShaderObjectOffset);
                Log("  %s cg+0x173=%u, shader=0x%llX", label, (uint32_t)vrFlag, shaderObj);

                if (shaderObj != 0) {
                    uint32_t s158 = *reinterpret_cast<uint32_t*>(shaderObj + 0x158);
                    uint32_t s1D8 = *reinterpret_cast<uint32_t*>(shaderObj + 0x1D8);
                    uint16_t cap = *reinterpret_cast<uint16_t*>(shaderObj + 0x168);
                    uint16_t cnt = *reinterpret_cast<uint16_t*>(shaderObj + 0x16A);
                    Log("    shader+0x158(cascades)=%u, +0x1D8(stored)=%u, +0x168(cap)=%u, +0x16A(cnt)=%u",
                        s158, s1D8, (uint32_t)cap, (uint32_t)cnt);
                }
            }

            // Shadow distance after fix
            float dist4 = *reinterpret_cast<float*>(base + ShadowDist4Cascade);
            float dist2 = *reinterpret_cast<float*>(base + ShadowDist2Cascade);
            Log("Shadow dist 4-cascade: %.1f, 2-cascade: %.1f", dist4, dist2);

            // VR state flags
            uint8_t vrInstStereo = *reinterpret_cast<uint8_t*>(base + VRInstStereoFlag);
            uint8_t vrInstDraw = *reinterpret_cast<uint8_t*>(base + VRInstDrawFlag);
            Log("VR instanced stereo (0x391d848): %u", (uint32_t)vrInstStereo);
            Log("VR instanced draw   (0x388a808): %u", (uint32_t)vrInstDraw);

            // VR array entry state (post-refresh)
            {
                uintptr_t vrBuf = *reinterpret_cast<uintptr_t*>(base + VRArrayExpansion::ArrayPtr);
                uint32_t vrCount = *reinterpret_cast<uint32_t*>(base + VRArrayExpansion::ArrayCount);
                Log("VR array: ptr=0x%llX, count=%u", vrBuf, vrCount);
                if (vrBuf != 0) {
                    for (uint32_t i = 0; i < 4 && i < vrCount; i++) {
                        uintptr_t entry = vrBuf + i * VRArrayExpansion::EntrySize;
                        const uint8_t* p = reinterpret_cast<const uint8_t*>(entry);
                        int nonZero = 0;
                        for (size_t off = 0; off < VRArrayExpansion::EntrySize; off++) {
                            if (p[off] != 0) nonZero++;
                        }
                        // Show first 64 bytes of each entry
                        Log("  VR[%u] (%d/%zu nz): %02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X",
                            i, nonZero, VRArrayExpansion::EntrySize,
                            p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                            p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15],
                            p[16],p[17],p[18],p[19],p[20],p[21],p[22],p[23],
                            p[24],p[25],p[26],p[27],p[28],p[29],p[30],p[31],
                            p[32],p[33],p[34],p[35],p[36],p[37],p[38],p[39],
                            p[40],p[41],p[42],p[43],p[44],p[45],p[46],p[47],
                            p[48],p[49],p[50],p[51],p[52],p[53],p[54],p[55],
                            p[56],p[57],p[58],p[59],p[60],p[61],p[62],p[63]);
                    }
                }
            }

            // Descriptor arrays
            for (int d = 0; d < 3; d++) {
                uintptr_t descBase = (d == 0) ? DescArray0 : (d == 1) ? DescArray1 : DescArray2;
                uintptr_t arrPtr = *reinterpret_cast<uintptr_t*>(base + descBase);
                uint32_t arrCount = *reinterpret_cast<uint32_t*>(base + descBase + 0x10);
                Log("DescArray[%d] (0x%X): ptr=0x%llX, count=%u", d, (uint32_t)descBase, arrPtr, arrCount);

                // Compare descriptor entries to known LEFT/RIGHT flat array maps
                if (arrPtr != 0 && arrCount > 0 && cg1 != 0) {
                    uintptr_t flatBuf = *reinterpret_cast<uintptr_t*>(cg1 + FlatBufferOffset);
                    for (uint32_t e = 0; e < arrCount && e < 8; e++) {
                        uintptr_t mapPtr = *reinterpret_cast<uintptr_t*>(arrPtr + e * 8);
                        // Check if this matches any LEFT or RIGHT flat map
                        const char* match = "?";
                        if (flatBuf) {
                            for (uint32_t c = 0; c < 4; c++) {
                                uintptr_t lm = *reinterpret_cast<uintptr_t*>(flatBuf + c * FlatEntrySize + FlatShadowMapOff);
                                uintptr_t rm = *reinterpret_cast<uintptr_t*>(flatBuf + c * FlatEntrySize + FlatShadowMapRightOff);
                                if (mapPtr == lm) {
                                    static char buf[32]; snprintf(buf, sizeof(buf), "LEFT[%u]", c);
                                    match = buf; break;
                                }
                                if (mapPtr == rm) {
                                    static char buf[32]; snprintf(buf, sizeof(buf), "RIGHT[%u]", c);
                                    match = buf; break;
                                }
                            }
                        }
                        Log("    [%u] 0x%llX = %s", e, mapPtr, match);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("Extended diagnostics: exception");
        }
    }

    // =========================================================================
    // Timer callback: polls for VR array expansion from Windows thread pool
    // v12.0.0: Continues running after activation for diagnostics and +0x173 forcing
    // =========================================================================
    static VOID CALLBACK ExpansionTimerCallback(PVOID /*lpParameter*/, BOOLEAN /*TimerOrWaitFired*/)
    {
        static volatile long s_tickCount = 0;
        long tick = InterlockedIncrement(&s_tickCount);

        // Keep forcing cascade count = 4 (belt-and-suspenders with instruction patches)
        ForceCascadeCount4();

        // v12.0.0: Always force +0x173 on both cascade groups and fix shadow distance
        ForceBothCascadeGroups();

        // v13.0.0: Fix setup scene node (NULL in VR) and force shader fields
        FixSetupSceneNode();
        ForceShaderFields();

        // v13.2.0: RefreshVRArrayEntries disabled — never triggers and adds heap reads during loading

        if (!g_maskRestored) {
            // Log state on first few ticks
            if (tick <= 3 || (tick % 20) == 0) {
                Log("Timer tick #%ld: vrExpanded=%ld, maskRestored=%ld",
                    tick, g_vrExpanded, g_maskRestored);
            }

            TryExpandVRArray();
            TryRestoreMaskRotation();

            if (g_maskRestored) {
                Log("4-cascade shadow rendering active (via timer, tick #%ld)", tick);
            }
        } else {
            // v12.0.0: After activation, run extended diagnostics once (after ~5s of gameplay)
            if (tick > 30) {
                LogExtendedDiagnostics();

                // Kill timer after diagnostics logged (no longer needed)
                if (g_extDiagLogged && g_timerHandle) {
                    DeleteTimerQueueTimer(nullptr, g_timerHandle, nullptr);
                    g_timerHandle = nullptr;
                }
            }
        }
    }

    static void StartExpansionTimer()
    {
        if (g_timerStarted) return;
        if (!g_maskSafe) return;
        if (g_maskRestored) return;

        if (InterlockedCompareExchange(&g_timerStarted, 1, 0) != 0) return;

        BOOL ok = CreateTimerQueueTimer(
            &g_timerHandle,
            nullptr,
            ExpansionTimerCallback,
            nullptr,
            2000,              // 2s initial delay
            500,               // 500ms polling interval
            WT_EXECUTEDEFAULT
        );

        if (ok) {
            Log("Expansion timer started (2s delay, 500ms interval)");
        } else {
            Log("WARN: CreateTimerQueueTimer failed, error %u", GetLastError());
            InterlockedExchange(&g_timerStarted, 0);
        }
    }

    // =========================================================================
    // Mask clamp (backup, active before code patches apply)
    // =========================================================================
    static void ClampMask()
    {
        if (g_maskSafe) return;

        __try {
            uintptr_t base = GetModuleBase();
            uint32_t* pMask = reinterpret_cast<uint32_t*>(base + CascadeMaskGlobal);
            uint32_t mask = *pMask;
            if (mask > 0x3) {
                *pMask = mask & 0x3;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // =========================================================================
    // Public API
    // =========================================================================
    bool Initialize()
    {
        OutputDebugStringA("[VRShadowCascade] DllMain: version.dll proxy loaded\n");
        return true;
    }

    void EnsureInitialized()
    {
        // Fast path: once the timer is running, all continuous work is done there.
        // Avoids doing VirtualProtect/memory ops on every version.dll proxy call,
        // which caused timing interference with BackgroundProcessThread NIF loading.
        if (g_timerStarted) return;

        // One-time log setup
        if (InterlockedCompareExchange(&g_logInitialized, 1, 0) == 0) {
            InitializeCriticalSection(&g_logLock);
            g_logLockReady = true;

            char logPath[MAX_PATH];
            GetModuleFileNameA(nullptr, logPath, MAX_PATH);
            char* lastSlash = strrchr(logPath, '\\');
            if (lastSlash) {
                strcpy(lastSlash + 1, "VRShadowCascade.log");
            }
            g_logFile = fopen(logPath, "w");

            Log("VR Shadow Cascade Pre-loader v13.4.1 (no .rdata VP: redirect setup to .data distance)");
            Log("Module base: 0x%llX", GetModuleBase());
        }

        // Force cascade count to 4 (covers window before instruction patches)
        ForceCascadeCount4();

        // Progression after SteamStub decryption:
        // 1. Patch MOV instructions to load 4 instead of reading DAT_143924818
        // 2. Apply mask safe mode (force 0x3)
        // 3. Patch shader constructor
        // 4. Expand VR array (when initialized)
        // 5. Restore full mask rotation (after both arrays have 4 valid entries)
        CheckTextDecrypted();
        PatchCountReadSites();
        ApplyMaskSafeMode();
        PatchShaderCtor();
        PatchStereoDispatch();
        // Write desired shadow distance to .data address (no VirtualProtect needed).
        // The CMP patch above makes FUN_14290dbd0 read from ShadowDist2Cascade (.data)
        // instead of ShadowDist4Cascade (.rdata). We set the .data value to 5x original.
        // Must run AFTER SteamStub decryption — .data values may not be valid before.
        if (!g_shadowDistPatched && g_textDecrypted) {
            __try {
                uintptr_t base = GetModuleBase();
                float* pDist2 = reinterpret_cast<float*>(base + ShadowDist2Cascade);
                float origDist2 = *pDist2;
                if (origDist2 > 0.0f && origDist2 < 1e10f) {
                    float desiredDist = origDist2 * 5.0f;
                    *pDist2 = desiredDist;
                    InterlockedExchange(&g_shadowDistPatched, 1);
                    Log("Shadow distance: wrote %.1f to .data (was %.1f, no .rdata VP needed)", desiredDist, origDist2);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("WARN: shadow distance write failed (exception)");
            }
        }
        TryExpandVRArray();
        TryRestoreMaskRotation();
        ClampMask();

        StartExpansionTimer();
    }

    void Shutdown()
    {
        if (g_timerHandle) {
            DeleteTimerQueueTimer(nullptr, g_timerHandle, INVALID_HANDLE_VALUE);
            g_timerHandle = nullptr;
        }

        if (g_logLockReady) {
            Log("=== Shutdown ===");
            Log("Count reads patched: %s", g_countReadsPatched ? "YES" : "NO");
            Log("Mask safe mode: %s", g_maskSafe ? "YES" : "NO");
            Log("Shader patched: %s", g_shaderPatched ? "YES" : "NO");
            Log("Stereo dispatch fix: %s", g_stereoFixPatched ? "YES" : "NO");
            Log("Shadow dist patched: %s", g_shadowDistPatched ? "YES" : "NO");
            Log("VR expanded: %s", g_vrExpanded ? "YES" : "NO");
            Log("Entry zero-init patched: %s", g_entryZeroInitPatched ? "YES" : "NO");
            Log("Node alloc patched: %s", g_nodeAllocPatched ? "YES" : "NO");
            Log("Null safety patched: %s", g_nullSafePatched ? "YES" : "NO");
            Log("Ptr validation patched: %s", g_ptrValidationPatched ? "YES" : "NO");
            Log("Setup scene node fixed: %s", g_setupNodeFixed ? "YES" : "NO");
            Log("Shader fields forced: %s", g_shaderFieldsForced ? "YES" : "NO");
            Log("VR entries refreshed: %s", g_vrEntriesRefreshed ? "YES" : "NO");
            Log("Mask restored: %s", g_maskRestored ? "YES" : "NO");
        }

        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }

        if (g_logLockReady) {
            DeleteCriticalSection(&g_logLock);
            g_logLockReady = false;
        }
    }
}
