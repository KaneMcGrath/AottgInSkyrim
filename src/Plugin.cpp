// SPDX-License-Identifier: GPL-3.0-only
#include <RE/C/ConsoleLog.h>
#include <RE/M/Misc.h>
#include <RE/S/SendHUDMessage.h>
#include <SKSE/SKSE.h>
#include <LibAottg/Stats.hpp>
#include "Runtime.h"

SKSEPluginInfo(
    .Version = REL::Version{LIBAOTTG_VERSION_MAJOR,LIBAOTTG_VERSION_MINOR,LIBAOTTG_VERSION_PATCH,0},
    .Name = "AottgInSkyrim"sv,
    .Author = "LibAottg"sv,
    .StructCompatibility = SKSE::StructCompatibility::Dependent,
    .RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility{SKSE::VersionIndependence::AddressLibrary,true}
)

namespace
{
    constexpr auto greeting = "AoTTG HERO: configure character switch and controls in the Mod Control Panel (default F1).";

    bool CheckNativeController()
    {
        // Exercise the linked C++ library inside Skyrim without modifying player/save state.
        auto config = LibAottg::HeroStats::RcModPresets().at("mikasa").ToConfig();
        config.WorldScale = 70.0f;
        LibAottg::CalibrationHeroWorld world(0.5f, 1.7f, config.WorldScale);
        LibAottg::HeroController hero(&config, &world);
        hero.Respawn({}, true);
        LibAottg::HeroInputFrame input;
        input.MoveY = 1;
        for (int tick = 0; tick < 100; ++tick) hero.Advance(0.02f, input);
        const bool passed = hero.PhysicsTicks == 100 && hero.Motor->Position().Z > 0 &&
            hero.Motor->Gas == config.GasCapacity && LibAottg::WorldBox::Finite(hero.Motor->Position());
        SKSE::log::info("Native LibAottg: controller self-check {}; ticks={}; position=({},{},{}); build={}",
            passed ? "passed" : "failed", hero.PhysicsTicks, hero.Motor->Position().X,
            hero.Motor->Position().Y, hero.Motor->Position().Z, LIBAOTTG_BUILD_ID);
        return passed;
    }

    void PrintGreeting(bool showNotification)
    {
        SKSE::GetTaskInterface()->AddTask([showNotification] {
            if (!showNotification && (!CheckNativeController() || !Aottg::Runtime::Initialize())) {
                SKSE::log::error("Native controller startup check failed; build={}", LIBAOTTG_BUILD_ID);
                return;
            }
            if (auto console = RE::ConsoleLog::GetSingleton()) {
                console->Print("%s", greeting);
                SKSE::log::info("AottgInSkyrim: game-thread task completed; Skyrim console call succeeded; build={}", LIBAOTTG_BUILD_ID);
            } else {
                SKSE::log::error("AottgInSkyrim: Skyrim console singleton unavailable; build={}", LIBAOTTG_BUILD_ID);
            }
            if (showNotification) {
                RE::SendHUDMessage::ShowHUDMessage(greeting);
                SKSE::log::info("AottgInSkyrim: HUD notification sent; build={}", LIBAOTTG_BUILD_ID);
            }
        });
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        Aottg::Runtime::OnMessage(message);
        switch (message->type) {
        case SKSE::MessagingInterface::kPostLoad:
            SKSE::log::info("SKSE PostLoad; build={}", LIBAOTTG_BUILD_ID);
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            SKSE::log::info("AottgInSkyrim: DataLoaded; build={}", LIBAOTTG_BUILD_ID);
            PrintGreeting(false);
            break;
        case SKSE::MessagingInterface::kNewGame:
            SKSE::log::info("SKSE NewGame; build={}", LIBAOTTG_BUILD_ID);
            PrintGreeting(true);
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            if (message->data) {
                SKSE::log::info("SKSE PostLoadGame; build={}", LIBAOTTG_BUILD_ID);
                PrintGreeting(true);
            }
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse, { .logPattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v" });
    SKSE::log::info("AottgInSkyrim: plugin loaded; runtime={}; SKSE={}; build={}",
        skse->RuntimeVersion().string("."), REL::Version::unpack(SKSE::GetSKSEVersion()).string("."), LIBAOTTG_BUILD_ID);

    if (skse->RuntimeVersion() < SKSE::RUNTIME_SSE_1_6_629) {
        SKSE::log::error("HERO requires modern AE runtime structures (Skyrim 1.6.629 or newer)");
        return false;
    }
    SKSE::log::info("HERO Address Library compatibility mode; tested runtime=1.7.104.0; current runtime={}; engine ABI compatibility assumed",skse->RuntimeVersion().string("."));
    if (!SKSE::GetTaskInterface() || !SKSE::GetMessagingInterface() ||
        !SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        SKSE::log::error("Required SKSE messaging/task interfaces unavailable");
        return false;
    }
    return Aottg::Runtime::Install();
}
