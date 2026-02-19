#pragma once

#include "ConfigBase.h"

namespace ShadowBoostF4VR
{
    constexpr int MaxBlockLevels = 4;

    struct BlockLevel {
        float fLevel2;
        float fLevel1;
        float fLevel0;
    };

    class Config : public f4cf::ConfigBase
    {
    public:
        Config() : ConfigBase("ShadowBoostF4VR",
            "Data\\F4SE\\Plugins\\ShadowBoostF4VR.ini", 0) {}

        void load() override;
        void save() override;
        void loadMCMSettings();

        // ---- Performance ----
        bool  bAutoAdjust      = false;  // master toggle for FPS-based adjustment
        float fFpsTarget       = 90.0f;
        float fFpsDelay        = 10.0f;  // frames between adjustments
        float fMsTolerance     = 0.5f;   // ms tolerance (dead zone)

        // ---- Shadow ----
        bool  bShadowEnable    = true;
        float fShadowFactor    = 30.0f;
        float fShadowMin       = 500.0f;
        float fShadowMax       = 8000.0f;

        // ---- LOD ----
        bool  bLodEnable       = true;
        float fLodFactor       = 0.1f;
        float fLodObjectsMin   = 4.5f;
        float fLodObjectsMax   = 10.0f;
        float fLodItemsMin     = 2.5f;
        float fLodItemsMax     = 8.0f;
        float fLodActorsMin    = 6.0f;
        float fLodActorsMax    = 15.0f;

        // ---- Grass ----
        bool  bGrassEnable     = true;
        float fGrassFactor     = 30.0f;
        float fGrassMin        = 3500.0f;
        float fGrassMax        = 7000.0f;

        // ---- Block Level (draw distance) ----
        bool  bBlockEnable     = false;  // disabled by default (VR pop-in)
        BlockLevel blockLevels[MaxBlockLevels] = {
            { 110000.0f, 90000.0f, 60000.0f },   // Ultra
            {  80000.0f, 60000.0f, 30000.0f },   // High
            {  80000.0f, 32000.0f, 20000.0f },   // Medium
            {  75000.0f, 25000.0f, 15000.0f },   // Low
        };

        // ---- God Rays ----
        bool         bGodRaysEnable = false;  // disabled by default (VR perf)
        std::int32_t iGodRaysQuality = 3;
        std::int32_t iGodRaysGrid    = 8;
        float        fGodRaysScale   = 0.4f;
        std::int32_t iGodRaysCascade = 1;

    protected:
        void loadIniConfigInternal(const CSimpleIniA& ini) override;
        void saveIniConfigInternal(CSimpleIniA& ini) override;

    private:
        void loadFromIni(const CSimpleIniA& ini);
    };

} // namespace ShadowBoostF4VR
