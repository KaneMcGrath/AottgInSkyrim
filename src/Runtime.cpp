// SPDX-License-Identifier: GPL-3.0-only
#include "Runtime.h"
#include "Character.h"
#include "SkyrimWorld.h"
#include "Render.h"
#include "CallDiscovery.h"
#include "SettingsMenu.h"
#include <LibAottg/Stats.hpp>
#include <Windows.h>
#include <chrono>
#include <deque>
#include <sstream>
#include <optional>
#include <limits>
#include <thread>

namespace Aottg::Runtime {
    namespace {
        Character character;
        SkyrimWorld world;
        LibAottg::HeroDamageHud damageHud;
        LibAottg::HeroKillInfo killInfo;
        std::unique_ptr<LibAottg::HeroController> hero;
        std::unique_ptr<LibAottg::HeroCamera> camera;
        RE::BSTSmartPointer<RE::TESCameraState> savedCamera;
        std::vector<RE::NiPointer<RE::BSGeometry>> hidden;
        RE::NiPointer<RE::NiAVObject> playerRoot;
        RE::FormID cellId=0;
        RE::FormID worldspaceId=0;
        bool interior=false;
        RE::NiPoint3 lastPosition{};
        RE::bhkCharacterController* savedController=nullptr;
        float savedGravity=0,savedFov=0;
        bool savedAutoVanity=true;
        RE::Setting* vanillaCrosshair=nullptr;
        bool savedCrosshair=false;
        std::optional<bool> appliedCrosshair;
        bool suspended=false, menuPaused=false;
        unsigned worldTransfers=0;
        RE::FormID expectedDoorCell=0;
        float configuredScale=70;
        Settings settings;
        bool loaded=false, toggle=false, cycleSkill=false, activateTarget=false;
        bool shoutHeld=false;
        std::optional<LibAottg::Vec3> whirlwindLaunch;
        constexpr auto skillIds=HeroIds;
        size_t currentSkill=0;
        LibAottg::HeroInputFrame pending{};
        std::array<bool,256> keys{};
        std::array<bool,10> mouse{};
        LibAottg::HeroRope ropes[2];
        LibAottg::HeroSmokeTrail smoke;
        std::array<RE::BSSoundHandle,16> heroSounds;
        std::array<const char*,16> soundPaths{};
        std::array<bool,16> soundReported{};
        size_t nextSound=0;
        float clock=0;
        std::deque<std::string> restoreHistory;
        std::chrono::steady_clock::time_point nextRestoreSample{};
        int lastDiagnosticCamera=-2;
        float lastUpdateDt=0;
        const char* updateStage="inactive";
        bool Menu();
        bool Focus();
        bool Eligible(RE::PlayerCharacter* p);
        void SuppressVanity() {
            if (!hero || suspended) return;
            if (auto pc=RE::PlayerCamera::GetSingleton()) {
                pc->GetRuntimeData2().allowAutoVanityMode=false;
                // Recover only the idle orbit camera if Skyrim or another mod entered it.
                // Furniture, kill moves, loading and other unsupported states retain their guards.
                if (pc->currentState && pc->currentState->id==RE::CameraStates::kAutoVanity) {
                    SKSE::log::info("HERO prevented auto-vanity camera; returning to third-person; build={}",LIBAOTTG_BUILD_ID);
                    pc->ForceThirdPerson();
                }
            }
        }
        int CameraId() {
            auto pc=RE::PlayerCamera::GetSingleton();
            return pc && pc->currentState ? int(pc->currentState->id) : -1;
        }
        const char* CameraName(int id) {
            constexpr const char* names[]={"first-person","auto-vanity","VATS","free","iron-sights","furniture",
                "PC-transition","tween","animated","third-person","mount","bleedout","dragon"};
            return id>=0 && id<13 ? names[id] : "missing/unknown";
        }
        std::string RestoreSnapshot() {
            auto p=RE::PlayerCharacter::GetSingleton();
            auto cell=p ? p->GetParentCell() : nullptr;
            auto ws=cell ? cell->GetRuntimeData().worldSpace : nullptr;
            const int id=CameraId();
            std::ostringstream out;
            out<<"heroTime="<<clock<<"; dt="<<lastUpdateDt<<"; stage="<<updateStage
                <<"; camera="<<id<<" ("<<CameraName(id)<<"); menu="<<Menu()<<"; focus="<<Focus()
                <<"; allowAutoVanity="<<(RE::PlayerCamera::GetSingleton() ? RE::PlayerCamera::GetSingleton()->GetRuntimeData2().allowAutoVanityMode : false)
                <<"; rendererReady="<<Render::Ready()<<"; cell="<<(cell ? cell->GetFormID() : 0)
                <<"; savedCell="<<cellId<<"; world="<<(ws ? ws->GetFormID() : 0)<<"; savedWorld="<<worldspaceId;
            if (p) {
                auto pos=p->GetPosition();
                out<<"; player=("<<pos.x<<","<<pos.y<<","<<pos.z<<"); displacement="<<pos.GetDistance(lastPosition)
                    <<"; dead="<<p->IsDead()<<"; mounted="<<p->IsOnMount()<<"; killMove="<<p->IsInKillMove()
                    <<"; sitSleep="<<int(p->GetSitSleepState())<<"; rootChanged="<<(p->Get3D(false)!=playerRoot.get())
                    <<"; controllerChanged="<<(p->GetCharController()!=savedController);
            } else out<<"; player=missing";
            if (hero) {
                auto pos=hero->Motor->Position(), v=hero->Motor->Velocity();
                out<<"; hero=("<<pos.X<<","<<pos.Y<<","<<pos.Z<<"); velocity=("<<v.X<<","<<v.Y<<","<<v.Z
                    <<"); grounded="<<hero->Motor->Grounded<<"; motorState="<<int(hero->Motor->State)
                    <<"; hooks="<<int(hero->Motor->LeftHookPhase())<<","<<int(hero->Motor->RightHookPhase());
            }
            return out.str();
        }
        void SampleRestoration() {
            if (!settings.restoreDiagnostics) return;
            auto now=std::chrono::steady_clock::now(); const int id=CameraId();
            if (now<nextRestoreSample && id==lastDiagnosticCamera) return;
            nextRestoreSample=now+std::chrono::milliseconds(250); lastDiagnosticCamera=id;
            if (restoreHistory.size()==16) restoreHistory.pop_front();
            restoreHistory.push_back(RestoreSnapshot());
        }
        bool testVanity=false, testVanityAttempted=false;
        bool selfTest=false, hudComparison=false, testStarted=false, testFinished=false, testWorldRequested=false;
        float testClock=0, settleClock=0;
        unsigned testStage=0;
        unsigned testShoutStage=0;
        bool testShoutCast=false, testShoutProjectile=false;
        unsigned testWhirlwindStage=0, testWhirlwindLaunches=0;
        float testWhirlwindClock=0;
        LibAottg::Vec3 testWhirlwindStart{}, testWhirlwindDirection{};
        void TestShoutInput(float value,float duration);
        class ShoutSink final : public RE::BSTEventSink<RE::TESSpellCastEvent> {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* event,RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
                if (hero && event && event->object.get()==RE::PlayerCharacter::GetSingleton()) {
                    auto power=event->object->As<RE::Actor>()->GetActorRuntimeData().selectedPower;
                    auto shout=power ? power->As<RE::TESShout>() : nullptr;
                    if (shout) for (const auto& variation:shout->variations) if (variation.spell && variation.spell->GetFormID()==event->spell) {
                        if (selfTest && shout->GetFormID()==0x13E07) testShoutCast=true;
                        SKSE::log::info("HERO shout cast; shout={:08X}; spell={:08X}; build={}",shout->GetFormID(),event->spell,LIBAOTTG_BUILD_ID);
                        // Wait for the real native cast, so locked words, charging and
                        // cooldown rejection cannot launch the HERO early.
                        if (shout==RE::TESForm::LookupByID<RE::TESShout>(0x2F7BA) && camera && !suspended && !Menu()) {
                            const auto direction=camera->Pose.Forward.Normalized();
                            const auto word=std::size_t(&variation-shout->variations);
                            const float speed=(100.0f+30.0f*float(word))*hero->Motor->Scale;
                            if (LibAottg::WorldBox::Finite(direction) && direction.LengthSquared()>.5f) {
                                whirlwindLaunch=direction*speed;
                                SKSE::log::info("HERO Whirlwind Sprint requested; words={}; speed={}; direction=({},{},{}); build={}",word+1,speed,direction.X,direction.Y,direction.Z,LIBAOTTG_BUILD_ID);
                            }
                        }
                        break;
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        } shoutSink;
        LibAottg::Vec3 testStart{}, testPrevious{};
        float testTravel=0,testRise=0;
        bool testHook=false;
        unsigned testAudioStage=0;
        unsigned testDrawStart=0;
        bool doorCheck=false, testDoorResources=true, testOriginalAutoRestore=true;
        unsigned testDoorsCompleted=0, testDoorPhase=0, testDoorTransfers=0, testDoorDraws=0, testDoorSweeps=0;
        RE::ObjectRefHandle testDoor;
        LibAottg::HeroController* testDoorHero=nullptr;
        float testDoorGas=0, testDoorBlade=0, testDoorCooldown=0, testDoorWalk=0;
        LibAottg::Vec3 testDoorStart{};
        int testDoorBlades=0;
        std::chrono::steady_clock::time_point testDoorDeadline{};
        unsigned testMenuIndex=0, testMenuPhase=0;
        bool testMenusComplete=false, testMenuOpened=false;
        std::chrono::steady_clock::time_point testMenuDeadline{};
        std::jthread testMenuTimer;
        constexpr auto assetDirectory=L"Data/SKSE/Plugins/LibAottg/hero";
        void Notify(const char* text) { if (settings.notifications) RE::SendHUDMessage::ShowHUDMessage(text); }
        bool Menu() {
            if (SettingsMenu::Blocking()) return true;
            auto ui=RE::UI::GetSingleton(); if (!ui) return true;
            if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return true;
            for (const auto& m:ui->menuStack) if (m && m->menuFlags.any(RE::UI_MENU_FLAGS::kPausesGame,RE::UI_MENU_FLAGS::kUsesCursor)) return true;
            return false;
        }
        bool Focus() {
            auto r=RE::BSGraphics::Renderer::GetSingleton();
            if (!r) return false;
            return GetForegroundWindow()==reinterpret_cast<HWND>(r->GetRuntimeData().renderWindows[0].hWnd);
        }
        void ClearInput() {
            whirlwindLaunch.reset();
            if (shoutHeld) {
                if (auto p=RE::PlayerCharacter::GetSingleton())
                    if (auto caster=p->GetMagicCaster(RE::MagicSystem::CastingSource::kOther)) caster->InterruptCast(false);
                shoutHeld=false;
            }
            pending={}; keys.fill(false); mouse.fill(false); if (hero) hero->Input->Reset();
        }
        void AimShout(RE::PlayerCharacter* p) {
            if (!p || !camera) return;
            const auto forward=ToSkyrim(camera->Pose.Forward);
            p->SetHeading(std::atan2(forward.x,forward.y));
            p->SetLooking(-std::atan2(forward.z,std::hypot(forward.x,forward.y)));
        }
        void ShoutInput(RE::ButtonEvent* button) {
            auto p=RE::PlayerCharacter::GetSingleton();
            auto pc=RE::PlayerControls::GetSingleton();
            // Restrict this bridge to an equipped shout; powers keep their normal Skyrim path.
            if (!hero || suspended || Menu() || (!selfTest && !Focus()) || !Eligible(p) || !pc ||
                !pc->shoutHandler || !pc->shoutHandler->IsInputEventHandlingEnabled()) return;
            auto power=p->GetActorRuntimeData().selectedPower;
            if (!power || !power->As<RE::TESShout>()) return;
            if (!button->IsDown() && !shoutHeld) return;
            AimShout(p);
            // Preserve the original event for all other sinks. Only the shout handler
            // receives its Skyrim user-event name, regardless of the HERO key binding.
            const auto userEvent=button->GetUserEvent();
            button->SetUserEvent(RE::UserEvents::GetSingleton()->shout);
            if (pc->shoutHandler->CanProcess(button)) {
                pc->shoutHandler->ProcessButton(button,&pc->data);
                if (button->IsDown()) {
                    shoutHeld=true;
                    SKSE::log::info("HERO shout input; equipped={:08X}; build={}",power->GetFormID(),LIBAOTTG_BUILD_ID);
                }
            }
            button->SetUserEvent(userEvent);
            if (button->IsUp()) shoutHeld=false;
        }
        void ApplySkill(LibAottg::CharacterConfig& config) {
            auto skill=LibAottg::HeroStats::RcModPresets().at(skillIds[currentSkill]).ToConfig();
            config.SkillId=skill.SkillId; config.SkillCooldown=skill.SkillCooldown;
            config.StandAnimation=skill.StandAnimation; config.RunAnimation=skill.RunAnimation;
        }
        void UpdateCrosshair(bool enabled) {
            if (vanillaCrosshair) vanillaCrosshair->SetBool(enabled);
            if (appliedCrosshair && *appliedCrosshair==enabled) return;
            auto queue=RE::UIMessageQueue::GetSingleton();
            auto strings=RE::InterfaceStrings::GetSingleton();
            if (!queue || !strings) return;
            if (auto data=static_cast<RE::HUDData*>(queue->CreateUIMessageData(strings->hudData))) {
                data->type=RE::HUD_MESSAGE_TYPE::kSetCrosshairEnabled;
                data->show=enabled;
                queue->AddMessage(RE::HUDMenu::MENU_NAME,RE::UI_MESSAGE_TYPE::kUpdate,data);
                appliedCrosshair=enabled;
            }
        }
        void Hide(RE::PlayerCharacter* p) {
            UpdateCrosshair(settings.crosshair ? false : savedCrosshair);
            for (bool first:{false,true}) if (auto root=p->Get3D(first)) RE::BSVisit::TraverseScenegraphGeometries(root,[&](RE::BSGeometry* mesh) {
                if (!mesh->GetAppCulled()) { hidden.emplace_back(mesh); mesh->SetAppCulled(true); }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        }
        void SuspendWorld(const char* reason) {
            if (!hero || suspended) return;
            suspended=true; ClearInput(); activateTarget=false;
            // Release the current host before Skyrim unloads its physics world. Never
            // dereference a cached controller after it has been replaced by the engine.
            auto p=RE::PlayerCharacter::GetSingleton();
            if (p && p->GetCharController()==savedController && savedController)
                savedController->gravity=savedGravity;
            savedController=nullptr;
            for (auto& mesh:hidden) if (mesh) mesh->SetAppCulled(false);
            hidden.clear(); playerRoot.reset();
            if (auto pc=RE::PlayerCamera::GetSingleton()) {
                pc->GetRuntimeData2().worldFOV=savedFov;
                pc->GetRuntimeData2().allowAutoVanityMode=savedAutoVanity;
            }
            for (auto& sound:heroSounds) if (sound.IsValid()) sound.Stop();
            smoke.Reset(); for (auto& rope:ropes) rope.Reset();
            Render::Publish(nullptr); Render::SetHudVisible(false);
            SKSE::log::info("HERO world suspended; reason={}; cell={}; autoRestore={}; build={}",reason,cellId,settings.autoRestore,LIBAOTTG_BUILD_ID);
        }
        bool ResumeWorld(RE::PlayerCharacter* p) {
            if (!suspended || Menu() || !Eligible(p) || !Render::Ready()) return false;
            if (expectedDoorCell && p->GetParentCell()->GetFormID()!=expectedDoorCell) return false;
            auto cell=p->GetParentCell(); auto ws=cell->GetRuntimeData().worldSpace;
            savedController=p->GetCharController(); savedGravity=savedController->gravity;
            playerRoot.reset(p->Get3D(false)); cellId=cell->GetFormID(); interior=cell->IsInteriorCell();
            worldspaceId=ws ? ws->GetFormID() : 0; lastPosition=p->GetPosition();
            hero->Motor->Teleport(ToHero(lastPosition),p->GetAngleZ()*180.0f/float(LibAottg::cs_pi));
            if (doorCheck && testDoorPhase==1) {
                testDoorResources &= hero.get()==testDoorHero && hero->Motor->Gas==testDoorGas && hero->Motor->Blade==testDoorBlade &&
                    hero->Motor->SpareBladePairs==testDoorBlades && hero->Motor->SkillCooldownRemaining()==testDoorCooldown;
            }
            character.Reset(); hero->Synchronize(); ClearInput(); world.ResetCombat();
            camera->Reset(hero->Motor->FacingDegrees,0);
            auto pc=RE::PlayerCamera::GetSingleton(); pc->ForceThirdPerson();
            pc->GetRuntimeData2().allowAutoVanityMode=false;
            savedController->gravity=0; suspended=false; menuPaused=false; expectedDoorCell=0; ++worldTransfers;
            Hide(p);
            SKSE::log::info("HERO world resumed; cell={}; interior={}; position=({},{},{}); gravityBaseline={}; transfers={}; autoRestore={}; build={}",
                cellId,interior,lastPosition.x,lastPosition.y,lastPosition.z,savedGravity,worldTransfers,settings.autoRestore,LIBAOTTG_BUILD_ID);
            return true;
        }
        bool Eligible(RE::PlayerCharacter* p) {
            auto c=RE::PlayerCamera::GetSingleton();
            return loaded && p && p->GetParentCell() && p->Get3D(false) && p->GetCharController() &&
                !p->IsDead() && !p->IsOnMount() && !p->IsInKillMove() && p->GetSitSleepState()==RE::SIT_SLEEP_STATE::kNormal &&
                c && (c->IsInFirstPerson() || c->IsInThirdPerson());
        }
        bool Activate() {
            auto p=RE::PlayerCharacter::GetSingleton();
            if (Menu() || !Eligible(p)) { Notify("AoTTG: toggle requires normal, loaded gameplay."); return false; }
            if (!Render::Ready()) { Notify("AoTTG: hero renderer unavailable. See plugin log."); return false; }
            auto config=settings.CharacterConfig(); ApplySkill(config); configuredScale=config.WorldScale;
            character.Reset(); smoke.Reset(); damageHud.Reset(); killInfo.Clear(); world.ResetCombat();
            auto next=std::make_unique<LibAottg::HeroController>(&config,&world);
            auto cc=settings.CameraConfig();
            camera=std::make_unique<LibAottg::HeroCamera>(&cc);
            next->SoundRequested=[](LibAottg::HeroSoundCue cue) {
                const char* path=nullptr;
                switch (cue.Sound) {
                case LibAottg::HeroSound::HookLaunch: path="Sound\\LibAottg\\hook-launch.wav"; break;
                case LibAottg::HeroSound::BladeSwing: path="Sound\\LibAottg\\blade-swing.wav"; break;
                case LibAottg::HeroSound::AhssFire: path="Sound\\LibAottg\\ahss-fire.wav"; break;
                case LibAottg::HeroSound::BladeHit: path="Sound\\LibAottg\\blade-hit.wav"; break;
                case LibAottg::HeroSound::Death: path="Sound\\LibAottg\\death.wav"; break;
                default: return;
                }
                if (auto audio=RE::BSAudioManager::GetSingleton()) {
                    const auto slot=nextSound++%heroSounds.size();
                    auto& sound=heroSounds[slot];
                    soundPaths[slot]=path; soundReported[slot]=false;
                    if (sound.IsValid()) sound.Stop();
                    sound=RE::BSSoundHandle{};
                    RE::BSResource::ID file; file.GenerateFromPath(path);
                    audio->GetSoundHandleByFile(sound,file,0x1A,128);
                    const bool positioned=sound.SetPosition(ToSkyrim(cue.Position));
                    if (auto player=RE::PlayerCharacter::GetSingleton())
                        if (auto root=player->Get3D(false)) sound.SetObjectToFollow(root);
                    sound.SetVolume(cue.Volume); sound.SetFrequency(cue.Pitch);
                    const bool played=sound.Play();
                    SKSE::log::info("HERO sound requested; cue={}; positioned={}; played={}; id={}; build={}",path,positioned,played,sound.soundID,LIBAOTTG_BUILD_ID);
                } else {
                    SKSE::log::warn("HERO sound unavailable: audio manager missing; cue={}",path);
                }
            };
            next->Camera=camera.get(); next->SetPresentation(&character);
            next->Respawn(ToHero(p->GetPosition()),false);
            next->Motor->SetPose(ToHero(p->GetPosition()),{},p->GetAngleZ()*180.0f/float(LibAottg::cs_pi)); next->Synchronize();
            if (auto renderCamera=RE::Main::WorldRootCamera()) {
                const auto& r=renderCamera->world.rotate;
                const auto view=ToHero({r.entry[0][0],r.entry[1][0],r.entry[2][0]}).Normalized();
                camera->Reset(std::atan2(view.X,view.Z)*180.0f/float(LibAottg::cs_pi),std::asin(std::clamp(view.Y,-1.0f,1.0f))*180.0f/float(LibAottg::cs_pi));
            }
            auto pc=RE::PlayerCamera::GetSingleton(); savedCamera=pc->currentState; savedFov=pc->GetRuntimeData2().worldFOV;
            savedAutoVanity=pc->GetRuntimeData2().allowAutoVanityMode;
            savedController=p->GetCharController(); savedGravity=savedController->gravity;
            playerRoot.reset(p->Get3D(false)); cellId=p->GetParentCell()->GetFormID(); lastPosition=p->GetPosition();
            interior=p->GetParentCell()->IsInteriorCell();
            auto ws=p->GetParentCell()->GetRuntimeData().worldSpace; worldspaceId=ws ? ws->GetFormID() : 0;
            vanillaCrosshair=RE::INIPrefSettingCollection::GetSingleton()->GetSetting("bShowCrosshair:Interface");
            if(!vanillaCrosshair) vanillaCrosshair=RE::INISettingCollection::GetSingleton()->GetSetting("bShowCrosshair:Interface");
            savedCrosshair=vanillaCrosshair ? vanillaCrosshair->GetBool() : true;
            appliedCrosshair.reset();
            hero=std::move(next); suspended=menuPaused=false; ClearInput(); clock=0; Hide(p);
            restoreHistory.clear(); nextRestoreSample={}; lastDiagnosticCamera=-2; updateStage="activated";
            hero->Combat=settings.combat; hero->ProceduralPose=settings.proceduralPose;
            world.scoreMultiplier=settings.damageMultiplier; world.bladeHitboxMultiplier=settings.bladeHitboxMultiplier; SettingsMenu::PublishStatus(true,configuredScale);
            pc->GetRuntimeData2().allowAutoVanityMode=false;
            pc->ForceThirdPerson();
            SKSE::log::info("HERO auto-vanity disabled; previous={}; build={}",savedAutoVanity,LIBAOTTG_BUILD_ID);
            savedController->gravity=0;
            for (auto& r:ropes) r.Reset();
            const auto message="AoTTG hero enabled. "+BindingName(settings.bindings[0])+" returns to Skyrim."; Notify(message.c_str());
            SKSE::log::info("HERO enabled; position=({},{},{}); scale={}; build={}",lastPosition.x,lastPosition.y,lastPosition.z,configuredScale,LIBAOTTG_BUILD_ID);
            return true;
        }
        void Publish() {
            auto f=std::make_shared<Render::Frame>(); const auto root=hero->RenderPose;
            f->smoke=smoke.Particles();
            f->ropeThicknessMultiplier=settings.ropeThicknessMultiplier;
            f->damage=damageHud; f->kills=killInfo; f->damageHud=settings.damageHud; f->killInfo=settings.killInfo;
            f->hudComparison=hudComparison;
            f->resourceHud=settings.heroHud || settings.crosshair;
            f->heroHud.ResourcesVisible=settings.heroHud;
            f->heroHud.Read(*hero->Motor); f->heroHud.AimVisible=settings.crosshair;
            f->heroHud.ShowDistance=settings.crosshairDistance; f->heroHud.ShowArrows=settings.crosshairArrows;
            f->heroHud.CrosshairStyle=static_cast<LibAottg::HeroCrosshairStyle>(settings.crosshairStyle);
            f->heroHud.Speedometer=static_cast<LibAottg::HeroSpeedometer>(settings.speedometer);
            auto renderer=RE::BSGraphics::Renderer::GetSingleton();RECT viewport{};
            GetClientRect(reinterpret_cast<HWND>(renderer->GetRuntimeData().renderWindows[0].hWnd),&viewport);
            float aspect=float(std::max(1L,viewport.right))/float(std::max(1L,viewport.bottom));
            auto hudView=camera->Pose;
            hudView.FieldOfView=std::clamp(settings.fieldOfView+(settings.dynamicFov?hudView.FieldOfView-50:0),20.f,120.f);
            if(f->heroHud.AimVisible)f->heroHud.Aim(*hero->Motor,hero->LastInput,world,hudView,aspect);
            for (size_t i=0;i<character.meshes.size();++i) {
                if (!character.meshes[i].visible) continue;
                Render::Mesh mesh; mesh.material=character.primitives[i].material; const auto& src=character.meshes[i].vertices;
                mesh.vertices.reserve(character.primitives[i].indices.size());
                for (auto index:character.primitives[i].indices) {
                    const auto& v=src[index]; auto pos=root.Position+root.Rotate(LibAottg::Vec3(v.position.x,v.position.y,-v.position.z)*(configuredScale/character.metersPerUnit));
                    auto n=root.Rotate({v.normal.x,v.normal.y,-v.normal.z}); auto s=ToSkyrim(pos),sn=ToSkyrim(n);
                    mesh.vertices.push_back({{s.x,s.y,s.z},{sn.x,sn.y,sn.z},v.uv});
                }
                f->meshes.push_back(std::move(mesh));
            }
            for (int side=0;settings.showRopes && side<2;++side) {
                ropes[side].Sample(hero->Motor,LibAottg::HookSide(side),character.HookOrigin(hero->Motor,LibAottg::HookSide(side)),clock);
                const auto& points=ropes[side].Points();
                for (size_t j=1;j<points.size();++j) for (auto point:{points[j-1],points[j]}) { auto s=ToSkyrim(point); f->ropes.push_back({{s.x,s.y,s.z},{0,0,1},{0,0}}); }
            }
            Render::Publish(std::move(f));
        }
        LibAottg::HeroInputFrame Input() {
            using B=LibAottg::HeroButtons;
            auto f=pending; pending.MouseDeltaX=pending.MouseDeltaY=0; pending.Pressed=pending.Released=B::None; pending.Reel=0;
            auto held=[](Action a) { auto b=settings.bindings[size_t(a)]; return b>=256?mouse[size_t(b-256)]:(b>=0 && keys[size_t(b)]); };
            f.MoveX=float(held(Action::Right))-float(held(Action::Left)); f.MoveY=float(held(Action::Forward))-float(held(Action::Back));
            f.Held=B::None;
            for (size_t i=0;i<ActionCount;++i) if (held(Action(i))) f.Held|=ButtonFor(Action(i));
            if (held(Action::ReelIn)) f.Reel=-1; else if (held(Action::ReelOut)) f.Reel=1;
            auto r=RE::BSGraphics::Renderer::GetSingleton(); auto& w=r->GetRuntimeData().renderWindows[0];
            f.AspectRatio=w.windowHeight>0 ? float(w.windowWidth)/w.windowHeight : 16.0f/9; return f;
        }
        void TestMenus() {
            if (!doorCheck || testFinished || testClock<=7 || testMenusComplete) return;
            auto ui=RE::UI::GetSingleton(); auto queue=RE::UIMessageQueue::GetSingleton();
            const auto name=testMenuIndex==0 ? RE::InventoryMenu::MENU_NAME : RE::MagicMenu::MENU_NAME;
            auto fail=[&](const char* reason) {
                SKSE::log::error("HERO gameplay check failed: {} persistence {}; build={}",name,reason,LIBAOTTG_BUILD_ID);
                testMenuTimer.request_stop();
                testFinished=true; queue->AddMessage(name,RE::UI_MESSAGE_TYPE::kHide,nullptr); Exit("failed menu check");
            };
            if (!hero) { fail("lost HERO"); return; }
            const auto now=std::chrono::steady_clock::now();
            if (testMenuPhase==0) {
                if (Menu()) return;
                testDoorHero=hero.get(); testMenuOpened=false; testMenuPhase=1;
                testMenuDeadline=now+std::chrono::seconds(10);
                // Paused inventory/magic menus stop the camera/gameplay hooks. A
                // timer only enqueues work; every UI/runtime access stays on the
                // game thread. This runs solely in the explicit unsaved check.
                if (!testMenuTimer.joinable()) testMenuTimer=std::jthread([](std::stop_token stop) {
                    while (!stop.stop_requested()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        if (!stop.stop_requested()) SKSE::GetTaskInterface()->AddTask([] { TestMenus(); });
                    }
                });
                queue->AddMessage(name,RE::UI_MESSAGE_TYPE::kShow,nullptr); return;
            }
            if (testMenuPhase==1) {
                if (!testMenuOpened) { if (now>testMenuDeadline) fail("did not open"); return; }
                if (now<testMenuDeadline) return;
                const bool retained=ui->IsMenuOpen(name) && hero.get()==testDoorHero && hero->Motor->Gas==testDoorGas &&
                    hero->Motor->Blade==testDoorBlade && hero->Motor->SkillCooldownRemaining()==testDoorCooldown;
                if (!retained) { fail("changed character/resources while open"); return; }
                queue->AddMessage(name,RE::UI_MESSAGE_TYPE::kHide,nullptr); testMenuPhase=2;
                testMenuDeadline=now+std::chrono::seconds(10); return;
            }
            if (Menu()) { if (now>testMenuDeadline) fail("did not close"); return; }
            if (hero.get()!=testDoorHero) { fail("did not resume the same HERO"); return; }
            SKSE::log::info("HERO menu persistence check passed; menu={}; resources=true; build={}",name,LIBAOTTG_BUILD_ID);
            testMenuPhase=0;
            if (++testMenuIndex==2) { testMenusComplete=true; testMenuTimer.request_stop(); }
        }
        bool TestDoors(float dt) {
            auto fail=[](const char* reason) {
                SKSE::log::error("HERO gameplay check failed: loading door {}; completed={}; build={}",reason,testDoorsCompleted,LIBAOTTG_BUILD_ID);
                settings.autoRestore=testOriginalAutoRestore; testFinished=true; Exit("failed loading door check");
            };
            if (!hero) { fail("lost HERO"); return false; }
            auto p=RE::PlayerCharacter::GetSingleton();
            if (testDoorPhase==0) {
                testOriginalAutoRestore=settings.autoRestore;
                float closest=std::numeric_limits<float>::max();
                RE::TES::GetSingleton()->ForEachReference([&](RE::TESObjectREFR* ref) {
                    auto extra=ref->extraList.GetByType<RE::ExtraTeleport>();
                    auto linked=extra && extra->teleportData ? extra->teleportData->linkedDoor.get() : RE::NiPointer<RE::TESObjectREFR>{};
                    if (linked && linked->GetParentCell() && linked->GetParentCell()->IsInteriorCell() && !ref->IsDisabled() && !ref->IsLocked()) {
                        const float distance=ref->GetPosition().GetDistance(p->GetPosition());
                        if (distance<closest) { closest=distance; testDoor=ref->GetHandle(); }
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                if (!testDoor) { fail("could not find an unlocked Riverwood loading door"); return false; }
            }
            if (testDoorPhase==1) {
                if (std::chrono::steady_clock::now()>testDoorDeadline) { fail("timed out"); return false; }
                if (suspended || worldTransfers==testDoorTransfers) return false;
                testDoorPhase=2; testDoorWalk=0; testDoorDraws=Render::Draws(); testDoorSweeps=world.sweeps;
                testDoorStart=hero->Motor->Position();
            }
            if (testDoorPhase==2) {
                testDoorWalk+=dt;
                if (testDoorWalk<.5f) {
                    const auto b=settings.bindings[size_t(Action::Forward)];
                    if (b>=256) mouse[size_t(b-256)]=true; else if (b>=0) keys[size_t(b)]=true;
                    return false;
                }
                ClearInput();
                const bool healthy=hero.get()==testDoorHero && p->GetParentCell() && p->Get3D(false)==playerRoot.get() &&
                    p->GetCharController()==savedController && savedController && savedController->gravity==0 &&
                    ToSkyrim(hero->Motor->Position()).GetDistance(p->GetPosition())<1 && (hero->Motor->Position()-testDoorStart).Length()>1 &&
                    Render::Draws()>testDoorDraws && world.sweeps>testDoorSweeps;
                if (!healthy || !testDoorResources) { fail("destination movement/render/resources failed"); return false; }
                ++testDoorsCompleted;
                SKSE::log::info("HERO loading door check passed; transition={}; interior={}; autoRestore={}; resources={}; travel={}; draws={}; sweeps={}; cell={}; build={}",
                    testDoorsCompleted,p->GetParentCell()->IsInteriorCell(),settings.autoRestore,testDoorResources,(hero->Motor->Position()-testDoorStart).Length(),Render::Draws()-testDoorDraws,world.sweeps-testDoorSweeps,cellId,LIBAOTTG_BUILD_ID);
                if (testDoorsCompleted==4) {
                    settings.autoRestore=testOriginalAutoRestore; testDoorPhase=3; return true;
                }
            }
            auto door=testDoor.get();
            auto extra=door ? door->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
            auto linked=extra && extra->teleportData ? extra->teleportData->linkedDoor.get() : RE::NiPointer<RE::TESObjectREFR>{};
            if (!linked || !linked->GetParentCell()) { fail("linked door unavailable"); return false; }
            settings.autoRestore=testDoorsCompleted<2;
            ClearInput(); testDoorHero=hero.get(); testDoorGas=hero->Motor->Gas; testDoorBlade=hero->Motor->Blade;
            testDoorBlades=hero->Motor->SpareBladePairs; testDoorCooldown=hero->Motor->SkillCooldownRemaining();
            testDoorTransfers=worldTransfers; testDoorPhase=1; testDoorDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(40);
            expectedDoorCell=linked->GetParentCell()->GetFormID(); testDoor=linked->GetHandle();
            SKSE::log::info("HERO loading door requested; door={:08X}; destination={:08X}; autoRestore={}; build={}",door->GetFormID(),expectedDoorCell,settings.autoRestore,LIBAOTTG_BUILD_ID);
            const bool activated=door->ActivateRef(p,0,nullptr,0,false);
            SKSE::log::info("HERO loading door activation returned {}; build={}",activated,LIBAOTTG_BUILD_ID);
            return false;
        }
        bool TestWhirlwind(float dt) {
            auto p=RE::PlayerCharacter::GetSingleton();
            if (!testWhirlwindStage) {
                ClearInput();
                auto process=p->GetActorRuntimeData().currentProcess;
                const float recovery=process && process->high ? process->high->voiceRecoveryTime : 0;
                SKSE::log::info("HERO shout check {}; cast={}; projectile={}; recovery={}; build={}",testShoutCast && testShoutProjectile && recovery>0 ? "passed" : "failed",testShoutCast,testShoutProjectile,recovery,LIBAOTTG_BUILD_ID);
                // Reset recovery only in the unsaved test to exercise a second shout.
                if (process && process->high) process->high->voiceRecoveryTime=0;
                auto factory=RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
                for (const char* command:{"player.teachword 2f7bb","player.unlockword 2f7bb","player.addshout 2f7ba","player.equipshout 2f7ba"}) {
                    if (auto script=factory ? factory->Create() : nullptr) { script->SetCommand(command); script->CompileAndRun(p); delete script; }
                }
                // Give the camera time to publish a non-horizontal aiming direction.
                camera->Reset(camera->Pose.Yaw,25);
                testWhirlwindStart=hero->Motor->Position(); testWhirlwindStage=1;
            }
            testWhirlwindClock+=dt;
            if (testWhirlwindStage==1 && testWhirlwindClock>.5f) { TestShoutInput(1,0); testWhirlwindStage=2; }
            if (testWhirlwindStage==2 && testWhirlwindClock>1) { TestShoutInput(0,.5f); testWhirlwindStage=3; }
            // A second key press during recovery must not generate another launch.
            if (testWhirlwindStage==3 && testWhirlwindClock>1.5f) { TestShoutInput(1,0); testWhirlwindStage=4; }
            if (testWhirlwindStage==4 && testWhirlwindClock>1.7f) { TestShoutInput(0,.2f); testWhirlwindStage=5; }
            if (testWhirlwindClock<2.5f) return false;
            const auto delta=hero->Motor->Position()-testWhirlwindStart;
            auto process=p->GetActorRuntimeData().currentProcess;
            const float recovery=process && process->high ? process->high->voiceRecoveryTime : 0;
            const bool ok=testWhirlwindLaunches==1 && testWhirlwindDirection.Y>.3f &&
                LibAottg::Vec3::Dot(delta,testWhirlwindDirection)>100 && delta.Y>50 && recovery>0;
            SKSE::log::info("HERO Whirlwind Sprint check {}; launches={}; displacement=({},{},{}); recovery={}; build={}",ok ? "passed" : "failed",testWhirlwindLaunches,delta.X,delta.Y,delta.Z,recovery,LIBAOTTG_BUILD_ID);
            return true;
        }
        void Test(float dt) {
            if (!selfTest || testFinished || Menu()) return;
            if (!testStarted) {
                settleClock+=dt; if (settleClock<5 || !Eligible(RE::PlayerCharacter::GetSingleton())) return;
                if (!Activate()) { testFinished=true; SKSE::log::error("HERO gameplay check failed: could not activate; build={}",LIBAOTTG_BUILD_ID); return; }
                testStarted=true; testStart=hero->Motor->Position(); testPrevious=testStart; testDrawStart=Render::Draws();
                // Only the explicitly requested, unsaved gameplay check teaches a shout.
                auto p=RE::PlayerCharacter::GetSingleton();
                auto factory=RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
                for (const char* command:{"player.teachword 13e22","player.unlockword 13e22","player.addshout 13e07","player.equipshout 13e07"}) {
                    if (auto script=factory ? factory->Create() : nullptr) { script->SetCommand(command); script->CompileAndRun(p); delete script; }
                }
            }
            if (doorCheck && testClock>7) {
                if (!testMenusComplete) return;
                if (!TestDoors(dt)) return;
            } else {
            testClock+=dt;
            if (hero && testShoutStage<2 && testClock>(testShoutStage==0 ? 2.5f : 3.0f)) {
                TestShoutInput(testShoutStage==0 ? 1.0f : 0.0f,testShoutStage==0 ? 0.0f : .5f);
                ++testShoutStage;
            }
            if (hero && testShoutStage && !testShoutProjectile) {
                auto p=RE::PlayerCharacter::GetSingleton();
                RE::TES::GetSingleton()->ForEachReferenceInRange(p,10000,[&](RE::TESObjectREFR* ref) {
                    if (auto projectile=ref->As<RE::Projectile>()) {
                        const auto& data=projectile->GetProjectileRuntimeData();
                        if (data.shooter.get().get()==p && data.spell && data.spell->GetFormID()==0x13E09) {
                            testShoutProjectile=true;
                            SKSE::log::info("HERO shout projectile; ref={:08X}; position=({},{},{}); build={}",ref->GetFormID(),ref->GetPosition().x,ref->GetPosition().y,ref->GetPosition().z,LIBAOTTG_BUILD_ID);
                        }
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            if (testClock>2 && !testVanityAttempted && hero) {
                testVanityAttempted=true;
                auto pc=RE::PlayerCamera::GetSingleton();
                const bool disabled=!pc->GetRuntimeData2().allowAutoVanityMode;
                // Simulate an external camera override without waiting two minutes.
                pc->GetRuntimeData2().allowAutoVanityMode=true;
                pc->SetState(pc->GetRuntimeData().cameraStates[RE::CameraStates::kAutoVanity].get());
                const bool entered=pc->currentState && pc->currentState->id==RE::CameraStates::kAutoVanity;
                SuppressVanity();
                testVanity=disabled && entered && pc->IsInThirdPerson() && !pc->GetRuntimeData2().allowAutoVanityMode;
                SKSE::log::info("HERO auto-vanity check {}; initiallyDisabled={}; forcedVanity={}; recovered={}; build={}",
                    testVanity ? "passed" : "failed",disabled,entered,pc->IsInThirdPerson(),LIBAOTTG_BUILD_ID);
            }
            // Exercise every audio adapter cue in the explicitly enabled unsaved check.
            if (testAudioStage<5 && testClock>float(testAudioStage)) {
                constexpr LibAottg::HeroSound cues[]={LibAottg::HeroSound::HookLaunch,LibAottg::HeroSound::BladeSwing,
                    LibAottg::HeroSound::AhssFire,LibAottg::HeroSound::BladeHit,LibAottg::HeroSound::Death};
                hero->SoundRequested(LibAottg::HeroSoundCue(cues[testAudioStage++],hero->Motor->Position()));
            }
            for (size_t slot=0;slot<heroSounds.size();++slot) {
                if (!soundReported[slot] && soundPaths[slot] && heroSounds[slot].IsValid() && heroSounds[slot].IsPlaying()) {
                    soundReported[slot]=true;
                    SKSE::log::info("HERO audio check: engine reports active playback; cue={}; build={}",soundPaths[slot],LIBAOTTG_BUILD_ID);
                }
            }
            if (hero) {
                auto p=hero->Motor->Position(); testTravel+=(p-testPrevious).Length(); testPrevious=p;
                testRise=std::max(testRise,p.Y-testStart.Y);
                testHook|=hero->Motor->LeftHookAttached() || hero->Motor->RightHookAttached();
            }
            auto set=[](Action a,bool held) { auto b=settings.bindings[size_t(a)]; if (b>=256) mouse[size_t(b-256)]=held; else if (b>=0) keys[size_t(b)]=held; };
            if (testClock<1) set(Action::Forward,true);
            else if (testStage==0) { set(Action::Forward,false); set(Action::Jump,true); testStage=1; }
            else if (testClock>1.15f && testStage==1) { set(Action::Jump,false); testStage=2; }
            if (testClock>4 && testStage==2) { set(Action::HookLeft,true); testStage=3; }
            if (testClock>5 && testStage==3) { set(Action::HookLeft,false); testStage=4; }
            if (doorCheck && testClock>7) { ClearInput(); return; }
            }
            if (testClock>7) {
                if (!doorCheck && !TestWhirlwind(dt)) return;
                auto p=RE::PlayerCharacter::GetSingleton();
                auto process=p->GetActorRuntimeData().currentProcess;
                const float recovery=process && process->high ? process->high->voiceRecoveryTime : 0;
                if (doorCheck) SKSE::log::info("HERO shout check {}; cast={}; projectile={}; recovery={}; build={}",testShoutCast && testShoutProjectile ? "passed" : "failed",testShoutCast,testShoutProjectile,recovery,LIBAOTTG_BUILD_ID);
                auto meshes=hidden; auto controller=savedController; float gravity=savedGravity,fov=savedFov; auto state=savedCamera; const bool vanity=savedAutoVanity;
                auto controls=RE::ControlMap::GetSingleton()->GetRuntimeData().enabledControls;
                unsigned restored=unsigned(hidden.size()); Exit("automated gameplay check");
                auto pc=RE::PlayerCamera::GetSingleton();
                bool restoreOk=controller->gravity==gravity && pc->GetRuntimeData2().worldFOV==fov && pc->currentState==state && pc->GetRuntimeData2().allowAutoVanityMode==vanity &&
                    RE::ControlMap::GetSingleton()->GetRuntimeData().enabledControls==controls && std::ranges::all_of(meshes,[](const auto& mesh) { return !mesh->GetAppCulled(); });
                bool ok=testTravel>20 && testRise>10 && Render::Draws()>testDrawStart && restored>0 && world.sweeps>0 && testHook && testVanity && restoreOk;
                SKSE::log::info("HERO gameplay check {}; travel={}; rise={}; hookAttached={}; worldDraws={}; capsuleSweeps={}; rays={}; volumesIgnored={}; ignoredBySweeps={}; ignoredByProbes={}; rayExhausted={}; restoredMeshes={}; restoration={}; build={}",ok?"passed":"failed",testTravel,testRise,testHook,Render::Draws()-testDrawStart,world.sweeps,world.rayQueries,world.filtered,world.filteredSweeps,world.filteredProbes,world.rayExhausted,restored,restoreOk,LIBAOTTG_BUILD_ID);
                testFinished=true;
            }
        }
        void Update(RE::PlayerCharacter* p,float dt) {
            lastUpdateDt=dt; updateStage="lifecycle";
            if (cycleSkill) {
                cycleSkill=false; SelectSkill((currentSkill+1)%skillIds.size());
            }
            if (toggle) { toggle=false; if (hero) Exit("hotkey"); else Activate(); }
            if (selfTest) Test(dt);
            if (!hero) return;
            if (suspended) { if (!ResumeWorld(p)) return; }
            // Inventory/magic use Skyrim's tween camera. Keep the HERO and its
            // resources while the menu owns input/camera instead of treating that
            // temporary camera state as a gameplay lifecycle failure.
            if (Menu()) {
                if (!menuPaused) SKSE::log::info("HERO menu paused; camera={}; build={}",CameraId(),LIBAOTTG_BUILD_ID);
                menuPaused=true; Render::SetHudVisible(false); ClearInput(); return;
            }
            if (menuPaused) {
                menuPaused=false;
                if (auto pc=RE::PlayerCamera::GetSingleton(); pc && pc->currentState && pc->currentState->id==RE::CameraStates::kTween)
                    pc->ForceThirdPerson();
                SKSE::log::info("HERO menu resumed; camera={}; build={}",CameraId(),LIBAOTTG_BUILD_ID);
            }
            SuppressVanity();
            SampleRestoration();
            if (activateTarget) {
                activateTarget=false;
                auto pick=RE::CrosshairPickData::GetSingleton(); auto target=pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
                if (target && target.get()!=p) {
                    target->ActivateRef(p,0,nullptr,0,false);
                    if (suspended) return;
                }
            }
            auto cell=p->GetParentCell(); auto ws=cell ? cell->GetRuntimeData().worldSpace : nullptr;
            bool cellChange=cell && cell->GetFormID()!=cellId;
            bool worldChange=cellChange && (interior || cell->IsInteriorCell() || (ws ? ws->GetFormID() : 0)!=worldspaceId);
            if (worldChange || p->GetPosition().GetDistance(lastPosition)>500 || p->Get3D(false)!=playerRoot.get() || p->GetCharController()!=savedController) {
                SuspendWorld("host world/geometry changed"); ResumeWorld(p); return;
            }
            if (!Render::Ready() || !Eligible(p) || p->Get3D(false)!=playerRoot.get() || p->GetCharController()!=savedController ||
                worldChange || p->GetPosition().GetDistance(lastPosition)>500) {
                auto playerCamera=RE::PlayerCamera::GetSingleton();
                SKSE::log::info("HERO lifecycle: eligible={}; dead={}; mounted={}; killMove={}; sitSleep={}; firstPerson={}; thirdPerson={}; hasCell={}; has3D={}; hasController={}; rendererReady={}; rootChanged={}; controllerChanged={}; worldChanged={}; cellChanged={}; displacement={}",
                    Eligible(p),p->IsDead(),p->IsOnMount(),p->IsInKillMove(),static_cast<int>(p->GetSitSleepState()),
                    playerCamera && playerCamera->IsInFirstPerson(),playerCamera && playerCamera->IsInThirdPerson(),
                    p->GetParentCell()!=nullptr,p->Get3D(false)!=nullptr,p->GetCharController()!=nullptr,Render::Ready(),
                    p->Get3D(false)!=playerRoot.get(),p->GetCharController()!=savedController,worldChange,
                    !cell || cell->GetFormID()!=cellId,p->GetPosition().GetDistance(lastPosition));
                Exit("player lifecycle changed",true); if (selfTest) { testFinished=true; SKSE::log::error("HERO gameplay check failed: lifecycle interrupted; build={}",LIBAOTTG_BUILD_ID); } return;
            }
            if (cellChange) cellId=cell->GetFormID();
            if (Menu() || (!selfTest && !Focus())) { Render::SetHudVisible(false); ClearInput(); return; }
            Render::SetHudVisible(true);
            if (!std::isfinite(dt) || dt<=0) return;
            updateStage="simulation";
            if (whirlwindLaunch) {
                // Assign velocity, never position: the normal fixed-step capsule sweeps
                // remain responsible for walls, terrain and subsequent momentum.
                const auto velocity=*whirlwindLaunch; whirlwindLaunch.reset();
                hero->Motor->SetPose(hero->Motor->Position(),velocity,hero->Motor->FacingDegrees);
                if (selfTest) { ++testWhirlwindLaunches; testWhirlwindDirection=velocity.Normalized(); }
                SKSE::log::info("HERO Whirlwind Sprint launched; velocity=({},{},{}); build={}",velocity.X,velocity.Y,velocity.Z,LIBAOTTG_BUILD_ID);
            }
            auto input=Input(); hero->Advance(std::min(dt,0.1f),input); smoke.Advance(std::min(dt,0.1f),hero->Motor,hero->RenderPose); clock+=dt;
            auto pos=hero->Motor->Position(); if (!LibAottg::WorldBox::Finite(pos)) { Exit("invalid hero position",true); return; }
            updateStage="player synchronization";
            p->SetPosition(ToSkyrim(pos),true); lastPosition=p->GetPosition();
            AimShout(p);
            auto ctrl=p->GetCharController(); ctrl->gravity=0; ctrl->SetLinearVelocityImpl(RE::hkVector4(0,0,0,0)); ctrl->fallTime=0; ctrl->fallStartHeight=lastPosition.z;
            ctrl->context.currentState=hero->Motor->Grounded ? RE::hkpCharacterStateType::kOnGround : RE::hkpCharacterStateType::kInAir;
            if (hero->Motor->Grounded) ctrl->flags.set(RE::CHARACTER_FLAGS::kSupport); else ctrl->flags.reset(RE::CHARACTER_FLAGS::kSupport);
            auto renderer=RE::BSGraphics::Renderer::GetSingleton(); RECT viewport{};
            GetClientRect(reinterpret_cast<HWND>(renderer->GetRuntimeData().renderWindows[0].hWnd),&viewport);
            float width=float(std::max(1L,viewport.right)),height=float(std::max(1L,viewport.bottom));
            damageHud.Resize(width);
            for (auto& n:world.DrainNotices()) {
                if(n.kill) killInfo.Show(false,n.killer,false,n.victim,n.damage,height);
                else damageHud.ShowDamage(n.damage);
            }
            damageHud.Advance(dt); killInfo.Advance(dt,height);
            updateStage="presentation";
            Hide(p); Publish();
            updateStage="complete";
        }
        struct PlayerHook {
            static void Thunk(RE::PlayerCharacter* p,float dt) { original(p,dt); try { Update(p,dt); } catch (const std::exception& e) { SKSE::log::error("HERO update failed: {}",e.what()); Exit("update failure",true); } }
            static inline REL::Relocation<decltype(Thunk)> original;
        };
        struct CameraHook {
            static void Thunk(RE::PlayerCamera* pc) {
                TestMenus();
                SuppressVanity();
                original(pc); if (!hero || suspended || Menu() || !pc->cameraRoot) return;
                const auto& view=camera->Pose; auto root=pc->cameraRoot.get(); auto pos=ToSkyrim(view.Position), forward=ToSkyrim(view.Forward),up=ToSkyrim(view.Up);
                auto right=forward.Cross(up); RE::NiMatrix3 desired;
                // NiCamera columns are view-direction/up/right. Determine the fixed child
                // basis from the live hierarchy instead of assuming camera-root axes.
                for (int i=0;i<3;++i) { const float r[]={right.x,right.y,right.z},f[]={forward.x,forward.y,forward.z},u[]={up.x,up.y,up.z}; desired.entry[i][0]=f[i]; desired.entry[i][1]=u[i]; desired.entry[i][2]=r[i]; }
                auto renderCamera=RE::Main::WorldRootCamera();
                if (!renderCamera) return;
                const auto childBasis=root->world.rotate.Transpose()*renderCamera->world.rotate;
                const auto rot=desired*childBasis.Transpose();
                root->world.translate=pos; root->world.rotate=rot;
                if (root->parent) {
                    const auto& parent=root->parent->world; root->local.translate=parent.rotate.Transpose()*(pos-parent.translate)/parent.scale; root->local.rotate=parent.rotate.Transpose()*rot;
                } else { root->local.translate=pos; root->local.rotate=rot; }
                pc->GetRuntimeData2().pos=pos;
                const auto verticalFov=std::clamp(settings.fieldOfView+(settings.dynamicFov?view.FieldOfView-50:0),20.0f,120.0f);
                pc->GetRuntimeData2().worldFOV=2*std::atan(std::tan(verticalFov*float(LibAottg::cs_pi)/360)*(4.0f/3))*180/float(LibAottg::cs_pi);
                RE::NiUpdateData update{}; root->UpdateDownwardPass(update,0);
                if (auto sky=RE::Sky::GetSingleton(); sky && sky->root) { sky->root->local.translate=pos; sky->root->world.translate=pos; }
            }
            static inline REL::Relocation<decltype(Thunk)> original;
        };
        bool ToggleEvent(RE::InputEvent* event) {
            auto button=event ? event->AsButtonEvent() : nullptr; if (!button) return false;
            const auto code=button->GetIDCode();
            const int binding=button->GetDevice()==RE::INPUT_DEVICE::kKeyboard?int(code):
                (button->GetDevice()==RE::INPUT_DEVICE::kMouse && code<8?256+int(code):-1);
            return binding>=0 && binding!=SettingsMenu::ReservedKey() && binding==settings.bindings[0];
        }
        bool CycleSkillEvent(RE::InputEvent* event) {
            auto button=event ? event->AsButtonEvent() : nullptr; if (!button) return false;
            const auto code=button->GetIDCode();
            const int binding=button->GetDevice()==RE::INPUT_DEVICE::kKeyboard?int(code):
                (button->GetDevice()==RE::INPUT_DEVICE::kMouse && code<8?256+int(code):-1);
            return binding>=0 && binding==settings.bindings[size_t(Action::CycleSkill)];
        }
        struct InputRoute {
            std::vector<std::pair<RE::InputEvent*,RE::InputEvent*>> links;
            std::vector<RE::InputEvent*> keep;
            RE::InputEvent* head=nullptr;
            template<class Predicate> InputRoute(RE::InputEvent* const* events,Predicate include) {
                if (events) for (auto event=*events;event;event=event->next) {
                    links.emplace_back(event,event->next); if (include(event)) keep.push_back(event);
                }
                for (size_t i=0;i<keep.size();++i) keep[i]->next=i+1<keep.size()?keep[i+1]:nullptr;
                head=keep.empty()?nullptr:keep.front();
            }
            ~InputRoute() { for (auto [event,next]:links) event->next=next; }
        };
        struct InputHook {
            static RE::BSEventNotifyControl Thunk(RE::PlayerControls* pc,RE::InputEvent* const* events,RE::BSTEventSource<RE::InputEvent*>* source) {
                if (Menu()) { ClearInput(); return original(pc,events,source); }
                if (events) for (auto e=*events;e;e=e->next) {
                    if (auto b=e->AsButtonEvent()) {
                        const auto code=b->GetIDCode();
                        if (b->GetDevice()==RE::INPUT_DEVICE::kKeyboard) {
                            if (code<keys.size()) keys[code]=b->IsPressed();
                        } else if (b->GetDevice()==RE::INPUT_DEVICE::kMouse && code<mouse.size()) {
                            mouse[code]=b->IsPressed();
                            if (hero && settings.mouseWheelReel && b->IsDown() && code>=8) pending.Reel=(code==8 ? -1.0f : 1.0f)*(settings.invertWheel?-1.0f:1.0f);
                        }
                        const int binding=b->GetDevice()==RE::INPUT_DEVICE::kKeyboard?int(code):
                            (b->GetDevice()==RE::INPUT_DEVICE::kMouse && code<8?256+int(code):-1);
                        if (binding==SettingsMenu::ReservedKey()) continue;
                        if (binding>=0 && binding==settings.bindings[0] && b->IsDown()) toggle=true;
                        if (binding>=0 && binding==settings.bindings[size_t(Action::CycleSkill)] && b->IsDown()) cycleSkill=true;
                        if (hero && binding>=0 && binding==settings.bindings[size_t(Action::Interact)] && b->IsDown()) activateTarget=true;
                        if (hero && binding>=0 && binding==settings.bindings[size_t(Action::Shout)]) ShoutInput(b);
                        if (hero) {
                            const auto action=ButtonsFor(settings,binding);
                            if (b->IsDown()) pending.Pressed|=action;
                            if (b->IsUp()) pending.Released|=action;
                        }
                    } else if (hero) if (auto m=e->AsMouseMoveEvent()) { pending.MouseDeltaX+=float(m->mouseInputX); pending.MouseDeltaY+=float(m->mouseInputY); }
                }
                if (hero) return RE::BSEventNotifyControl::kContinue;
                // A remapped switch key must not also attack, quicksave or move the normal
                // actor before the queued character switch executes on the game thread.
                InputRoute route(events,[](auto event) { return !ToggleEvent(event) && !CycleSkillEvent(event); });
                return original(pc,&route.head,source);
            }
            static inline REL::Relocation<decltype(Thunk)> original;
        };
        void TestShoutInput(float value,float duration) {
            const auto binding=settings.bindings[size_t(Action::Shout)];
            if (binding<0) return;
            auto event=RE::ButtonEvent::Create(binding>=256 ? RE::INPUT_DEVICE::kMouse : RE::INPUT_DEVICE::kKeyboard,
                RE::BSFixedString{},std::uint32_t(binding>=256 ? binding-256 : binding),value,duration);
            if (event) {
                RE::InputEvent* head=event;
                InputHook::Thunk(RE::PlayerControls::GetSingleton(),&head,nullptr);
                delete event;
            }
        }
        struct MenuInputHook {
            static RE::BSEventNotifyControl Thunk(RE::MenuControls* controls,RE::InputEvent* const* events,RE::BSTEventSource<RE::InputEvent*>* source) {
                if (Menu() || !events) return original(controls,events,source);
                // Favorites (Q), wait (T) and mouse wheel must not open menus while HERO
                // uses those inputs. Temporarily route only Skyrim menu keys, then restore
                // every link before any other input sink sees this shared event list.
                InputRoute route(events,[](auto e) {
                    if (ToggleEvent(e)) return false;
                    if (!hero) return true;
                    auto b=e->AsButtonEvent(); if (!b || b->GetDevice()!=RE::INPUT_DEVICE::kKeyboard) return false;
                    auto code=b->GetIDCode();
                    return !UsesBinding(settings,int(code)) && (code==0x01 || code==0x0F || code==0x29 || code==0x17 || code==0x24 || code==0x32 || code==0x3F || code==0x43 || int(code)==SettingsMenu::ReservedKey());
                });
                return original(controls,&route.head,source);
            }
            static inline REL::Relocation<decltype(Thunk)> original;
        };
        void RequestTestWorld() {
            if (!selfTest || testWorldRequested) return;
            testWorldRequested=true;
            SKSE::GetTaskInterface()->AddTask([] {
                SKSE::log::info("HERO gameplay check: requesting isolated Riverwood world; build={}",LIBAOTTG_BUILD_ID);
                auto factory=RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
                if (auto script=factory ? factory->Create() : nullptr) { script->SetCommand("coc Riverwood"); script->CompileAndRun(RE::PlayerCharacter::GetSingleton()); delete script; }
            });
        }
        class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (e && e->opening) {
                    ClearInput();
                    if (e->menuName==RE::LoadingMenu::MENU_NAME) SuspendWorld("loading menu");
                    if (hero && (e->menuName==RE::InventoryMenu::MENU_NAME || e->menuName==RE::MagicMenu::MENU_NAME || e->menuName==RE::TweenMenu::MENU_NAME)) {
                        menuPaused=true; Render::SetHudVisible(false);
                        SKSE::log::info("HERO menu retained; menu={}; camera={}; build={}",e->menuName.c_str(),CameraId(),LIBAOTTG_BUILD_ID);
                        if (doorCheck && testMenuPhase==1 && e->menuName==(testMenuIndex==0 ? RE::InventoryMenu::MENU_NAME : RE::MagicMenu::MENU_NAME)) {
                            testMenuOpened=true; testMenuDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                            testDoorGas=hero->Motor->Gas; testDoorBlade=hero->Motor->Blade; testDoorCooldown=hero->Motor->SkillCooldownRemaining();
                        }
                    }
                    if (e->menuName==RE::MainMenu::MENU_NAME) RequestTestWorld();
                    if (selfTest && e->menuName==RE::MessageBoxMenu::MENU_NAME) SKSE::GetTaskInterface()->AddTask([] {
                        auto data=RE::MessageBoxMenu::GetCurrentMessageBoxData();
                        if (!data || std::string_view(data->bodyText.c_str()).find("Enable Survival Mode?")==std::string_view::npos) return;
                        for (unsigned i=0;i<data->buttonText.size();++i) if (std::string_view(data->buttonText[i].c_str())=="No") {
                            SKSE::log::info("HERO gameplay check: declined Survival Mode in unsaved test world");
                            RE::MessageBoxMenu::SelectOption(int(i)); return;
                        }
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        } menuSink;
    }
    void Exit(const char* reason,bool automatic) {
        if (!hero) return;
        static std::string suppressedReason;
        if (automatic && !settings.autoRestore) {
            if (suppressedReason!=reason) SKSE::log::info("HERO automatic restore suppressed; reason={}; use the toggle key to restore; build={}",reason,LIBAOTTG_BUILD_ID);
            suppressedReason=reason; return;
        }
        suppressedReason.clear();
        if (automatic && settings.restoreDiagnostics) {
            SKSE::log::warn("HERO automatic restoration diagnostic; reason={}; build={}; historySamples={}",reason,LIBAOTTG_BUILD_ID,restoreHistory.size());
            for (const auto& sample:restoreHistory) SKSE::log::info("HERO restore history: {}",sample);
            SKSE::log::warn("HERO restore trigger: {}",RestoreSnapshot());
            if (auto ui=RE::UI::GetSingleton()) for (const auto& menu:ui->menuStack)
                if (menu) SKSE::log::info("HERO restore menu flags: {}",static_cast<unsigned>(menu->menuFlags.underlying()));
        }
        restoreHistory.clear();
        UpdateCrosshair(savedCrosshair);
        appliedCrosshair.reset();
        if(vanillaCrosshair) {
            SKSE::log::info("HERO crosshair restored; expected={}; actual={}; build={}",savedCrosshair,vanillaCrosshair->GetBool(),LIBAOTTG_BUILD_ID);
            vanillaCrosshair=nullptr;
        }
        for (auto& sound:heroSounds) { if (sound.IsValid()) sound.Stop(); sound=RE::BSSoundHandle{}; }
        nextSound=0; soundPaths.fill(nullptr); soundReported.fill(false); smoke.Reset(); Render::Publish(nullptr); world.ResetCombat(); damageHud.Reset(); killInfo.Clear();
        auto p=RE::PlayerCharacter::GetSingleton();
        if (p && p->GetCharController()==savedController) { savedController->gravity=savedGravity; savedController->SetLinearVelocityImpl(RE::hkVector4(0,0,0,0)); savedController->fallTime=0; savedController->fallStartHeight=p->GetPosition().z; }
        for (auto& mesh:hidden) if (mesh) mesh->SetAppCulled(false);
        hidden.clear(); playerRoot.reset();
        if (auto pc=RE::PlayerCamera::GetSingleton()) { pc->GetRuntimeData2().worldFOV=savedFov; if (savedCamera) pc->SetState(savedCamera.get()); pc->GetRuntimeData2().allowAutoVanityMode=savedAutoVanity;
            SKSE::log::info("HERO auto-vanity restored; expected={}; actual={}; build={}",savedAutoVanity,pc->GetRuntimeData2().allowAutoVanityMode,LIBAOTTG_BUILD_ID); }
        savedCamera.reset(); savedController=nullptr; hero.reset(); camera.reset(); suspended=menuPaused=false; expectedDoorCell=0; ClearInput();
        SettingsMenu::PublishStatus(false,configuredScale);
        SKSE::log::info("HERO disabled; restored player geometry/gravity/camera; reason={}; build={}",reason,LIBAOTTG_BUILD_ID);
        Notify("Skyrim character restored.");
    }
    bool Initialize() {
        try {
            auto ini=std::filesystem::absolute(L"Data/SKSE/Plugins/AottgInSkyrim.ini");
            settings=Settings::Load(ini);
            const auto reserved=FrameworkToggleBinding(std::filesystem::absolute(L"Data/SKSE/Plugins/SKSEMenuFramework.ini"));
            // A hand edited or legacy INI must never steal Escape / console / menu access.
            for (size_t i=0;i<ActionCount;++i) {
                auto& b=settings.bindings[i];
                if (b>=0 && (b==1 || b==15 || b==41 || b==reserved)) {
                    SKSE::log::warn("HERO reserved binding disabled: {}",ActionNames[i]); b=-1;
                    if (i==0) for (int fallback:{66,65,68,88}) if (fallback!=reserved) { b=fallback; break; }
                }
            }
            RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESDeathEvent>(&world);
            RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESSpellCastEvent>(&shoutSink);
            SettingsMenu::Register(settings); world.scoreMultiplier=settings.damageMultiplier; world.bladeHitboxMultiplier=settings.bladeHitboxMultiplier;
            selfTest=GetPrivateProfileIntW(L"Development",L"GameplayCheck",0,ini.c_str())!=0;
            doorCheck=selfTest && GetPrivateProfileIntW(L"Development",L"DoorCheck",0,ini.c_str())!=0;
            hudComparison=selfTest && GetPrivateProfileIntW(L"Development",L"HudComparison",0,ini.c_str())!=0;
            character.Load(std::filesystem::path(assetDirectory)/L"hero.bin"); Render::SetAssets(character,assetDirectory); Render::Prepare(); loaded=true;
            RE::UI::GetSingleton()->AddEventSink(&menuSink);
            if (RE::UI::GetSingleton()->IsMenuOpen(RE::MainMenu::MENU_NAME)) RequestTestWorld();
            SKSE::log::info("HERO host initialized; nodes={}; meshes={}; clips={}; gameplayCheck={}; autoRestore={}; build={}",character.nodes.size(),character.primitives.size(),character.clips.size(),selfTest,settings.autoRestore,LIBAOTTG_BUILD_ID); return true;
        } catch (const std::exception& e) { SKSE::log::error("HERO host initialization failed: {}",e.what()); return false; }
    }
    void SelectSkill(size_t index) {
        if (index>=skillIds.size()) return;
        currentSkill=index;
        if (hero) ApplySkill(*hero->Motor->Config());
        SettingsMenu::PublishSkill(index);
        if (!(hero && settings.heroHud)) {
            const auto message=std::string("AoTTG skill: ")+skillIds[currentSkill]; Notify(message.c_str());
        }
        SKSE::log::info("HERO skill switched; skill={}; build={}",skillIds[currentSkill],LIBAOTTG_BUILD_ID);
    }
    void ApplySettings(const Settings& value) {
        // Called only on Skyrim's game thread; the UI exchanges immutable settings copies.
        settings=value; if (!settings.restoreDiagnostics) restoreHistory.clear(); world.scoreMultiplier=settings.damageMultiplier; world.bladeHitboxMultiplier=settings.bladeHitboxMultiplier;
        if (camera) *camera->Config=settings.CameraConfig();
        if (hero) { hero->Combat=settings.combat; hero->ProceduralPose=settings.proceduralPose; UpdateCrosshair(settings.crosshair ? false : savedCrosshair); }
        ClearInput(); toggle=activateTarget=false;
    }
    void SetAutomaticRestore(bool enabled) {
        settings.autoRestore=enabled;
        SKSE::log::info("HERO automatic restore setting applied immediately; enabled={}; build={}",enabled,LIBAOTTG_BUILD_ID);
    }
    bool Install() {
        // Resolve and validate every site before modifying the executable or vtables.
        if (!SKSE::GetSerializationInterface()) { SKSE::log::error("HERO requires SKSE serialization interface"); return false; }
        if (!Render::ValidateHooks()) return false;
        const auto target=REL::ID(50784).address();
        const auto sites=Hooks::ExecutableCalls(target);
        if (sites.empty()) { SKSE::log::error("No supported Skyrim camera update call"); return false; }
        REL::Relocation<std::uintptr_t> player{RE::VTABLE_PlayerCharacter[0]},controls{RE::VTABLE_PlayerControls[0]},menuControls{RE::VTABLE_MenuControls[0]};
        // Slots follow the modern AE CommonLib Actor::Update and input event-sink ABI.
        // Address Library relocates the tables; it cannot describe future slot changes.
        const auto playerUpdate=reinterpret_cast<const std::uintptr_t*>(player.address())[0xAD];
        const auto playerInput=reinterpret_cast<const std::uintptr_t*>(controls.address())[1];
        const auto menuInput=reinterpret_cast<const std::uintptr_t*>(menuControls.address())[1];
        const auto executable=[](std::uintptr_t address) {
            MEMORY_BASIC_INFORMATION page{};
            if (!VirtualQuery(reinterpret_cast<const void*>(address),&page,sizeof(page)) || page.State!=MEM_COMMIT || (page.Protect & (PAGE_GUARD|PAGE_NOACCESS))) return false;
            return (page.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
        };
        if (!executable(playerUpdate) || !executable(playerInput) || !executable(menuInput)) {
            SKSE::log::error("HERO vtable preflight failed; required original functions are not executable");
            return false;
        }
        SKSE::log::info("HERO hook preflight passed; cameraCalls={}; playerUpdate={:X}; playerInput={:X}; menuInput={:X}; chainedExternalHook={}; build={}",
            sites.size(),playerUpdate,playerInput,menuInput,!Hooks::ExecutableAddress(playerUpdate)||!Hooks::ExecutableAddress(playerInput)||!Hooks::ExecutableAddress(menuInput),LIBAOTTG_BUILD_ID);
        SKSE::AllocTrampoline(2048);
        if (!Render::Install()) return false;
        for (auto site:sites) CameraHook::original=SKSE::GetTrampoline().write_call<5>(site,CameraHook::Thunk);
        PlayerHook::original=player.write_vfunc(0xAD,PlayerHook::Thunk); InputHook::original=controls.write_vfunc(1,InputHook::Thunk);
        MenuInputHook::original=menuControls.write_vfunc(1,MenuInputHook::Thunk);
        SKSE::GetSerializationInterface()->SetUniqueID('AOTG');
        SKSE::GetSerializationInterface()->SetRevertCallback([](SKSE::SerializationInterface*) { Exit("serialization revert"); });
        SKSE::log::info("HERO hooks installed; cameraCalls={}; build={}",sites.size(),LIBAOTTG_BUILD_ID); return true;
    }
    void OnMessage(SKSE::MessagingInterface::Message* message) {
        if (message->type==SKSE::MessagingInterface::kPreLoadGame || message->type==SKSE::MessagingInterface::kNewGame) Exit("load/new game");
    }
}
