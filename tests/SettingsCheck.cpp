// SPDX-License-Identifier: GPL-3.0-only
#include "Settings.h"
#include <LibAottg/Stats.hpp>
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
    void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
    std::string Read(const std::filesystem::path& p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f),{}}; }
}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/("LibAottg-settings-check-"+std::to_string(GetCurrentProcessId()));
    const auto path=directory/"settings.ini";
    try {
        const auto presets=LibAottg::HeroStats::RcModPresets();
        for (size_t i=0;i<Aottg::HeroIds.size();++i) {
            Aottg::Settings preset; preset.SetHeroPreset(i);
            const auto expected=presets.at(Aottg::HeroIds[i]).ToConfig();
            const auto actual=preset.CharacterConfig();
            Require(preset.HeroPreset()==int(i),"Hero preset selection mismatch");
            Require(actual.BladeCapacity==expected.BladeCapacity && actual.GasCapacity==expected.GasCapacity &&
                actual.GroundSpeed==expected.GroundSpeed && actual.WallRunSpeed==expected.WallRunSpeed &&
                actual.GroundDodgeSpeed==expected.GroundDodgeSpeed && actual.AirAcceleration==expected.AirAcceleration &&
                actual.Mass==expected.Mass,"Hero preset stat conversion mismatch");
            Require(actual.SkillId=="mikasa","Stat preset changed skill");
        }
        Aottg::Settings customStats;
        customStats.heroBlade=123; customStats.heroGas=144; customStats.heroSpeed=111; customStats.heroAcceleration=137;
        Require(customStats.HeroPreset()==-1,"Edited hero stats did not show Custom");
        const auto customConfig=customStats.CharacterConfig();
        Require(customConfig.BladeCapacity==123 && customConfig.GasCapacity==144 && customConfig.GroundSpeed==11.1f &&
            std::abs(customConfig.AirAcceleration-27.4f)<1e-5f && std::abs(customConfig.Mass-.463f)<1e-5f,"Custom hero stats not applied");
        std::string statsError;
        Require(customStats.Save(path,statsError),statsError.c_str());
        Require(Aottg::Settings::Load(path)==customStats,"Hero stats save/reload failed");
        customStats.heroBlade=0; customStats.heroGas=-1; customStats.heroSpeed=201; customStats.heroAcceleration=999;
        customStats.Sanitize();
        Require(customStats.heroBlade==1 && customStats.heroGas==1 && customStats.heroSpeed==200 && customStats.heroAcceleration==200,"Hero stat bounds failed");
        LibAottg::CharacterConfig transferConfig;
        transferConfig.SkillId="mikasa"; transferConfig.SkillCooldown=20;
        LibAottg::HeroController transfer(&transferConfig);
        LibAottg::HeroInputFrame skillInput; skillInput.Pressed=LibAottg::HeroButtons::Skill;
        transfer.Advance(.02f,skillInput);
        const float cooldown=transfer.Motor->SkillCooldownRemaining();
        Require(cooldown>0,"Transfer check did not start skill cooldown");
        auto motor=transfer.Motor;
        motor->Teleport({},0);
        motor->Gas=12.5f; motor->Blade=34.5f; motor->SpareBladePairs=2; motor->BladesEquipped=false;
        motor->SetEquipment(LibAottg::EquipmentMode::Guns);
        motor->LeftGunLoaded=false; motor->RightGunLoaded=true; motor->LeftGunReserve=3; motor->RightGunReserve=4;
        motor->Teleport({123,456,789},42); transfer.Synchronize();
        Require((motor->Position()-LibAottg::Vec3(123,456,789)).Length()<.0001f && motor->Velocity().Length()==0 && motor->FacingDegrees==42,"Transfer pose/velocity failed");
        Require(motor->Gas==12.5f && motor->Blade==34.5f && motor->SpareBladePairs==2 && !motor->BladesEquipped &&
            motor->Equipment==LibAottg::EquipmentMode::Guns && !motor->LeftGunLoaded && motor->RightGunLoaded &&
            motor->LeftGunReserve==3 && motor->RightGunReserve==4 && motor->SkillCooldownRemaining()==cooldown,"Transfer refilled resources/equipment/cooldown");
        Require(motor->LeftHookPhase()==LibAottg::HookPhase::Inactive && motor->RightHookPhase()==LibAottg::HookPhase::Inactive &&
            !motor->AttackHitboxActive && motor->State==LibAottg::CharacterState::Idle,"Transfer retained old world action/hooks");
        std::filesystem::create_directories(directory);
        { std::ofstream f(path); f<<"\xEF\xBB\xBF; User comment\n[Hero]\nToggleScanCode=67\nWorldScale=82.5\nCustomValue=keep me\n[Development]\nGameplayCheck=0\n[Camera]\nSensitivity=nan\nFieldOfView=500\n[Movement]\nGravityMultiplier=-1\n[Controls]\nAttack=99999\n"; }
        auto s=Aottg::Settings::Load(path);
        Require(s.worldScale==82.5f && s.bindings[0]==67,"Legacy decimal scale / toggle migration failed");
        Require(s.sensitivity==.15f && s.fieldOfView==100 && s.gravityMultiplier==.1f && s.bindings[size_t(Aottg::Action::Attack)]==256,"Invalid settings not sanitized");
        Require(s.bindings[size_t(Aottg::Action::Shout)]==44 && s.bindings[size_t(Aottg::Action::ReelIn)]==29,"Shout / reel defaults missing");
        { const auto legacy=directory/"legacy.ini"; std::ofstream f(legacy); f<<"[Controls]\nReelIn=44\n"; f.close();
            const auto migrated=Aottg::Settings::Load(legacy);
            Require(migrated.bindings[size_t(Aottg::Action::Shout)]==44 && migrated.bindings[size_t(Aottg::Action::ReelIn)]==20 && migrated.BindingError(59).empty(),"Legacy Z reel migration conflicts");
            f.open(legacy); f<<"[Controls]\nReelIn=44\nShout=21\n"; f.close();
            const auto custom=Aottg::Settings::Load(legacy);
            Require(custom.bindings[size_t(Aottg::Action::ReelIn)]==44 && custom.bindings[size_t(Aottg::Action::Shout)]==21,"Explicit shout/reel bindings overwritten");
        }
        s.bindings[size_t(Aottg::Action::Shout)]=22;
        s.bindings[size_t(Aottg::Action::HookLeft)]=259; s.bindings[size_t(Aottg::Action::Jump)]=21;
        s.fieldOfView=62.5f; s.cameraDistance=.85f; s.invertY=true; s.mouseWheelReel=false;
        s.speedMultiplier=1.7f; s.gasCapacityMultiplier=2; s.bladeDamage=37.25f; s.bladeHitboxMultiplier=2.5f; s.infiniteGas=true; s.spareBladePairs=0;
        Require(s.restoreDiagnostics,"Restoration diagnostics default missing");
        s.restoreDiagnostics=false;
        s.heroHud=false; s.crosshairDistance=false; s.crosshairArrows=false; s.speedometer=2; s.crosshairStyle=4;
        Require(s.BindingError(59).empty(),"Valid custom bindings rejected");
        std::string error; Require(s.Save(path,error),error.c_str());
        Require(Aottg::Settings::Load(path)==s,"Settings save/reload did not round trip");
        const auto text=Read(path);
        Require(text.find("; User comment")!=std::string::npos && text.find("CustomValue=keep me")!=std::string::npos && text.find("GameplayCheck=0")!=std::string::npos,"Save discarded unrelated configuration");
        wchar_t value[64]{}; GetPrivateProfileStringW(L"Camera",L"Distance",L"missing",value,64,path.c_str());
        Require(std::wstring(value)!=L"missing","Save created duplicate sections inaccessible to Windows INI reader");
        auto invalid=s; invalid.bladeHitboxMultiplier=std::numeric_limits<float>::quiet_NaN(); invalid.Sanitize();
        Require(invalid.bladeHitboxMultiplier==1,"Nonfinite blade hitbox size did not use default");
        invalid.bladeHitboxMultiplier=100; invalid.Sanitize();
        Require(invalid.bladeHitboxMultiplier==10,"Blade hitbox size not clamped");
        invalid.speedometer=99; invalid.crosshairStyle=-1; invalid.Sanitize();
        Require(invalid.speedometer==2 && invalid.crosshairStyle==0,"HUD mode bounds");
        auto c=s.CharacterConfig(); auto camera=s.CameraConfig();
        Require(c.WorldScale==82.5f && c.GroundSpeed==12.5f*1.7f && c.GasCapacity==150 && c.InfiniteGas && c.SpareBladePairs==0,"Hero tuning not connected to native controller configuration");
        Require(camera.InvertY && std::abs(camera.CameraDistance()-.85f)<1e-6f && camera.Sensitivity==.15f,"Camera tuning not connected");
        Require(Aottg::ButtonsFor(s,259)==LibAottg::HeroButtons::HookLeft && Aottg::ButtonsFor(s,16)==LibAottg::HeroButtons::None,"Remapping kept stale hook key");
        Require(Aottg::ButtonsFor(s,21)==LibAottg::HeroButtons::Jump && Aottg::ButtonsFor(s,257)==LibAottg::HeroButtons::Skill,"Remapping failed for alternate mouse / keyboard actions");
        s.bindings[0]=59; Require(!s.BindingError(59).empty(),"Framework menu hotkey collision allowed");
        s.bindings[0]=1; Require(!s.BindingError(59).empty(),"Escape binding allowed");
        s.bindings[0]=67;
        const auto dodgeBinding=s.bindings[size_t(Aottg::Action::Dodge)];
        s.bindings[size_t(Aottg::Action::ReelIn)]=dodgeBinding;
        Require(s.BindingError(59).empty(),"Shared dodge / reel-in binding rejected");
        const auto sharedButtons=Aottg::ButtonFor(Aottg::Action::Dodge)|Aottg::ButtonFor(Aottg::Action::ReelIn);
        Require(Aottg::ButtonsFor(s,dodgeBinding)==sharedButtons,"Shared binding did not dispatch both actions");
        Require(s.Save(path,error),error.c_str());
        const auto shared=Aottg::Settings::Load(path);
        Require(shared==s && shared.BindingError(59).empty() && Aottg::ButtonsFor(shared,dodgeBinding)==sharedButtons,"Shared bindings did not survive save/reload");
        const auto previous=Read(path); SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY);
        Require(!s.Save(path,error),"Read-only save unexpectedly succeeded");
        Require(Read(path)==previous,"Failed save damaged previous configuration");
        SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove_all(directory);
        std::cout<<"Settings check passed: hero presets/custom stats/bounds/persistence, world transfer pose/resources/equipment/cooldown/action reset, legacy migration, decimal values, invalid values, save/reload, Windows INI compatibility, preserved flags/comments, native config, remapping/shared bindings/reserved keys, failed-save preservation\n";
        return 0;
    } catch (const std::exception& e) {
        SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL); std::error_code ignored; std::filesystem::remove_all(directory,ignored);
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
