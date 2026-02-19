#pragma once

#include "Config.h"

// ============================================================================
// Shadow Boost F4VR - Dynamic FPS-Based Quality Adjustment
// ============================================================================
// Ported from Shadow Boost FO4 by PK0 (https://github.com/P-K-0/Shadow-Boost-FO4)
// Dynamically adjusts shadow distance, LOD, grass, block levels, and god rays
// based on real-time frame rate to maintain target FPS.
//
// Cascade expansion (2→4) is handled by the version.dll proxy.
// This plugin handles shadow distance + all dynamic quality scaling.
// ============================================================================

namespace ShadowBoostF4VR
{
    namespace offsets
    {
        // ShadowDist2Cascade - Cascade split range (.data, writable)
        // Managed by version.dll proxy (reads original, multiplies by 5x for 4 cascades)
        // We only READ this for logging — never write, to avoid overriding the proxy's value
        inline REL::Relocation<float*> ShadowDist2Cascade{ REL::Offset(0x3924808) };

        // Shadow system global object base at DAT_1468787f0 (RVA 0x68787f0)
        // The renderer caches shadow distance at +0x100 during init and never re-reads
        // from RE::Setting. We must write here directly for runtime changes to take effect.
        inline REL::Relocation<float*> ShadowDistRenderer{ REL::Offset(0x68788f0) };
    }

    // ========================================================================
    // Shared shadow maps: force RIGHT eye to use LEFT shadow maps
    // Patches FUN_14290d640's VR instanced path: displacement 0x58 → 0x50
    // at two MOV instructions so both eyes dispatch with the LEFT scene node.
    // Applied AFTER game load to avoid infinite loading screen.
    // ========================================================================
    namespace SharedShadowFix
    {
        // In FUN_14290d640 VR instanced path:
        //   14290d9cd: MOV RCX,[R15+0x58]  — load RIGHT shadow map for activate
        //   14290d9d6: MOV RDX,[R15+0x58]  — load RIGHT shadow map for dispatch
        // Patch displacement byte 0x58 → 0x50 to use LEFT shadow map instead.
        constexpr std::uintptr_t RightActivate_Offset = 0x290d9d0;  // disp8 byte
        constexpr std::uintptr_t RightDispatch_Offset = 0x290d9d9;  // disp8 byte
        constexpr std::uint8_t OldDisp = 0x58;
        constexpr std::uint8_t NewDisp = 0x50;

        bool Apply();
    }

    class ShadowBoost
    {
    public:
        static ShadowBoost& GetSingleton()
        {
            static ShadowBoost instance;
            return instance;
        }

        bool init(Config* config);
        void update(float deltaTime);
        void applyGodRays();

    private:
        ShadowBoost() = default;

        bool cacheGameSettings();
        void saveOriginalValues();
        void restoreOriginalValues();

        Config* _config = nullptr;
        bool    _initialized = false;

        // Cached game setting pointers (resolved once on init)
        RE::Setting* _fDirShadowDistance     = nullptr;
        RE::Setting* _fLODFadeOutMultObjects = nullptr;
        RE::Setting* _fLODFadeOutMultItems   = nullptr;
        RE::Setting* _fLODFadeOutMultActors  = nullptr;
        RE::Setting* _fGrassStartFadeDistance = nullptr;
        RE::Setting* _fBlockLevel0Distance   = nullptr;
        RE::Setting* _fBlockLevel1Distance   = nullptr;
        RE::Setting* _fBlockLevel2Distance   = nullptr;

        // God rays (optional, may not exist in VR)
        RE::Setting* _grQuality = nullptr;
        RE::Setting* _grGrid    = nullptr;
        RE::Setting* _grScale   = nullptr;
        RE::Setting* _grCascade = nullptr;

        // Original values for restore
        float o_dirShadowDist    = 0.0f;
        float o_lodObjects       = 0.0f;
        float o_lodItems         = 0.0f;
        float o_lodActors        = 0.0f;
        float o_grassDist        = 0.0f;
        float o_blockLevel0      = 0.0f;
        float o_blockLevel1      = 0.0f;
        float o_blockLevel2      = 0.0f;
        std::int32_t o_grQuality = 0;
        std::int32_t o_grGrid    = 0;
        float        o_grScale   = 0.0f;
        std::int32_t o_grCascade = 0;

        // FPS tracking
        std::chrono::steady_clock::time_point _lastTime{};
        float _frameCount = 0.0f;
        float _targetMs   = 0.0f;
        int   _blockIndex = 0;
        int   _debugCounter = 0;
    };

} // namespace ShadowBoostF4VR
