#include "PCH.h"
#include "Config.h"

namespace ShadowBoostF4VR
{
    void Config::load()
    {
        const char* iniPath = "Data\\F4SE\\Plugins\\ShadowBoostF4VR.ini";

        CSimpleIniA ini;
        ini.SetUnicode();

        if (ini.LoadFile(iniPath) < 0) {
            logger::info("No INI file found at {}, using defaults", iniPath);
            save();
            return;
        }

        logger::info("Loaded config from {}", iniPath);
        loadFromIni(ini);
    }

    void Config::save()
    {
        const char* iniPath = "Data\\F4SE\\Plugins\\ShadowBoostF4VR.ini";

        CSimpleIniA ini;
        ini.SetUnicode();
        saveIniConfigInternal(ini);

        if (ini.SaveFile(iniPath) < 0) {
            logger::warn("Failed to save config to {}", iniPath);
        }
    }

    void Config::loadMCMSettings()
    {
        const char* mcmPath = "Data\\MCM\\Settings\\ShadowBoostF4VR.ini";

        CSimpleIniA mcmIni;
        mcmIni.SetUnicode();
        SI_Error rc = mcmIni.LoadFile(mcmPath);
        if (rc == SI_OK) {
            loadFromIni(mcmIni);
            logger::info("MCM loaded: auto={} shadow=[{:.0f},{:.0f}] f={:.0f}, lod=[{:.1f},{:.1f}] f={:.2f}, "
                "grass=[{:.0f},{:.0f}], fps={:.0f}",
                bAutoAdjust ? "ON" : "OFF",
                fShadowMin, fShadowMax, fShadowFactor,
                fLodObjectsMin, fLodObjectsMax, fLodFactor,
                fGrassMin, fGrassMax, fFpsTarget);
        } else {
            logger::info("MCM file not found (rc={}), using current config", static_cast<int>(rc));
        }
    }

    void Config::loadFromIni(const CSimpleIniA& ini)
    {
        // Performance
        bAutoAdjust   = ini.GetBoolValue("Main", "bAutoAdjust", bAutoAdjust);
        fFpsTarget    = static_cast<float>(ini.GetDoubleValue("Main", "fFpsTarget", fFpsTarget));
        fFpsDelay     = static_cast<float>(ini.GetDoubleValue("Main", "fFpsDelay", fFpsDelay));
        fMsTolerance  = static_cast<float>(ini.GetDoubleValue("Main", "fMsTolerance", fMsTolerance));

        // Shadow
        bShadowEnable = ini.GetBoolValue("Shadow", "bEnable", bShadowEnable);
        fShadowFactor = static_cast<float>(ini.GetDoubleValue("Shadow", "fDynamicValueFactor", fShadowFactor));
        fShadowMin    = static_cast<float>(ini.GetDoubleValue("Shadow", "fMinDistance", fShadowMin));
        fShadowMax    = static_cast<float>(ini.GetDoubleValue("Shadow", "fMaxDistance", fShadowMax));

        // LOD
        bLodEnable     = ini.GetBoolValue("Lod", "bEnable", bLodEnable);
        fLodFactor     = static_cast<float>(ini.GetDoubleValue("Lod", "fDynamicValueFactor", fLodFactor));
        fLodObjectsMin = static_cast<float>(ini.GetDoubleValue("Lod", "fLODFadeOutMultObjectsMin", fLodObjectsMin));
        fLodObjectsMax = static_cast<float>(ini.GetDoubleValue("Lod", "fLODFadeOutMultObjectsMax", fLodObjectsMax));
        fLodItemsMin   = static_cast<float>(ini.GetDoubleValue("Lod", "fLODFadeOutMultItemsMin", fLodItemsMin));
        fLodItemsMax   = static_cast<float>(ini.GetDoubleValue("Lod", "fLODFadeOutMultItemsMax", fLodItemsMax));
        fLodActorsMin  = static_cast<float>(ini.GetDoubleValue("Lod", "fLODFadeOutMultActorsMin", fLodActorsMin));
        fLodActorsMax  = static_cast<float>(ini.GetDoubleValue("Lod", "fLODFadeOutMultActorsMax", fLodActorsMax));

        // Grass
        bGrassEnable = ini.GetBoolValue("Grass", "bEnable", bGrassEnable);
        fGrassFactor = static_cast<float>(ini.GetDoubleValue("Grass", "fDynamicValueFactor", fGrassFactor));
        fGrassMin    = static_cast<float>(ini.GetDoubleValue("Grass", "fGrassStartFadeDistanceMin", fGrassMin));
        fGrassMax    = static_cast<float>(ini.GetDoubleValue("Grass", "fGrassStartFadeDistanceMax", fGrassMax));

        // Block levels
        bBlockEnable = ini.GetBoolValue("TerrainManager", "bEnable", bBlockEnable);
        const char* blSections[] = { "TerrainManager", "TerrainManager:Level1", "TerrainManager:Level2", "TerrainManager:Level3" };
        for (int i = 0; i < MaxBlockLevels; i++) {
            blockLevels[i].fLevel2 = static_cast<float>(ini.GetDoubleValue(blSections[i], "fBlockLevel2Distance", blockLevels[i].fLevel2));
            blockLevels[i].fLevel1 = static_cast<float>(ini.GetDoubleValue(blSections[i], "fBlockLevel1Distance", blockLevels[i].fLevel1));
            blockLevels[i].fLevel0 = static_cast<float>(ini.GetDoubleValue(blSections[i], "fBlockLevel0Distance", blockLevels[i].fLevel0));
        }

        // God Rays
        bGodRaysEnable = ini.GetBoolValue("GodRays", "bEnable", bGodRaysEnable);
        iGodRaysQuality = static_cast<std::int32_t>(ini.GetLongValue("GodRays", "iQuality", iGodRaysQuality));
        iGodRaysGrid    = static_cast<std::int32_t>(ini.GetLongValue("GodRays", "iGrid", iGodRaysGrid));
        fGodRaysScale   = static_cast<float>(ini.GetDoubleValue("GodRays", "fScale", fGodRaysScale));
        iGodRaysCascade = static_cast<std::int32_t>(ini.GetLongValue("GodRays", "iCascade", iGodRaysCascade));
    }

    void Config::loadIniConfigInternal(const CSimpleIniA& ini)
    {
        loadFromIni(ini);
    }

    void Config::saveIniConfigInternal(CSimpleIniA& ini)
    {
        // Performance
        ini.SetBoolValue("Main", "bAutoAdjust", bAutoAdjust);
        ini.SetDoubleValue("Main", "fFpsTarget", fFpsTarget);
        ini.SetDoubleValue("Main", "fFpsDelay", fFpsDelay);
        ini.SetDoubleValue("Main", "fMsTolerance", fMsTolerance);

        // Shadow
        ini.SetBoolValue("Shadow", "bEnable", bShadowEnable);
        ini.SetDoubleValue("Shadow", "fDynamicValueFactor", fShadowFactor);
        ini.SetDoubleValue("Shadow", "fMinDistance", fShadowMin);
        ini.SetDoubleValue("Shadow", "fMaxDistance", fShadowMax);

        // LOD
        ini.SetBoolValue("Lod", "bEnable", bLodEnable);
        ini.SetDoubleValue("Lod", "fDynamicValueFactor", fLodFactor);
        ini.SetDoubleValue("Lod", "fLODFadeOutMultObjectsMin", fLodObjectsMin);
        ini.SetDoubleValue("Lod", "fLODFadeOutMultObjectsMax", fLodObjectsMax);
        ini.SetDoubleValue("Lod", "fLODFadeOutMultItemsMin", fLodItemsMin);
        ini.SetDoubleValue("Lod", "fLODFadeOutMultItemsMax", fLodItemsMax);
        ini.SetDoubleValue("Lod", "fLODFadeOutMultActorsMin", fLodActorsMin);
        ini.SetDoubleValue("Lod", "fLODFadeOutMultActorsMax", fLodActorsMax);

        // Grass
        ini.SetBoolValue("Grass", "bEnable", bGrassEnable);
        ini.SetDoubleValue("Grass", "fDynamicValueFactor", fGrassFactor);
        ini.SetDoubleValue("Grass", "fGrassStartFadeDistanceMin", fGrassMin);
        ini.SetDoubleValue("Grass", "fGrassStartFadeDistanceMax", fGrassMax);

        // Block levels
        ini.SetBoolValue("TerrainManager", "bEnable", bBlockEnable);
        const char* blSections[] = { "TerrainManager", "TerrainManager:Level1", "TerrainManager:Level2", "TerrainManager:Level3" };
        for (int i = 0; i < MaxBlockLevels; i++) {
            ini.SetDoubleValue(blSections[i], "fBlockLevel2Distance", blockLevels[i].fLevel2);
            ini.SetDoubleValue(blSections[i], "fBlockLevel1Distance", blockLevels[i].fLevel1);
            ini.SetDoubleValue(blSections[i], "fBlockLevel0Distance", blockLevels[i].fLevel0);
        }

        // God Rays
        ini.SetBoolValue("GodRays", "bEnable", bGodRaysEnable);
        ini.SetLongValue("GodRays", "iQuality", iGodRaysQuality);
        ini.SetLongValue("GodRays", "iGrid", iGodRaysGrid);
        ini.SetDoubleValue("GodRays", "fScale", fGodRaysScale);
        ini.SetLongValue("GodRays", "iCascade", iGodRaysCascade);
    }

} // namespace ShadowBoostF4VR
