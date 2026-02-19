#pragma once

#include <Windows.h>
#include <cstdint>

namespace CascadePatch
{
    // =========================================================================
    // Game offsets (Fallout 4 VR 1.2.72, relative to module base 0x140000000)
    // All verified via Ghidra disassembly
    // =========================================================================

    // Cascade mask global - written each frame by FUN_14284e9e0
    // Rotates through {0x3, 0x5, 0x3, 0x9} for 4-cascade temporal rendering
    constexpr uintptr_t CascadeMaskGlobal = 0x6885cc4;

    // SteamStub decryption sentinel (any .text function entry)
    // Used to detect when .text section is readable
    constexpr uintptr_t TextSentinel         = 0x27c33d0;  // shader ctor entry
    constexpr uint8_t   TextSentinelExpected = 0x48;        // MOV [RSP+8],RBX

    // ---- Cascade count global ----
    // DAT_143924818: uint32 read by 4 code sites
    // Binary value is 4 but VR init overwrites to 2 via indirect/INI write
    namespace CascadeCountPatch
    {
        constexpr uintptr_t CountGlobal = 0x3924818;
        constexpr uint32_t  DesiredValue = 4;
    }

    // ---- Patch A: Instruction patches for cascade count reads ----
    // All 4 sites read DAT_143924818 via MOV reg, [RIP+disp32]
    // We patch each to MOV reg, 4 (immediate) so the game always uses 4 cascades
    namespace CountReadPatch
    {
        // FUN_1427e8f50 (shadow scene node ctor) - controls flat array entry count
        constexpr uintptr_t CtorRead     = 0x27e929a;
        // FUN_14290dbd0 (shadow setup) - selects 2-vs-4 cascade distance path
        constexpr uintptr_t SetupRead    = 0x290dc03;
        // FUN_1428a4a60 (render target setup) - two reads
        constexpr uintptr_t RenderRead1  = 0x28a57a0;
        constexpr uintptr_t RenderRead2  = 0x28a5c3c;

        constexpr uintptr_t AllSites[] = { CtorRead, SetupRead, RenderRead1, RenderRead2 };

        // The setup read at 0x290dc03 is a CMP instruction (not MOV), so PatchMovRipToImm
        // cannot handle it. It compares DAT_143924818 == 2 to select 2-cascade shadow distance.
        // We patch the immediate from 0x02 to 0x04 so the comparison SUCCEEDS (count==4),
        // making the function read shadow distance from DAT_143924808 (.data, writable)
        // instead of DAT_142c7f648 (.rdata, read-only — VirtualProtect on .rdata causes crashes).
        // We then write the desired 4-cascade distance to the .data address.
        constexpr uintptr_t SetupCmpImm  = 0x290dc09;  // immediate byte in CMP [rip+disp], imm8
        constexpr uint8_t   SetupCmpOld  = 0x02;
        constexpr uint8_t   SetupCmpNew  = 0x04;       // 4 == 4 → takes .data distance path
    }

    // ---- Patch B: Shader constructor (BSImagespaceShaderCopyShadowMapToArray) ----
    // .text section - encrypted by SteamStub
    namespace ShaderCtorPatch
    {
        // MOV EDX, 2 (BA 02 00 00 00) at 0x27c340b - shader texture array capacity
        constexpr uintptr_t ArrayCap_Byte = 0x27c340c;
        constexpr uint8_t   ArrayCap_Old  = 0x02;
        constexpr uint8_t   ArrayCap_New  = 0x04;

        // MOV dword ptr [RBX+0x1D8], 2 at 0x27c34d2 - shader stored cascade count
        constexpr uintptr_t StoredCount_Byte = 0x27c34d8;
        constexpr uint8_t   StoredCount_Old  = 0x02;
        constexpr uint8_t   StoredCount_New  = 0x04;
    }

    // ---- Fallback: Mask writer code patches (v5.1 safety) ----
    // If Patch A fails, patch mask writer to always use 0x3 (2-cascade safe mode)
    namespace MaskWriterPatch
    {
        constexpr uintptr_t InitMask_Byte     = 0x284e9fb;
        constexpr uint8_t   InitMask_Old      = 0x0F;
        constexpr uint8_t   InitMask_New      = 0x03;

        constexpr uintptr_t FallbackMask_Byte = 0x284ea38;
        constexpr uint8_t   FallbackMask_Old  = 0x0F;
        constexpr uint8_t   FallbackMask_New  = 0x03;

        constexpr uintptr_t ArrayEntry1_Byte  = 0x284ea4c;
        constexpr uint8_t   ArrayEntry1_Old   = 0x05;
        constexpr uint8_t   ArrayEntry1_New   = 0x03;

        constexpr uintptr_t ArrayEntry3_Byte  = 0x284ea5f;
        constexpr uint8_t   ArrayEntry3_Old   = 0x09;
        constexpr uint8_t   ArrayEntry3_New   = 0x03;
    }

    // ---- Null safety patch: FUN_142813740 ----
    // Crash at +0x3F: mov rbp, [r10+0x180] where r10 (param_2) can be NULL
    // param_2 comes from linked list node+0x18 which is NULL for cascade 3
    // The function checks *(param_2+0x180)==0 but not param_2==0 (latent game bug)
    namespace NullSafetyPatch
    {
        constexpr uintptr_t CrashInstrRVA = 0x281377F;  // mov rbp, [r10+0x180]
        constexpr size_t    InstrSize     = 7;           // 49 8B AA 80 01 00 00
    }

    // ---- Node allocator patch: FUN_14278e610 ----
    // Render pass node reuse function. When reusing an existing node (param_2 != NULL),
    // initializes +0x08, +0x10, +0x18, +0x48, +0x4c, +0x4d, +0x50 but NOT +0x40 (->next).
    // Stale +0x40 values cause linked list corruption when cascade 3 nodes are reused.
    // Function body: 0x14278e610 - 0x14278e6a1 (145 bytes)
    namespace NodeAllocPatch
    {
        constexpr uintptr_t FuncRVA = 0x278e610;
    }

    // ---- Cascade entry zero-init: FUN_1427a51e0 ----
    // BSLightingShaderProperty cascade lookup function. When a shadow tag is NOT
    // found in the internal array, it picks a slot and writes the tag. But the
    // RETURNED entry (at slot+5, containing per-cascade pointers) is never zeroed.
    // Cascade 3's pointer is uninitialized garbage, causing crashes at multiple sites.
    // Fix: zero the 0x20-byte returned entry in the "not found" path before return.
    namespace CascadeEntryZeroInit
    {
        // "mov [rax+r10+0x90], rdx" - the tag write in the "not found" path
        // 8 bytes: 4A 89 94 10 90 00 00 00
        constexpr uintptr_t TagWriteRVA  = 0x27A52A0;
        constexpr size_t    InstrSize    = 8;
        constexpr uintptr_t ReturnRVA    = 0x27A52A8;  // next instruction after tag write
    }

    // ---- Cascade array pointer validation: FUN_1427a3f90 ----
    // BSLightingShaderProperty render method. Code at +0xA53 loads a per-cascade
    // pointer from an internal array via [rax+rdi*8] where rdi=cascade index.
    // For cascade 3, the array entry is uninitialized garbage (e.g. 0x200000000000000).
    // The existing null check (test r14 / jz) doesn't catch non-zero garbage.
    // We replace it with a code cave that validates pointer range.
    namespace CascadePtrValidation
    {
        constexpr uintptr_t TestInstrRVA  = 0x27A49DA;  // test r14, r14 (4D 85 F6)
        constexpr size_t    PatchSize     = 9;           // test(3) + jz near(6)
        constexpr uintptr_t SkipTargetRVA = 0x27A4A6D;  // original jz target
        constexpr uintptr_t ContinueRVA  = 0x27A49E3;  // mov edi, [r14+0x48]
    }

    // ---- Stereo dispatch fix: FUN_14281bd40 ----
    // ROOT CAUSE of right-eye missing far shadows.
    // Flag=2 (LEFT deferred) sets bit 53 on geometry objects (param_2[0x21] |= 0x20000000000000).
    // Flag=1 (RIGHT immediate) checks bit 53: if set, SKIPS dispatch entirely.
    // This was designed for mono rendering; in VR stereo, RIGHT eye must render independently.
    // Fix: change JZ (conditional) to JMP (unconditional) so flag=1 always dispatches.
    namespace StereoDispatchFix
    {
        constexpr uintptr_t JzInstrRVA = 0x281be1c;  // JZ rel8 at this address
        constexpr uint8_t   JzOpcode   = 0x74;        // JZ opcode
        constexpr uint8_t   JmpOpcode  = 0xEB;        // JMP opcode (unconditional)
    }

    // ---- Shadow distance globals ----
    // FUN_14290dbd0 reads these to determine cascade shadow distances
    // DAT_142c7f648: 4-cascade shadow distance (used when cascade count != 2)
    // DAT_143924808: 2-cascade shadow distance (used when cascade count == 2, shorter)
    constexpr uintptr_t ShadowDist4Cascade = 0x2c7f648;
    constexpr uintptr_t ShadowDist2Cascade = 0x3924808;

    // ---- Shadow Scene Node globals ----
    // DAT_146879520: pointer to the shadow scene node used for RENDERING (FUN_14290d640)
    // DAT_146885d40: pointer to the shadow scene node used for SETUP (FUN_14290dbd0)
    // FUN_1428440c0 uses both: setup reads cascade group from DAT_146885d40+0x248,
    // rendering reads cascade group from DAT_146879520+0x248.
    // Cascade group object at +0x248, flat array buffer at cascade_group+0x198
    // Flat count at cascade_group+0x190, each entry 0x110 bytes
    constexpr uintptr_t ShadowSceneNodePtr  = 0x6879520;  // rendering scene node
    constexpr uintptr_t ShadowSceneNodePtr2 = 0x6885d40;  // setup scene node

    // BSShaderManager::SetShadowSceneNode(int, ShadowSceneNode*)
    // VR: 0x1427f54c0, SE: 0x1427d6190 (ID 325018, confidence 4)
    // Called with int=0 for render node, int=1 for setup node
    // DAT_146885d40 is VR-only (no SE equivalent) and never initialized by the engine.
    constexpr uintptr_t SetShadowSceneNodeFuncRVA = 0x27f54c0;
    constexpr uintptr_t CascadeGroupOffset = 0x248;
    constexpr uintptr_t FlatBufferOffset   = 0x198;
    constexpr uintptr_t FlatCountOffset    = 0x190;
    constexpr size_t    FlatEntrySize      = 0x110;
    constexpr size_t    FlatShadowMapOff   = 0x50;  // left shadow map ptr within flat entry
    constexpr size_t    FlatShadowMapRightOff = 0x58;  // right shadow map ptr within flat entry

    // ---- Cascade group internal fields ----
    // +0x173: VR cascade count flag byte. When nonzero, FUN_14290d640 sets
    //   shader+0x158 = 4 (all cascades). When zero, shader+0x158 = 3 (misses one).
    //   FUN_14290d640 line: *(shader+0x158) = (*(cascade_group+0x173) != 0) + 3
    constexpr size_t    CascadeGroupVRFlag = 0x173;

    // +0x2B8: BSImagespaceShaderCopyShadowMapToArray object pointer
    constexpr size_t    ShaderObjectOffset = 0x2B8;

    // ---- VR state globals ----
    // DAT_14391d848: Instanced stereo rendering flag (returned by FUN_1427e0dc0)
    //   When set, FUN_14290d640 takes VR Path C (dispatches BOTH +0x50 and +0x58)
    constexpr uintptr_t VRInstStereoFlag = 0x391d848;
    // DAT_14388a808: VR instanced draw flag (returned by FUN_141d4b6c0)
    //   When set, FUN_1428440c0 executes cascade processing
    constexpr uintptr_t VRInstDrawFlag   = 0x388a808;

    // ---- Shadow map descriptor arrays ----
    // Three global arrays at DAT_146886450/468/480, each with:
    //   +0x00: ptr to array of shadow map pointers (8 bytes each)
    //   +0x08: capacity
    //   +0x10: count
    // FUN_1427ff5d0 iterates these and binds to ISCopy shader
    constexpr uintptr_t DescArray0 = 0x6886450;
    constexpr uintptr_t DescArray1 = 0x6886468;
    constexpr uintptr_t DescArray2 = 0x6886480;

    // ---- FUN_14290d640: shader+0x158 computation ----
    // At RVA 0x290d685: CMP byte ptr [RSI+0x173], 0x00
    // SETNZ AL; ADD EAX, 3; MOV [R10+0x158], EAX
    // This computes shader+0x158 = (cascade_group+0x173 != 0) + 3
    // We can patch the ADD EAX, 3 to ADD EAX, 3 (already correct with +0x173=1)
    // OR patch the CMP to CMP [RSI+0x173], 0xFF so it's always nonzero
    // OR patch the entire sequence to MOV [R10+0x158], 4
    namespace ShaderCountPatch
    {
        // ADD EAX, 3 at RVA 0x290d68e — change 3 to 4 to always get 4+SETNZ(0or1)=4or5
        // Better: patch the MOV at 0x290d691 to write immediate 4
        // Actually simplest: just set +0x173=1 continuously (already works)
        constexpr uintptr_t AddImmRVA = 0x290d68e;
    }

    // ---- Patch C: VR cascade array expansion ----
    // Global container: ptr at +0x00, capacity at +0x08, count at +0x10
    namespace VRArrayExpansion
    {
        constexpr uintptr_t ArrayPtr   = 0x6878b18;  // DAT_146878b18
        constexpr uintptr_t ArrayCount = 0x6878b28;  // DAT_146878b28
        constexpr size_t    EntrySize  = 0x180;
        constexpr uint32_t  TargetCount = 4;

        // Self-referencing pointer offsets within each 0x180-byte VR entry
        // Empty-list pattern: ptr at (base + offset + 8) points to (base + offset)
        constexpr size_t PoolOffsets[] = { 0x70, 0xA8, 0xE8, 0x128 };
    }

    // Public API
    bool Initialize();        // Called from DllMain - minimal setup only
    void EnsureInitialized(); // Called from every proxy export
    void Shutdown();          // Called from DLL_PROCESS_DETACH

    void Log(const char* format, ...);
    uintptr_t GetModuleBase();
    bool IsFullyActive();     // All 3 patches applied + VR expanded
}
