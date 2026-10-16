// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <LibAottg/Core.hpp>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace Aottg {
    enum class Action : size_t { Toggle, Forward, Back, Left, Right, Jump, Dash, Dodge, HookLeft, HookRight,
        HookBoth, Attack, Skill, SkillAlternate, CycleSkill, Reload, Supply, ReelIn, ReelOut, Interact, Shout, Count };
    constexpr size_t ActionCount=size_t(Action::Count);
    // Keyboard: DirectInput scan codes; mouse: 256 + Skyrim button index; -1: unbound.
    struct Settings {
        std::array<int,ActionCount> bindings{66,17,31,30,32,42,57,29,16,18,258,256,46,257,47,19,48,29,56,33,44};
        float worldScale=70, sensitivity=.15f, cameraDistance=.3f, cameraHeight=.64f,
            cameraBaseDistance=10, cameraCollisionRadius=.1f, fieldOfView=50, ropeThicknessMultiplier=3;
        bool invertY=false, cameraTilt=true, cameraObstruction=true, dynamicFov=true,
            mouseWheelReel=true, invertWheel=true, showRopes=true, autoRestore=true, notifications=true;
        float speedMultiplier=1, accelerationMultiplier=1, gravityMultiplier=1, jumpMultiplier=1,
            dashMultiplier=1, hookPullMultiplier=1, hookRange=10000, hookProjectileSpeed=150,
            gasCapacityMultiplier=1, gasUseMultiplier=1, bladeCapacityMultiplier=1,
            bladeHitboxMultiplier=1, bladeDamage=25, damageMultiplier=1, doubleTapWindow=.2f;
        int heroBlade=75, heroGas=75, heroSpeed=125, heroAcceleration=135;
        int spareBladePairs=5, speedometer=1, crosshairStyle=0;
        bool heroHud=true, crosshair=true, crosshairDistance=true, crosshairArrows=true;
        bool restoreDiagnostics=true;
        bool infiniteGas=false, doubleTapDash=true, autoWallJump=false, noForcedToRoof=false,
            noStopJump=false, combat=true, proceduralPose=true, damageHud=true, killInfo=true;
        LibAottg::CharacterConfig CharacterConfig() const;
        LibAottg::HeroCameraConfig CameraConfig() const;
        void SetHeroPreset(size_t index);
        int HeroPreset() const;
        void Sanitize();
        std::string BindingError(int frameworkKey) const;
        static Settings Load(const std::filesystem::path&);
        bool Save(const std::filesystem::path&,std::string& error) const;
        bool operator==(const Settings&) const = default;
    };
    struct FloatOption { const char* section; const char* key; const char* label; float Settings::* member; float minimum,maximum; };
    inline constexpr std::array<const char*,8> HeroIds={"mikasa","levi","armin","marco","jean","eren","petra","sasha"};
    inline constexpr std::array<const char*,8> HeroNames={"Mikasa","Levi","Armin","Marco","Jean","Eren","Petra","Sasha"};
    struct HeroStatOption { const char* key; const char* label; int Settings::* member; };
    inline constexpr std::array<HeroStatOption,4> HeroStatOptions={{{"Blade","Blade",&Settings::heroBlade},{"Gas","Gas",&Settings::heroGas},{"Speed","Speed",&Settings::heroSpeed},{"Acceleration","Acceleration",&Settings::heroAcceleration}}};
    struct BoolOption { const char* section; const char* key; const char* label; bool Settings::* member; };
    extern const std::vector<FloatOption> FloatOptions;
    extern const std::vector<BoolOption> BoolOptions;
    extern const std::array<const char*,ActionCount> ActionNames;
    extern const std::array<const char*,ActionCount> ActionKeys;
    LibAottg::HeroButtons ButtonFor(Action);
    LibAottg::HeroButtons ButtonsFor(const Settings&,int binding);
    bool UsesBinding(const Settings&,int binding);
    std::string BindingName(int);
    int FrameworkToggleBinding(const std::filesystem::path&);
    std::filesystem::path SettingsPath();
}
