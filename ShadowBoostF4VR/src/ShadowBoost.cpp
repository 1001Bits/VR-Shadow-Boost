#include "PCH.h"
#include "ShadowBoost.h"

namespace ShadowBoostF4VR
{
    constexpr float Millisecond = 1000.0f;

    // ========================================================================
    // Shared shadow maps patch — applied after game load
    // ========================================================================
    bool SharedShadowFix::Apply()
    {
        auto base = REL::Module::get().base();
        int applied = 0;

        auto patchByte = [&](std::uintptr_t offset, std::uint8_t oldVal, std::uint8_t newVal,
                             const char* label) -> bool {
            auto* addr = reinterpret_cast<std::uint8_t*>(base + offset);
            if (*addr == newVal) {
                logger::info("  {} already patched (0x{:02X})", label, newVal);
                applied++;
                return true;
            }
            if (*addr != oldVal) {
                logger::warn("  {} unexpected byte: 0x{:02X} (expected 0x{:02X})",
                    label, *addr, oldVal);
                return false;
            }
            DWORD oldProtect;
            if (!VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                logger::error("  {} VirtualProtect failed", label);
                return false;
            }
            *addr = newVal;
            VirtualProtect(addr, 1, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), addr, 1);
            logger::info("  {} patched: 0x{:02X} -> 0x{:02X}", label, oldVal, newVal);
            applied++;
            return true;
        };

        logger::info("Applying shared shadow maps (RIGHT eye uses LEFT shadow maps)...");
        patchByte(RightActivate_Offset, OldDisp, NewDisp, "activate disp");
        patchByte(RightDispatch_Offset, OldDisp, NewDisp, "dispatch disp");

        if (applied == 2) {
            logger::info("Shared shadow maps: 2/2 patches applied");
            return true;
        }
        logger::warn("Shared shadow maps: only {}/2 patches applied", applied);
        return false;
    }

    // Helper to look up a game setting and log result
    static RE::Setting* findSetting(const char* name)
    {
        auto* s = RE::GetINISetting(name);
        if (s) {
            logger::info("  Found: {} = {}", name, s->GetFloat());
        } else {
            logger::warn("  NOT FOUND: {}", name);
        }
        return s;
    }

    static RE::Setting* findSettingInt(const char* name)
    {
        auto* s = RE::GetINISetting(name);
        if (s) {
            logger::info("  Found: {} = {}", name, s->GetInt());
        } else {
            logger::warn("  NOT FOUND: {}", name);
        }
        return s;
    }

    bool ShadowBoost::cacheGameSettings()
    {
        logger::info("Caching game settings...");

        _fDirShadowDistance     = findSetting("fDirShadowDistance:Display");
        _fLODFadeOutMultObjects = findSetting("fLODFadeOutMultObjects:LOD");
        _fLODFadeOutMultItems   = findSetting("fLODFadeOutMultItems:LOD");
        _fLODFadeOutMultActors  = findSetting("fLODFadeOutMultActors:LOD");
        _fGrassStartFadeDistance = findSetting("fGrassStartFadeDistance:Grass");
        _fBlockLevel0Distance   = findSetting("fBlockLevel0Distance:TerrainManager");
        _fBlockLevel1Distance   = findSetting("fBlockLevel1Distance:TerrainManager");
        _fBlockLevel2Distance   = findSetting("fBlockLevel2Distance:TerrainManager");

        // God rays (optional)
        _grQuality = findSettingInt("iVolumetricLightingQuality:Display");
        _grGrid    = findSettingInt("iVolumetricLightingTextureGridSize:Display");
        _grScale   = findSetting("fVolumetricLightingIntensity:Display");
        _grCascade = findSettingInt("iVolumetricLightingCascadeCount:Display");

        // At minimum we need shadow distance for the plugin to be useful
        return _fDirShadowDistance != nullptr;
    }

    void ShadowBoost::saveOriginalValues()
    {
        if (_fDirShadowDistance)     o_dirShadowDist = _fDirShadowDistance->GetFloat();
        if (_fLODFadeOutMultObjects) o_lodObjects    = _fLODFadeOutMultObjects->GetFloat();
        if (_fLODFadeOutMultItems)   o_lodItems      = _fLODFadeOutMultItems->GetFloat();
        if (_fLODFadeOutMultActors)  o_lodActors     = _fLODFadeOutMultActors->GetFloat();
        if (_fGrassStartFadeDistance) o_grassDist    = _fGrassStartFadeDistance->GetFloat();
        if (_fBlockLevel0Distance)   o_blockLevel0   = _fBlockLevel0Distance->GetFloat();
        if (_fBlockLevel1Distance)   o_blockLevel1   = _fBlockLevel1Distance->GetFloat();
        if (_fBlockLevel2Distance)   o_blockLevel2   = _fBlockLevel2Distance->GetFloat();
        if (_grQuality) o_grQuality = _grQuality->GetInt();
        if (_grGrid)    o_grGrid    = _grGrid->GetInt();
        if (_grScale)   o_grScale   = _grScale->GetFloat();
        if (_grCascade) o_grCascade = _grCascade->GetInt();

        logger::info("Original values saved: shadow={:.0f}, lodObj={:.1f}, grass={:.0f}",
            o_dirShadowDist, o_lodObjects, o_grassDist);
    }

    bool ShadowBoost::init(Config* config)
    {
        _config = config;

        if (!cacheGameSettings()) {
            logger::error("Failed to cache game settings — dynamic adjustment disabled");
            return false;
        }

        saveOriginalValues();

        // Log current cascade split range (managed by version.dll proxy, not us)
        if (offsets::ShadowDist2Cascade.address()) {
            logger::info("ShadowDist2Cascade (from proxy) = {:.0f}",
                *offsets::ShadowDist2Cascade);
        }

        // Verify renderer shadow distance offset
        logger::info("ShadowDistRenderer addr=0x{:X}, value={:.0f} (Setting value={:.0f})",
            offsets::ShadowDistRenderer.address(),
            *offsets::ShadowDistRenderer,
            _fDirShadowDistance ? _fDirShadowDistance->GetFloat() : -1.0f);

        // Initialize FPS tracking
        _targetMs = Millisecond / _config->fFpsTarget;
        _lastTime = std::chrono::steady_clock::now();
        _frameCount = 0.0f;
        _blockIndex = 0;

        _initialized = true;
        logger::info("ShadowBoost initialized (target={:.0f} FPS, {:.2f} ms/frame)",
            _config->fFpsTarget, _targetMs);
        return true;
    }

    void ShadowBoost::applyGodRays()
    {
        if (!_config || !_config->bGodRaysEnable) return;

        if (_grQuality) _grQuality->SetInt(_config->iGodRaysQuality);
        if (_grGrid)    _grGrid->SetInt(_config->iGodRaysGrid);
        if (_grScale)   _grScale->SetFloat(_config->fGodRaysScale);
        if (_grCascade) _grCascade->SetInt(_config->iGodRaysCascade);

        logger::info("God rays applied: quality={}, grid={}, scale={:.2f}, cascade={}",
            _config->iGodRaysQuality, _config->iGodRaysGrid,
            _config->fGodRaysScale, _config->iGodRaysCascade);
    }

    void ShadowBoost::update(float /*deltaTime*/)
    {
        if (!_config || !_initialized) return;

        // ---- Throttle: only run every fFpsDelay frames ----
        _frameCount += 1.0f;
        if (_frameCount < _config->fFpsDelay) {
            return;
        }
        _frameCount = 0.0f;

        // ---- Calculate FPS-based adjustment (only when auto-adjust is on) ----
        float dyn = 0.0f;

        auto now = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - _lastTime);
        float avgMs = static_cast<float>(delta.count()) / Millisecond / _config->fFpsDelay;
        _lastTime = now;

        _targetMs = Millisecond / _config->fFpsTarget;

        if (_config->bAutoAdjust) {
            dyn = avgMs - _targetMs;

            // Dead zone: if barely over target, don't adjust
            if (dyn >= 0.0f && dyn <= _config->fMsTolerance) {
                dyn = 0.0f;
            }
        }

        // Periodic debug logging
        _debugCounter++;
        if (_debugCounter >= 90) {
            _debugCounter = 0;
            float curShadow = offsets::ShadowDistRenderer.address() ? *offsets::ShadowDistRenderer : -1.0f;
            float curLodObj = _fLODFadeOutMultObjects ? _fLODFadeOutMultObjects->GetFloat() : -1.0f;
            float curGrass = _fGrassStartFadeDistance ? _fGrassStartFadeDistance->GetFloat() : -1.0f;
            logger::info("SB: auto={} avg={:.2f}ms tgt={:.2f}ms dyn={:.2f} | "
                "shadow={:.0f} [{:.0f},{:.0f}] | lod={:.1f} [{:.1f},{:.1f}] | grass={:.0f} [{:.0f},{:.0f}]",
                _config->bAutoAdjust ? "ON" : "OFF", avgMs, _targetMs, dyn,
                curShadow, _config->fShadowMin, _config->fShadowMax,
                curLodObj, _config->fLodObjectsMin, _config->fLodObjectsMax,
                curGrass, _config->fGrassMin, _config->fGrassMax);
        }

        // ---- Shadow distance ----
        // Only write to renderer cache — NEVER to RE::Setting, values >3000 in INI crash VR.
        if (_config->bAutoAdjust && _config->bShadowEnable) {
            // P-controller: adjust between min and max based on FPS
            float cur = *offsets::ShadowDistRenderer;
            float adj = std::clamp(cur - dyn * _config->fShadowFactor,
                _config->fShadowMin, _config->fShadowMax);
            *offsets::ShadowDistRenderer = adj;
        } else {
            // Direct: max slider sets the shadow distance
            *offsets::ShadowDistRenderer = _config->fShadowMax;
        }

        // ---- LOD fade multipliers ----
        if (_config->bAutoAdjust && _config->bLodEnable) {
            float d = dyn * _config->fLodFactor;
            if (_fLODFadeOutMultObjects) {
                float cur = _fLODFadeOutMultObjects->GetFloat();
                _fLODFadeOutMultObjects->SetFloat(
                    std::clamp(cur - d, _config->fLodObjectsMin, _config->fLodObjectsMax));
            }
            if (_fLODFadeOutMultItems) {
                float cur = _fLODFadeOutMultItems->GetFloat();
                _fLODFadeOutMultItems->SetFloat(
                    std::clamp(cur - d, _config->fLodItemsMin, _config->fLodItemsMax));
            }
            if (_fLODFadeOutMultActors) {
                float cur = _fLODFadeOutMultActors->GetFloat();
                _fLODFadeOutMultActors->SetFloat(
                    std::clamp(cur - d, _config->fLodActorsMin, _config->fLodActorsMax));
            }
        } else {
            // Direct: max sliders set the LOD values
            if (_fLODFadeOutMultObjects) _fLODFadeOutMultObjects->SetFloat(_config->fLodObjectsMax);
            if (_fLODFadeOutMultItems)   _fLODFadeOutMultItems->SetFloat(_config->fLodItemsMax);
            if (_fLODFadeOutMultActors)  _fLODFadeOutMultActors->SetFloat(_config->fLodActorsMax);
        }

        // ---- Grass distance ----
        if (_config->bAutoAdjust && _config->bGrassEnable && _fGrassStartFadeDistance) {
            float cur = _fGrassStartFadeDistance->GetFloat();
            float adj = std::clamp(cur - dyn * _config->fGrassFactor,
                _config->fGrassMin, _config->fGrassMax);
            _fGrassStartFadeDistance->SetFloat(adj);
        } else if (_fGrassStartFadeDistance) {
            // Direct: max slider sets the grass distance
            _fGrassStartFadeDistance->SetFloat(_config->fGrassMax);
        }

        // ---- Block level (draw distance tiers) ----
        if (_config->bAutoAdjust && _config->bBlockEnable &&
            _fBlockLevel0Distance && _fBlockLevel1Distance && _fBlockLevel2Distance) {

            float shadowDist = *offsets::ShadowDistRenderer;
            if (shadowDist <= _config->fShadowMin && dyn > 0.0f) {
                _blockIndex = std::clamp(_blockIndex + 1, 0, MaxBlockLevels - 1);
            }
            if (shadowDist >= _config->fShadowMax && dyn <= 0.0f) {
                _blockIndex = std::clamp(_blockIndex - 1, 0, MaxBlockLevels - 1);
            }

            auto& bl = _config->blockLevels[_blockIndex];
            _fBlockLevel2Distance->SetFloat(bl.fLevel2);
            _fBlockLevel1Distance->SetFloat(bl.fLevel1);
            _fBlockLevel0Distance->SetFloat(bl.fLevel0);
        }
    }

} // namespace ShadowBoostF4VR
