// SPDX-License-Identifier: GPL-3.0-only
#include "SettingsMenu.h"
#include "Runtime.h"
#include <SKSEMenuFramework.h>
#include <atomic>
#include <mutex>
#include <optional>
namespace ImGui = ImGuiMCP;

namespace Aottg::SettingsMenu {
    namespace {
        Settings draft, saved;
        std::atomic<size_t> selectedSkill{0};
        std::atomic<int> capture{-1}, captured{-2};
        std::atomic<bool> pending{false}, active{false};
        std::atomic<float> activeScale{70};
        bool installed=false;
        std::atomic<int> reservedKey{-1};
        std::mutex resultMutex;
        std::string resultMessage="Select Apply and save to keep your edits.";
        std::optional<Settings> resultSettings;
        // The framework owns callbacks for the lifetime of this loaded plugin.
        SKSEMenuFramework::Model::InputEvent* inputEvent=nullptr;
        SKSEMenuFramework::Model::Event* menuEvent=nullptr;

        void Result(std::string message,std::optional<Settings> value={}) {
            std::lock_guard lock(resultMutex); resultMessage=std::move(message); resultSettings=std::move(value); pending=false;
        }
        void Header() {
            std::lock_guard lock(resultMutex);
            if (resultSettings) { draft=saved=*resultSettings; resultSettings.reset(); }
            ImGui::TextWrapped("%s",resultMessage.c_str());
            ImGui::Text("HERO: %s",active?"active":"inactive");
            ImGui::SameLine(); ImGui::Text("%s",draft==saved?"No unsaved changes.":"Unsaved changes.");
            if (active && draft.worldScale!=activeScale) ImGui::TextWrapped("Current scale: %.2f. Saved scale will apply on the next character switch.",float(activeScale));
            ImGui::Separator();
            ImGui::BeginDisabled(pending);
            const auto conflict=draft.BindingError(reservedKey);
            ImGui::BeginDisabled(!conflict.empty());
            if (ImGui::Button("Apply and save")) {
                auto next=draft; next.Sanitize(); pending=true; capture=-1;
                SKSE::GetTaskInterface()->AddTask([next] {
                    std::string error;
                    if (!next.Save(SettingsPath(),error)) { Result("Save failed: "+error+". Running settings were preserved."); return; }
                    Runtime::ApplySettings(next); Result("Saved. Hero tuning applies next switch.",next);
                    SKSE::log::info("HERO settings saved and applied; scale={}; toggle={}; build={}",next.worldScale,next.bindings[0],LIBAOTTG_BUILD_ID);
                });
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button("Discard edits")) { draft=saved; capture=-1; }
            ImGui::SameLine();
            if (ImGui::Button("Restore defaults")) { draft=Settings{}; capture=-1; }
            ImGui::EndDisabled();
            if (!conflict.empty()) ImGui::TextWrapped("%s",conflict.c_str());
            ImGui::Separator();
        }
        void Options(const char* section) {
            if(std::string_view(section)=="Presentation") {
                const char* styles[]={"Default","Square","Plus","Target","Dot"};
                if(ImGui::BeginCombo("Crosshair style",styles[draft.crosshairStyle])) {
                    for(int i=0;i<5;i++) if(ImGui::Selectable(styles[i],draft.crosshairStyle==i)) draft.crosshairStyle=i;
                    ImGui::EndCombo();
                }
                const char* modes[]={"Off","Speed (u/s)","Damage estimate (K)"};
                if(ImGui::BeginCombo("Speedometer",modes[draft.speedometer])) {
                    for(int i=0;i<3;i++) if(ImGui::Selectable(modes[i],draft.speedometer==i)) draft.speedometer=i;
                    ImGui::EndCombo();
                }
            }
            for (const auto& option:FloatOptions) if (std::string_view(option.section)==section) {
                ImGui::PushID(option.key); ImGui::TextWrapped("%s",option.label); ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##value",&(draft.*option.member),option.minimum,option.maximum,"%.3f",ImGuiMCP::ImGuiSliderFlags_AlwaysClamp);
                ImGui::PopID();
            }
            for (const auto& option:BoolOptions) if (std::string_view(option.section)==section && std::string_view(option.key)!="AutoRestore") ImGui::Checkbox(option.label,&(draft.*option.member));
        }
        void CancelCapture() { capture=-1; captured=-2; }
        void __stdcall Controls() {
            Header();
            ImGui::TextWrapped("Click Bind, then press a keyboard or mouse button. Escape cancels. Multiple actions can share a key. Escape, Tab, console and the framework menu key are reserved. Gamepad HERO controls are not available.");
            ImGui::TextWrapped("Skyrim shout defaults to Z; reel in defaults to T. Equip an unlocked shout in the Magic menu, then hold the shout key to charge. Skyrim handles word selection and cooldown. Whirlwind Sprint launches HERO along the camera direction, including vertical aim; more words give a faster launch. Other movement and transformation shouts are unvalidated.");
            auto key=captured.exchange(-2); auto index=capture.load();
            if (key!=-2) { if (index>=0 && key!=-3) draft.bindings[size_t(index)]=key; capture=-1; }
            ImGui::BeginDisabled(pending);
            Options("Controls");
            for (size_t i=0;i<ActionCount;++i) {
                ImGui::PushID(int(i));
                ImGui::TextWrapped("%s",ActionNames[i]);
                if (ImGui::Button(capture==int(i)?"Press a key...":"Bind")) { captured=-2; capture=int(i); }
                ImGui::SameLine(); ImGui::SetNextItemWidth(300);
                const auto label=BindingName(draft.bindings[i]);
                if (ImGui::BeginCombo("##key",label.c_str())) {
                    if (i!=0 && ImGui::Selectable("Unbound",draft.bindings[i]<0)) draft.bindings[i]=-1;
                    // Named dropdown remains usable even when another mod consumes input events.
                    for (int code=1;code<=263;++code) {
                        if (code==reservedKey || code==1 || code==15 || code==41) continue;
                        auto name=BindingName(code); if (name.starts_with("Scan ")) continue;
                        ImGui::PushID(code);
                        if (ImGui::Selectable(name.c_str(),draft.bindings[i]==code)) draft.bindings[i]=code;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
        }
        void __stdcall Camera() {
            CancelCapture();
            Header();
            ImGui::TextWrapped("Third person mouse look with centered hook aiming. Camera changes apply live. Field of view is vertical; Skyrim's horizontal field of view is converted automatically.");
            ImGui::BeginDisabled(pending); Options("Camera"); ImGui::EndDisabled();
        }
        void __stdcall HeroSettings() {
            CancelCapture();
            Header();
            ImGui::BeginDisabled(pending);
            const auto skill=selectedSkill.load();
            if (ImGui::BeginCombo("Skill",HeroNames[skill])) {
                for (size_t i=0;i<HeroNames.size();++i) if (ImGui::Selectable(HeroNames[i],skill==i)) {
                    SKSE::GetTaskInterface()->AddTask([i] { Runtime::SelectSkill(i); });
                }
                ImGui::EndCombo();
            }
            ImGui::TextWrapped("Skill selection applies immediately, just like the switch skill key, and keeps your hero stats. The selected skill lasts for this game session.");
            ImGui::Separator();
            const auto preset=draft.HeroPreset();
            if (ImGui::BeginCombo("Hero stat preset",preset<0?"Custom":HeroNames[size_t(preset)])) {
                for (size_t i=0;i<HeroNames.size();++i) if (ImGui::Selectable(HeroNames[i],preset==int(i))) draft.SetHeroPreset(i);
                ImGui::EndCombo();
            }
            for (const auto& o:HeroStatOptions) ImGui::SliderInt(o.label,&(draft.*o.member),1,200);
            ImGui::TextWrapped("Presets set the four base AoTTG stats and keep the selected skill. Apply and save to keep stat edits; they apply on the next HERO activation. Movement and equipment multipliers also affect these stats.");
            ImGui::EndDisabled();
        }
        void __stdcall Hero() {
            CancelCapture();
            Header();
            ImGui::TextWrapped("The skill hotkey cycles Mikasa, Levi, Armin, Marco, Jean, Eren, Petra and Sasha skills while retaining the current HERO stats. Skill changes apply immediately. Scale changes hero size, movement and hook distances together; these settings apply on the next character switch.");
            ImGui::BeginDisabled(pending); Options("Hero"); Options("Movement"); Options("Hooks"); ImGui::EndDisabled();
        }
        void __stdcall Equipment() {
            CancelCapture();
            Header();
            ImGui::TextWrapped("Resource tuning applies on the next character switch. AoTTG relative-speed damage multiplier and actor hit detection apply live. Blade hitbox size scales collision reach and width only; visible swords stay the same size. Skyrim actor combat remains experimental. The current rig has blades; gun, titan and flare world effects are not available.");
            ImGui::BeginDisabled(pending); Options("Equipment");
            ImGui::SliderInt("Spare blade pairs",&draft.spareBladePairs,0,50); ImGui::EndDisabled();
        }
        void __stdcall Presentation() {
            CancelCapture();
            Header();
            ImGui::TextWrapped("Presentation changes apply immediately. The automatic restore checkbox saves immediately; other edits need Apply and save.");
            ImGui::BeginDisabled(pending);
            if (ImGui::Checkbox("Automatically restore Skyrim character",&draft.autoRestore)) {
                auto next=saved; next.autoRestore=draft.autoRestore;
                pending=true;
                SKSE::GetTaskInterface()->AddTask([next] {
                    std::string error;
                    if (!next.Save(SettingsPath(),error)) {
                        std::lock_guard lock(resultMutex);
                        draft.autoRestore=saved.autoRestore;
                        resultMessage="Automatic restore save failed: "+error;
                        pending=false; return;
                    }
                    Runtime::SetAutomaticRestore(next.autoRestore);
                    std::lock_guard lock(resultMutex);
                    saved.autoRestore=next.autoRestore;
                    draft.autoRestore=next.autoRestore;
                    resultMessage=next.autoRestore ? "Automatic restore enabled and saved." : "Automatic restore disabled and saved.";
                    pending=false;
                });
            }
            Options("Presentation");
            Options("Development");
            ImGui::TextWrapped("Restoration diagnostics retain a short history in memory and write it to AottgInSkyrim.log on automatic restoration. Apply and save to change this option.");
            ImGui::EndDisabled();
        }
        bool __stdcall CaptureInput(RE::InputEvent* event) {
            if (capture<0 || !Blocking() || !event) return false;
            auto button=event->AsButtonEvent(); if (!button || !button->IsDown()) return false;
            auto code=button->GetIDCode();
            if (button->GetDevice()==RE::INPUT_DEVICE::kKeyboard && code<256) captured=code==1?-3:int(code);
            else if (button->GetDevice()==RE::INPUT_DEVICE::kMouse && code<8) captured=256+int(code);
            else return false;
            return true;
        }
        void __stdcall MenuEvent(SKSEMenuFramework::Model::EventType type) {
            if (type==SKSEMenuFramework::Model::kOpenMenu || type==SKSEMenuFramework::Model::kCloseMenu) {
                try { reservedKey=FrameworkToggleBinding(std::filesystem::absolute(L"Data/SKSE/Plugins/SKSEMenuFramework.ini")); }
                catch (const std::exception& e) { SKSE::log::warn("Cannot refresh framework hotkey: {}",e.what()); }
            }
            if (type==SKSEMenuFramework::Model::kCloseMenu) { capture=-1; captured=-2; }
        }
    }
    void Register(const Settings& settings) {
        draft=saved=settings;
        if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule() ||
            !GetMenuFrameworkFunction<SKSEMenuFramework::Model::AddSectionItemFunction>("AddSectionItem") ||
            !GetMenuFrameworkFunction<void(*)()>("igSliderFloat")) {
            SKSE::log::warn("HERO settings menu unavailable; INI settings remain usable; build={}",LIBAOTTG_BUILD_ID); return;
        }
        installed=true;
        reservedKey=FrameworkToggleBinding(std::filesystem::absolute(L"Data/SKSE/Plugins/SKSEMenuFramework.ini"));
        SKSEMenuFramework::SetSection("AottgInSkyrim");
        SKSEMenuFramework::AddSectionItem("Controls",Controls);
        SKSEMenuFramework::AddSectionItem("Camera",Camera);
        SKSEMenuFramework::AddSectionItem("Hero settings",HeroSettings);
        SKSEMenuFramework::AddSectionItem("Hero and movement",Hero);
        SKSEMenuFramework::AddSectionItem("Equipment",Equipment);
        SKSEMenuFramework::AddSectionItem("Presentation",Presentation);
        inputEvent=SKSEMenuFramework::AddInputEvent(CaptureInput);
        menuEvent=SKSEMenuFramework::AddEvent(MenuEvent,0);
        SKSE::log::info("HERO settings menu registered; pages=6; framework={}; menuKey={}; build={}",
            SKSEMenuFramework::GetMenuFrameworkVersion(),BindingName(reservedKey),LIBAOTTG_BUILD_ID);
    }
    bool Blocking() { return installed && SKSEMenuFramework::IsAnyBlockingWindowOpened(); }
    int ReservedKey() { return reservedKey; }
    void PublishStatus(bool enabled,float scale) { active=enabled; activeScale=scale; }
    void PublishSkill(size_t index) { if (index<HeroIds.size()) selectedSkill=index; }
}
