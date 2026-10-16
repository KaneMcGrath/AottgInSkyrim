// SPDX-License-Identifier: GPL-3.0-only
#include "Settings.h"
#include <LibAottg/Stats.hpp>
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace Aottg {
    const std::array<const char*,ActionCount> ActionNames={"Switch character","Move forward","Move backward","Move left","Move right",
        "Jump / gas","Dash","Dodge","Left hook","Right hook","Both hooks","Blade attack","Skill","Skill (alternate)","Switch skill",
        "Reload blades","Supply","Reel in","Reel out","Interact with Skyrim target","Skyrim shout (hold to charge)"};
    const std::array<const char*,ActionCount> ActionKeys={"Toggle","Forward","Back","Left","Right","Jump","Dash","Dodge",
        "HookLeft","HookRight","HookBoth","Attack","Skill","SkillAlternate","CycleSkill","Reload","Supply","ReelIn","ReelOut","Interact","Shout"};
    const std::vector<FloatOption> FloatOptions={
        {"Hero","WorldScale","World units per AoTTG unit",&Settings::worldScale,10,200},
        {"Camera","Sensitivity","Mouse sensitivity (degrees / pixel)",&Settings::sensitivity,.01f,1},
        {"Camera","Distance","Camera distance",&Settings::cameraDistance,0,1},
        {"Camera","Height","Camera height multiplier",&Settings::cameraHeight,.1f,2},
        {"Camera","BaseDistance","Base follow distance (AoTTG units)",&Settings::cameraBaseDistance,1,30},
        {"Camera","CollisionRadius","Obstruction probe radius (AoTTG units)",&Settings::cameraCollisionRadius,.01f,1},
        {"Camera","FieldOfView","Base vertical field of view (degrees)",&Settings::fieldOfView,30,100},
        {"Movement","SpeedMultiplier","Ground / wall / dodge speed multiplier",&Settings::speedMultiplier,.1f,5},
        {"Movement","AccelerationMultiplier","Air acceleration multiplier",&Settings::accelerationMultiplier,.1f,5},
        {"Movement","GravityMultiplier","Gravity multiplier",&Settings::gravityMultiplier,.1f,5},
        {"Movement","JumpMultiplier","Jump / roof vault speed multiplier",&Settings::jumpMultiplier,.1f,5},
        {"Movement","DashMultiplier","Air dash speed multiplier",&Settings::dashMultiplier,.1f,5},
        {"Movement","DoubleTapWindow","Double tap interval (seconds)",&Settings::doubleTapWindow,.05f,.5f},
        {"Hooks","PullMultiplier","Hook pull acceleration multiplier",&Settings::hookPullMultiplier,.1f,5},
        {"Hooks","Range","Maximum hook range (AoTTG units)",&Settings::hookRange,10,10000},
        {"Hooks","ProjectileSpeed","Hook projectile speed (AoTTG units / second)",&Settings::hookProjectileSpeed,10,500},
        {"Equipment","GasCapacityMultiplier","Gas capacity multiplier",&Settings::gasCapacityMultiplier,.1f,10},
        {"Equipment","GasUseMultiplier","Gas consumption multiplier",&Settings::gasUseMultiplier,0,10},
        {"Equipment","BladeCapacityMultiplier","Blade durability multiplier",&Settings::bladeCapacityMultiplier,.1f,10},
        {"Equipment","BladeHitboxMultiplier","Blade hitbox size multiplier",&Settings::bladeHitboxMultiplier,.1f,10},
        {"Equipment","DamageMultiplier","AoTTG damage multiplier",&Settings::damageMultiplier,0.01f,10},
        {"Presentation","RopeThicknessMultiplier","Hook rope thickness multiplier (3 = three times the original width)",&Settings::ropeThicknessMultiplier,.5f,10}
    };
    const std::vector<BoolOption> BoolOptions={
        {"Development","RestoreDiagnostics","Log automatic restoration diagnostics",&Settings::restoreDiagnostics},
        {"Presentation","HeroHud","AoTTG special, gas and blade HUD",&Settings::heroHud},
        {"Presentation","Crosshair","AoTTG crosshair",&Settings::crosshair},
        {"Presentation","CrosshairDistance","Crosshair distance",&Settings::crosshairDistance},
        {"Presentation","CrosshairArrows","Hook direction arrows",&Settings::crosshairArrows},
        {"Camera","InvertY","Invert vertical mouse look",&Settings::invertY},
        {"Camera","Tilt","Camera tilt while hooking",&Settings::cameraTilt},
        {"Camera","Obstruction","Camera obstruction checks",&Settings::cameraObstruction},
        {"Camera","DynamicFov","Increase field of view with speed",&Settings::dynamicFov},
        {"Controls","MouseWheelReel","Use mouse wheel to reel",&Settings::mouseWheelReel},
        {"Controls","InvertWheel","Invert reel wheel direction",&Settings::invertWheel},
        {"Movement","DoubleTapDash","Double tap movement to dash",&Settings::doubleTapDash},
        {"Movement","AutoWallJump","Wall jump while dodge is held",&Settings::autoWallJump},
        {"Movement","NoForcedToRoof","Disable automatic roof vault",&Settings::noForcedToRoof},
        {"Movement","NoStopJump","Keep running state when jumping",&Settings::noStopJump},
        {"Equipment","InfiniteGas","Infinite gas",&Settings::infiniteGas},
        {"Equipment","Combat","Enable blade hits on Skyrim actors",&Settings::combat},
        {"Presentation","DamageHud","AoTTG score and Stylish HUD",&Settings::damageHud},
        {"Presentation","KillInfo","AoTTG KillInfo feed",&Settings::killInfo},
        {"Presentation","ShowRopes","Draw hook ropes",&Settings::showRopes},
        {"Presentation","AutoRestore","Automatically restore Skyrim character",&Settings::autoRestore},
        {"Presentation","Notifications","Character switch notifications",&Settings::notifications},
        {"Presentation","ProceduralPose","Procedural body tilt",&Settings::proceduralPose}
    };
    namespace {
        std::string Trim(std::string s) {
            auto a=s.find_first_not_of(" \t\r\n"); if (a==std::string::npos) return {};
            return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
        }
        std::string Lower(std::string s) { for (auto& c:s) c=char(std::tolower(static_cast<unsigned char>(c))); return s; }
        using Values=std::map<std::pair<std::string,std::string>,std::string>;
        std::string ReadText(const std::filesystem::path& path) {
            std::ifstream file(path,std::ios::binary);
            if (!file && std::filesystem::exists(path)) throw std::runtime_error("Cannot read settings file");
            std::string text{std::istreambuf_iterator<char>(file),{}};
            if (text.starts_with("\xEF\xBB\xBF")) text.erase(0,3);
            return text;
        }
        bool Section(const std::string& line,std::string& section) {
            auto s=Trim(line); if (s.empty() || s.front()!='[' || s.back()!=']') return false;
            section=Lower(Trim(s.substr(1,s.size()-2))); return true;
        }
        bool Entry(const std::string& line,std::string& key,std::string& value) {
            auto s=Trim(line); if (s.empty() || s.front()==';' || s.front()=='#') return false;
            auto eq=s.find('='); if (eq==std::string::npos) return false;
            key=Lower(Trim(s.substr(0,eq))); value=Trim(s.substr(eq+1)); return !key.empty();
        }
        Values Parse(const std::string& text) {
            Values v; std::string section,line,key,value; std::istringstream in(text);
            while (std::getline(in,line)) if (!Section(line,section) && Entry(line,key,value)) v[{section,key}]=value;
            return v;
        }
        template<class T> T Number(const Values& v,const char* section,const char* key,T fallback) {
            auto i=v.find({Lower(section),Lower(key)}); if (i==v.end()) return fallback;
            T n{}; const auto& s=i->second; auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),n);
            return ec==std::errc{} && end==s.data()+s.size() ? n : fallback;
        }
    }
    std::filesystem::path SettingsPath() { return std::filesystem::absolute(L"Data/SKSE/Plugins/AottgInSkyrim.ini"); }
    void Settings::SetHeroPreset(size_t index) {
        const auto p=LibAottg::HeroStats::RcModPresets().at(HeroIds.at(index));
        heroBlade=p.Blade; heroGas=p.Gas; heroSpeed=p.Speed; heroAcceleration=p.Acceleration;
    }
    int Settings::HeroPreset() const {
        const auto presets=LibAottg::HeroStats::RcModPresets();
        for (size_t i=0;i<HeroIds.size();++i) {
            const auto& p=presets.at(HeroIds[i]);
            if (heroBlade==p.Blade && heroGas==p.Gas && heroSpeed==p.Speed && heroAcceleration==p.Acceleration) return int(i);
        }
        return -1;
    }
    void Settings::Sanitize() {
        const Settings defaults;
        for (const auto& o:HeroStatOptions) this->*o.member=std::clamp(this->*o.member,1,200);
        for (const auto& o:FloatOptions) { auto& x=this->*o.member; x=std::isfinite(x)?std::clamp(x,o.minimum,o.maximum):defaults.*o.member; }
        spareBladePairs=std::clamp(spareBladePairs,0,50); speedometer=std::clamp(speedometer,0,2); crosshairStyle=std::clamp(crosshairStyle,0,4);
        for (size_t i=0;i<bindings.size();++i) if (!(bindings[i]==-1 || (bindings[i]>0 && bindings[i]<256) || (bindings[i]>=256 && bindings[i]<=263))) bindings[i]=defaults.bindings[i];
        if (bindings[0]<0) bindings[0]=defaults.bindings[0];
    }
    Settings Settings::Load(const std::filesystem::path& path) {
        auto v=Parse(ReadText(path)); Settings s;
        s.bindings[0]=Number(v,"Hero","ToggleScanCode",s.bindings[0]); // Legacy INI migration.
        for (size_t i=0;i<ActionCount;++i) s.bindings[i]=Number(v,"Controls",ActionKeys[i],s.bindings[i]);
        // Migrate the former default Z reel binding only when adding shout to an old INI.
        // Keep explicit shout bindings and all other custom controls intact.
        if (!v.contains({"controls","shout"}) && s.bindings[size_t(Action::ReelIn)]==44) {
            s.bindings[size_t(Action::ReelIn)]=-1;
            if (!UsesBinding(s,20)) s.bindings[size_t(Action::ReelIn)]=20;
        }
        for (const auto& o:HeroStatOptions) s.*o.member=Number(v,"HeroStats",o.key,s.*o.member);
        for (const auto& o:FloatOptions) s.*o.member=Number(v,o.section,o.key,s.*o.member);
        for (const auto& o:BoolOptions) {
            auto i=v.find({Lower(o.section),Lower(o.key)}); if (i==v.end()) continue;
            auto value=Lower(i->second); if (value=="true" || value=="1") s.*o.member=true;
            else if (value=="false" || value=="0") s.*o.member=false;
        }
        // Preserve the legacy fixed-damage preference for configuration round trips.
        // Combat uses damageMultiplier and the library calculation.
        s.bladeDamage=Number(v,"Equipment","BladeDamage",s.bladeDamage);
        s.crosshairStyle=Number(v,"Presentation","CrosshairStyle",s.crosshairStyle);
        s.speedometer=Number(v,"Presentation","Speedometer",s.speedometer);
        s.spareBladePairs=Number(v,"Equipment","SpareBladePairs",s.spareBladePairs); s.Sanitize(); return s;
    }
    bool Settings::Save(const std::filesystem::path& path,std::string& error) const {
        std::filesystem::path temporary=path;
        temporary+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
        try {
            Values values;
            for (const auto& o:HeroStatOptions) values[{"herostats",Lower(o.key)}]=std::to_string(this->*o.member);
            for (size_t i=0;i<ActionCount;++i) values[{"controls",Lower(ActionKeys[i])}]=std::to_string(bindings[i]);
            for (const auto& o:FloatOptions) { std::ostringstream n; n.precision(9); n<<this->*o.member; values[{Lower(o.section),Lower(o.key)}]=n.str(); }
            for (const auto& o:BoolOptions) values[{Lower(o.section),Lower(o.key)}]=this->*o.member?"1":"0";
            values[{"equipment","bladedamage"}]=std::to_string(bladeDamage);
            values[{"presentation","crosshairstyle"}]=std::to_string(crosshairStyle);
            values[{"presentation","speedometer"}]=std::to_string(speedometer);
            values[{"equipment","sparebladepairs"}]=std::to_string(spareBladePairs);
            // Preserve comments, development flags and unknown entries. Replace every duplicate
            // known entry too, so another INI reader cannot resurrect an earlier value.
            std::istringstream in(ReadText(path)); std::ostringstream out; std::string line,section,key,value;
            std::map<std::pair<std::string,std::string>,bool> written;
            auto finishSection=[&] {
                for (const auto& [id,val]:values) if (id.first==section && !written.contains(id)) {
                    out<<id.second<<'='<<val<<"\r\n"; written[id]=true;
                }
            };
            while (std::getline(in,line)) {
                if (!line.empty() && line.back()=='\r') line.pop_back();
                auto nextSection=section;
                if (Section(line,nextSection)) { finishSection(); section=nextSection; }
                else if (Entry(line,key,value)) {
                    if (section=="hero" && key=="togglescancode") line="ToggleScanCode="+std::to_string(bindings[0]);
                    auto i=values.find({section,key}); if (i!=values.end()) { line=line.substr(0,line.find('=')+1)+i->second; written[i->first]=true; }
                }
                out<<line<<"\r\n";
            }
            finishSection();
            // Append only sections that did not already exist.
            std::string last;
            for (const auto& [id,val]:values) if (!written.contains(id)) {
                if (id.first!=last) { out<<"\r\n["<<id.first<<"]\r\n"; last=id.first; }
                out<<id.second<<'='<<val<<"\r\n";
            }
            std::filesystem::create_directories(path.parent_path());
            { std::ofstream file(temporary,std::ios::binary|std::ios::trunc); file<<out.str(); file.flush(); if (!file) throw std::runtime_error("Cannot write settings temporary file"); }
            if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace settings file (Windows error "+std::to_string(GetLastError())+")");
            return true;
        } catch (const std::exception& e) { error=e.what(); std::error_code ignored; std::filesystem::remove(temporary,ignored); return false; }
    }
    std::string Settings::BindingError(int frameworkKey) const {
        if (bindings[0]<0) return "Switch character must have a binding.";
        for (size_t i=0;i<ActionCount;++i) {
            auto b=bindings[i]; if (b<0) continue;
            if (b==1 || b==15 || b==41 || b==frameworkKey) return std::string(ActionNames[i])+": Escape, Tab, console and the framework menu key are reserved.";
        }
        return {};
    }
    LibAottg::CharacterConfig Settings::CharacterConfig() const {
        auto c=LibAottg::HeroStats("Custom",heroSpeed,heroGas,heroBlade,heroAcceleration,"mikasa").ToConfig(); c.WorldScale=worldScale;
        // Skyrim has open sky and distant scenery without collision. Always use improved hook aiming.
        c.ImprovedRaycasts=true;
        c.GroundSpeed*=speedMultiplier; c.WallRunSpeed*=speedMultiplier; c.GroundDodgeSpeed*=speedMultiplier;
        c.AirAcceleration*=accelerationMultiplier; c.Gravity*=gravityMultiplier; c.HookedGravity*=gravityMultiplier;
        c.JumpSpeed*=jumpMultiplier; c.RoofVaultSpeed*=jumpMultiplier; c.DashSpeed*=dashMultiplier;
        c.HookPullAcceleration*=hookPullMultiplier; c.HookRange=hookRange; c.HookProjectileSpeed=hookProjectileSpeed;
        c.GasCapacity*=gasCapacityMultiplier; c.GasUsePerSecond*=gasUseMultiplier; c.HookGasUsePerSecond*=gasUseMultiplier;
        c.HookLaunchGasCost*=gasUseMultiplier; c.DashGasFraction*=gasUseMultiplier;
        c.BladeCapacity*=bladeCapacityMultiplier; c.SpareBladePairs=spareBladePairs; c.InfiniteGas=infiniteGas;
        c.DoubleTapDash=doubleTapDash; c.DoubleTapWindow=doubleTapWindow; c.AutoWallJump=autoWallJump;
        c.NoForcedToRoof=noForcedToRoof; c.NoStopJump=noStopJump; c.Validate(); return c;
    }
    LibAottg::HeroCameraConfig Settings::CameraConfig() const {
        LibAottg::HeroCameraConfig c; c.Sensitivity=sensitivity; c.SetCameraDistance(cameraDistance); c.HeightMultiplier=cameraHeight;
        c.BaseDistance=cameraBaseDistance; c.CollisionRadius=cameraCollisionRadius; c.InvertY=invertY; c.Tilt=cameraTilt;
        c.Obstruction=cameraObstruction; c.Validate(); return c;
    }
    LibAottg::HeroButtons ButtonFor(Action action) {
        using B=LibAottg::HeroButtons;
        switch (action) {
            case Action::Jump:return B::Jump; case Action::Dash:return B::Dash; case Action::Dodge:return B::Dodge;
            case Action::HookLeft:return B::HookLeft; case Action::HookRight:return B::HookRight; case Action::HookBoth:return B::HookBoth;
            case Action::Attack:return B::Attack; case Action::Skill:case Action::SkillAlternate:return B::Skill;
            case Action::Reload:return B::Reload; case Action::Supply:return B::Supply; default:return B::None;
        }
    }
    LibAottg::HeroButtons ButtonsFor(const Settings& s,int binding) {
        auto result=LibAottg::HeroButtons::None;
        for (size_t i=0;i<ActionCount;++i) if (binding>=0 && s.bindings[i]==binding) result|=ButtonFor(Action(i));
        return result;
    }
    bool UsesBinding(const Settings& s,int b) { return b>=0 && std::ranges::find(s.bindings,b)!=s.bindings.end(); }
    std::string BindingName(int code) {
        if (code<0) return "Unbound";
        if (code>=256) { static const char* names[]={"Mouse left","Mouse right","Mouse middle","Mouse 4","Mouse 5","Mouse 6","Mouse 7","Mouse 8"}; return code<=263?names[code-256]:"Unknown"; }
        char name[128]{}; LONG scan=LONG(code&127)<<16; if (code>=128) scan|=1L<<24;
        if (GetKeyNameTextA(scan,name,sizeof(name))) return name;
        return "Scan "+std::to_string(code);
    }
    int FrameworkToggleBinding(const std::filesystem::path& path) {
        auto values=Parse(ReadText(path)); auto i=values.find({"general","togglekey"}); auto name=Lower(i==values.end()?"F1":i->second);
        if (auto mode=values.find({"general","togglemode"}); mode!=values.end() && Lower(mode->second)=="off") return -1;
        for (int code=1;code<256;++code) if (Lower(BindingName(code))==name) return code;
        // Framework uses canonical names for modifiers / whitespace / punctuation.
        static const std::map<std::string,int> aliases={{"spacebar",57},{"leftcontrol",29},{"leftshift",42},{"leftalt",56},
            {"rightcontrol",157},{"rightshift",54},{"rightalt",184},{"tilde",41},{"escape",1},{"none",-1},
            {"minus",12},{"equals",13},{"bracketleft",26},{"bracketright",27},{"semicolon",39},{"apostrophe",40},
            {"backslash",43},{"comma",51},{"period",52},{"slash",53},{"capslock",58},{"numlock",69},{"scrolllock",70},
            {"kp_multiply",55},{"kp_7",71},{"kp_8",72},{"kp_9",73},{"kp_subtract",74},{"kp_4",75},{"kp_5",76},
            {"kp_6",77},{"kp_plus",78},{"kp_1",79},{"kp_2",80},{"kp_3",81},{"kp_0",82},{"kp_decimal",83},
            {"kp_enter",156},{"kp_divide",181},{"printscreen",183},{"pageup",201},{"pagedown",209},{"leftwin",219},{"rightwin",220}};
        auto alias=aliases.find(name); return alias==aliases.end()?-1:alias->second;
    }
}
