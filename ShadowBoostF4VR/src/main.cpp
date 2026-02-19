#include "PCH.h"

#include "ModBase.h"
#include "ConfigBase.h"
#include "ShadowBoost.h"
#include "Config.h"

#include <thread>
#include <chrono>

using namespace ShadowBoostF4VR;
using namespace f4cf;

namespace
{
    Config g_config;

    // ========================================================================
    // MCM VR Settings Reload — watches for PauseMenu close
    // ========================================================================
    class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuWatcher* GetSingleton()
        {
            static MenuWatcher instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent& a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event.opening && a_event.menuName == "PauseMenu")
            {
                logger::info("Pause menu closed, reloading MCM settings...");
                std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    g_config.loadMCMSettings();
                    logger::info("MCM settings reloaded");
                }).detach();
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ========================================================================
    // Main Mod Class
    // ========================================================================
    class ShadowBoostMod : public ModBase
    {
    public:
        ShadowBoostMod() :
            ModBase(Settings("ShadowBoostF4VR", "1.0.0", &g_config, 64, true))
        {
        }

    protected:
        void onModLoaded(const F4SE::LoadInterface* f4SE) override
        {
            logger::info("ShadowBoostF4VR loaded");
            g_config.load();
            g_config.loadMCMSettings();
        }

        void onGameLoaded() override
        {
            logger::info("Game loaded, initializing Shadow Boost...");
            g_config.loadMCMSettings();

            auto& shadowBoost = ShadowBoost::GetSingleton();
            if (!shadowBoost.init(&g_config)) {
                logger::error("Failed to initialize Shadow Boost");
                return;
            }

            shadowBoost.applyGodRays();

            // Apply shared shadow maps after game load (avoids infinite loading screen
            // that occurs when applied during early initialization by the proxy)
            SharedShadowFix::Apply();

            static bool menuWatcherRegistered = false;
            if (!menuWatcherRegistered) {
                if (auto ui = RE::UI::GetSingleton()) {
                    ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(
                        MenuWatcher::GetSingleton());
                    menuWatcherRegistered = true;
                    logger::info("MCM menu watcher registered");
                }
            }

            logger::info("ShadowBoostF4VR fully initialized");
        }

        void onGameSessionLoaded() override
        {
            g_config.loadMCMSettings();
        }

        void onFrameUpdate() override
        {
            static auto lastTime = std::chrono::high_resolution_clock::now();
            auto now = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float>(now - lastTime).count();
            lastTime = now;

            if (deltaTime < 0.001f) deltaTime = 0.001f;
            if (deltaTime > 0.1f) deltaTime = 0.1f;

            ShadowBoost::GetSingleton().update(deltaTime);
        }
    };
}

static ShadowBoostMod g_shadowBoostMod;

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_skse, F4SE::PluginInfo* a_info)
{
    g_mod = &g_shadowBoostMod;
    return g_mod->onF4SEPluginQuery(a_skse, a_info);
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    return g_mod->onF4SEPluginLoad(a_f4se);
}
