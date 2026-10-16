// SPDX-License-Identifier: GPL-3.0-only
#include "SkyrimWorld.h"
#include <SKSE/SKSE.h>

// CommonLib declares this Havok interface but supplies no bodies for its trivial
// destructor/reset. Our stack collector owns no Havok allocation or engine vtable.
namespace RE {
    hkpCdPointCollector::~hkpCdPointCollector() = default;
    void hkpCdPointCollector::Reset() { earlyOutDistance=std::numeric_limits<float>::max(); }
}

namespace Aottg {
    namespace {
        RE::NiPoint3 Vector(const RE::hkVector4& v) { alignas(16) float p[4]; _mm_store_ps(p,v.quad); return {p[0],p[1],p[2]}; }
        const RE::hkpCdBody* Root(const RE::hkpCdBody* p) { while (p && p->parent) p=p->parent; return p; }
        RE::bhkWorld* World() {
            auto p=RE::PlayerCharacter::GetSingleton(); auto cell=p ? p->GetParentCell() : nullptr; return cell ? cell->GetbhkWorld() : nullptr;
        }
        RE::hkpShapePhantom* Phantom() {
            auto p=RE::PlayerCharacter::GetSingleton(); auto c=p ? skyrim_cast<RE::bhkCharProxyController*>(p->GetCharController()) : nullptr;
            auto proxy=c ? c->GetCharacterProxy() : nullptr; return proxy ? proxy->shapePhantom : nullptr;
        }
        RE::TESObjectREFR* Reference(const RE::hkpCollidable* c) {
            if (!c) return nullptr;
            const auto type=c->broadPhaseHandle.type;
            if (type!=static_cast<int>(RE::hkpWorldObject::BroadPhaseType::kEntity) && type!=static_cast<int>(RE::hkpWorldObject::BroadPhaseType::kPhantom)) return nullptr;
            return RE::TESHavokUtilities::FindCollidableRef(*c);
        }
        // Skyrim's own name for a collision layer, e.g. TRIGGER(12) or ACTORZONE(22).
        std::string LayerName(std::size_t layer) {
            auto table=RE::bhkCollisionFilter::GetSingleton();
            if (!table || layer>=std::size(table->collisionLayerNames) || table->collisionLayerNames[layer].empty())
                return std::to_string(layer);
            return table->collisionLayerNames[layer].c_str();
        }
        std::string LayerList(std::uint64_t mask) {
            std::string text;
            for (std::size_t layer=0;layer<64;++layer) {
                if ((mask>>layer&1)==0) continue;
                if (!text.empty()) text+=", ";
                text+=LayerName(layer);
            }
            return text.empty() ? "none" : text;
        }
    }
    // Skyrim flags the layers it uses for trigger volumes and sensors in bhkCollisionFilter
    // (BGSCollisionLayer GNAM). Shapes on those layers report whoever passes through them; they are
    // not solid, so characters and physics objects walk straight through them. The vanilla character
    // controller gathers its own contacts and drops these volumes, which is why quest triggers,
    // actor zones, projectile zones, trap triggers and spell-effect volumes never stop a Skyrim
    // actor. HERO sweeps the borrowed player capsule directly, so every reported contact used to
    // count as a wall and each of those volumes stopped the character. Contacts with them are now
    // ignored, while everything else still follows Skyrim's collide layer matrix: the borrowed
    // capsule keeps the player's collision filter, so terrain, statics, props, doors and other
    // characters behave exactly as they do for the normal player.
    bool SkyrimWorld::Blocks(const RE::hkpCollidable& candidate) const {
        const auto info=candidate.broadPhaseHandle.collisionFilterInfo;
        if (info.QNoCollision()) return false;
        auto table=RE::bhkCollisionFilter::GetSingleton();
        if (!table) return true;
        const auto layer=static_cast<std::size_t>(info.GetCollisionLayer());
        if (layer>=std::size(table->layerBitfields)) return true;
        return ((table->triggerField|table->sensorField)>>layer&1)==0;
    }
    void SkyrimWorld::NoteIgnored(const RE::hkpCollidable& candidate,const char* source) {
        ++filtered;
        (*source=='s' ? filteredSweeps : filteredProbes)++;
        const auto layer=static_cast<std::size_t>(candidate.broadPhaseHandle.collisionFilterInfo.GetCollisionLayer());
        bool first=layer<ignoredLayers.size();
        if (first) first=ignoredLayers[layer]++==0;
        if (!filterReported) {
            // Record the rule in effect once per HERO use, with the layer names from Skyrim's own
            // collision filter table so a log always shows which volumes are stepped over.
            filterReported=true;
            auto table=RE::bhkCollisionFilter::GetSingleton();
            auto phantom=Phantom();
            const auto capsule=phantom ? LayerName(static_cast<std::size_t>(phantom->collidable.broadPhaseHandle.collisionFilterInfo.GetCollisionLayer())) : "unavailable";
            SKSE::log::info("HERO collision filter: capsule layer={}; Skyrim trigger volumes=[{}]; sensors=[{}]; they never block the HERO; build={}",
                capsule,table ? LayerList(table->triggerField) : "unavailable",table ? LayerList(table->sensorField) : "unavailable",LIBAOTTG_BUILD_ID);
        }
        if (!first) return;
        auto ref=Reference(&candidate);
        SKSE::log::info("HERO collision: {} stepping over {} layer={} {} (ref={:08X}); build={}",source,LayerName(layer),static_cast<int>(layer),
            candidate.broadPhaseHandle.type==static_cast<int>(RE::hkpWorldObject::BroadPhaseType::kPhantom)?"volume":"shape",
            ref ? ref->GetFormID() : 0,LIBAOTTG_BUILD_ID);
    }
    bool SkyrimWorld::Ray(RE::NiPoint3 from,RE::NiPoint3 to,RE::NiPoint3& point,RE::NiPoint3& normal,RE::TESObjectREFR*& ref) {
        auto bhk=World(); if (!bhk || !bhk->GetWorld1()) return false;
        const auto k=RE::bhkWorld::GetWorldScale();
        const auto initial=to-from; const auto length=initial.Length();
        if (length<=0) return false;
        const auto direction=initial/length;
        const auto step=direction*(0.01f/k);  // 0.01 Havok units, just past a surface we stepped over
        auto player=RE::PlayerCharacter::GetSingleton();
        // Probes, hooks and blade traces see the world the way a character does: a volume that
        // cannot block one is stepped over instead of ending the query.
        for (unsigned attempt=0;attempt<8;++attempt) {
            const auto span=to-from; if (span.Length()<=0) return false;
            RE::hkpWorldRayCastInput input{};
            input.from=RE::hkVector4(from*k); input.to=RE::hkVector4(to*k);
            input.enableShapeCollectionFilter=true; input.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
            if (auto controller=player ? player->GetCharController() : nullptr) input.filterInfo.SetSystemGroup(controller->collisionFilterGroup);
            RE::hkpWorldRayCastOutput out{};
            {
                RE::BSReadLockGuard lock(bhk->worldLock);
                ++rayQueries; bhk->GetWorld1()->CastRay(input,out);
            }
            if (!out.HasHit()) return false;
            point=from+span*out.hitFraction; normal=Vector(out.normal); ref=Reference(out.rootCollidable);
            if (ref==player) return false;
            if (!out.rootCollidable || Blocks(*out.rootCollidable)) return true;
            // The volume cannot stop a character, so continue from just past the surface it
            // reported instead of treating it as a wall. Probes, hooks and blade traces then see the
            // solid geometry behind it exactly as a character or physics object would.
            NoteIgnored(*out.rootCollidable,"probe");
            from=point+step;
            if ((to-from).Dot(direction)<=0) return false;
        }
        ++rayExhausted;
        return false;
    }
    bool SkyrimWorld::Raycast(LibAottg::Vec3 origin,LibAottg::Vec3 dir,float distance,LibAottg::HeroQuery,LibAottg::HookHit& hit) {
        RE::NiPoint3 point,normal; RE::TESObjectREFR* ref=nullptr;
        if (!Ray(ToSkyrim(origin),ToSkyrim(origin+dir.Normalized()*distance),point,normal,ref)) return false;
        hit.Point=ToHero(point); hit.TargetId=ref ? int(ref->GetFormID()) : 0;
        if (ref && ref->Get3D()) {
            const auto& xf=ref->Get3D()->world;
            auto local=xf.rotate.Transpose()*(point-xf.translate)/xf.scale;
            hit.LocalPoint=ToHero(local);
        } else hit.LocalPoint=hit.Point;
        return true;
    }
    bool SkyrimWorld::ResolveHookAnchor(int id,LibAottg::Vec3 local,LibAottg::Vec3& result) {
        if (!id) { result=local; return true; }
        auto ref=RE::TESForm::LookupByID<RE::TESObjectREFR>(static_cast<RE::FormID>(id));
        if (!ref || ref->IsDeleted() || ref->IsDisabled() || !ref->Get3D()) return false;
        const auto& xf=ref->Get3D()->world; result=ToHero(xf.translate+xf.rotate*(ToSkyrim(local)*xf.scale)); return true;
    }
    LibAottg::CharacterContacts SkyrimWorld::GetContacts(LibAottg::CharacterMotor* motor,LibAottg::HeroPose pose) {
        LibAottg::CharacterContacts contacts{}; RE::NiPoint3 point,normal; RE::TESObjectREFR* ref=nullptr; const auto scale=motor->Scale;
        auto start=motor->Position()+LibAottg::Vec3::Up()*(0.1f*scale);
        contacts.Grounded=Ray(ToSkyrim(start),ToSkyrim(start-LibAottg::Vec3::Up()*(0.3f*scale)),point,normal,ref) && normal.z>0.65f;
        contacts.GroundNormal=contacts.Grounded ? ToHero(normal) : LibAottg::Vec3::Up();
        start=motor->Position()+pose.Up()*scale;
        contacts.FrontWall=Ray(ToSkyrim(start),ToSkyrim(start+pose.Forward()*scale),point,normal,ref);
        start=motor->Position()+pose.Up()*(3*scale);
        contacts.UpperFrontClear=!Ray(ToSkyrim(start),ToSkyrim(start+pose.Forward()*(1.2f*scale)),point,normal,ref);
        return contacts;
    }
    // Contact-point collector for one capsule sweep. A nested class so it can ask its owner which
    // collidables are allowed to stop the HERO.
    class SkyrimWorld::Collector final : public RE::hkpCdPointCollector {
    public:
        SkyrimWorld* owner=nullptr;
        const RE::hkpCdBody* ignore=nullptr;
        RE::NiPoint3 motion{}, normal{};
        float fraction=1;
        bool penetration=false;
        Collector() { earlyOutDistance=std::numeric_limits<float>::max(); }
        void AddCdPoint(const RE::hkpCdPoint& p) override {
            const auto* body=Root(p.cdBodyB);
            if (body==ignore) return;
            if (owner && body) {
                const auto* candidate=static_cast<const RE::hkpCollidable*>(body);
                if (!owner->Blocks(*candidate)) { owner->NoteIgnored(*candidate,"sweep"); return; }
            }
            alignas(16) float n[4]; _mm_store_ps(n,p.contact.separatingNormal.quad);
            RE::NiPoint3 v{n[0],n[1],n[2]};
            if (!std::isfinite(n[3]) || v.Dot(motion)>=-0.000001f) return;
            if (n[3]<fraction) { fraction=n[3]; normal=v; earlyOutDistance=n[3]; }
        }
    };
    LibAottg::CollisionResult SkyrimWorld::Move(LibAottg::CharacterMotor* motor,LibAottg::Vec3 start,LibAottg::HeroPose proposed,float) {
        LibAottg::CollisionResult result{start,motor->Velocity(),false,false};
        auto bhk=World(); auto phantom=Phantom(); auto player=RE::PlayerCharacter::GetSingleton();
        if (!bhk || !phantom || !player) { result.Velocity={}; return result; }
        const float k=RE::bhkWorld::GetWorldScale();
        auto pos=ToSkyrim(start), motion=ToSkyrim(proposed.Position-start), velocity=ToSkyrim(result.Velocity);
        RE::BSReadLockGuard lock(bhk->worldLock);
        // A borrowed copy retains Skyrim's capsule shape and collision filter. Only its transform
        // is local: sweeps do not move/resize the live phantom or modify the physics world.
        RE::hkpCollidable shape=phantom->collidable;
        auto transform=phantom->motionState.transform;
        const auto offset=Vector(transform.translation)/k-player->GetPosition();
        shape.motion=&transform;
        for (unsigned pass=0;pass<6 && motion.Length()>0.01f;++pass) {
            transform.translation=RE::hkVector4((pos+offset)*k);
            RE::hkpLinearCastInput input{}; input.to=RE::hkVector4((pos+offset+motion)*k);
            input.maxExtraPenetration=0; input.startPointTolerance=0.01f;
            Collector cast; cast.ignore=&phantom->collidable; cast.motion=motion; cast.owner=this;
            ++sweeps; bhk->GetWorld1()->LinearCast(&shape,input,cast);
            if (cast.fraction>=1) { pos+=motion; break; }
            result.Collided=true;
            float t=std::max(0.0f,cast.fraction-0.5f/std::max(1.0f,motion.Length()));
            pos+=motion*t; motion=motion*(1-t);
            float inward=motion.Dot(cast.normal); if (inward<0) motion-=cast.normal*inward;
            inward=velocity.Dot(cast.normal); if (inward<0) velocity-=cast.normal*inward;
            if (cast.normal.z>0.65f) result.Grounded=true;
        }
        result.Position=ToHero(pos); result.Velocity=ToHero(velocity); return result;
    }
    void SkyrimWorld::CollectWeaponHits(LibAottg::HeroWeaponSweep sweep,std::vector<LibAottg::HeroHit>& found) {
        // Scale collision reach around each hilt without changing rig geometry or damage.
        if (!sweep.Gun) {
            sweep.End=sweep.Start+(sweep.End-sweep.Start)*bladeHitboxMultiplier;
            sweep.PreviousEnd=sweep.PreviousStart+(sweep.PreviousEnd-sweep.PreviousStart)*bladeHitboxMultiplier;
        }
        // Trace along both current/previous blade edges and their motion. Geometry blocks hits.
        const LibAottg::Vec3 a[]={sweep.Start,sweep.PreviousStart,sweep.PreviousEnd,sweep.PreviousStart};
        const LibAottg::Vec3 b[]={sweep.End,sweep.Start,sweep.End,sweep.PreviousEnd};
        const float radius=sweep.Gun ? 0 : sweep.Radius*bladeHitboxMultiplier;
        // Sample the blade volume, including its center. Every sample remains occlusion tested.
        for (int sample=0;sample<7;++sample) for (int i=0;i<4;++i) {
            LibAottg::Vec3 offset{};
            if (sample>0) {
                const float value=sample%2 ? radius : -radius;
                if (sample<=2) offset.X=value; else if (sample<=4) offset.Y=value; else offset.Z=value;
            }
            RE::NiPoint3 point,normal; RE::TESObjectREFR* ref=nullptr;
            if (Ray(ToSkyrim(a[i]+offset),ToSkyrim(b[i]+offset),point,normal,ref) && ref) {
                auto actor=ref->As<RE::Actor>();
                if (!actor || actor==RE::PlayerCharacter::GetSingleton() || actor->IsDead()) continue;
                RE::NiPoint3 velocity{}; actor->GetLinearVelocity(velocity);
                LibAottg::HeroHit hit{int(ref->GetFormID()),ToHero(point),sweep.Side,sweep.Gun,0};
                hit.TargetVelocity=ToHero(velocity); hit.ScoreMultiplier=scoreMultiplier; found.push_back(hit);
            }
        }
    }
    void SkyrimWorld::OnHeroHit(LibAottg::HeroHit hit) {
        auto actor=RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(hit.TargetId)); auto player=RE::PlayerCharacter::GetSingleton();
        if (!actor || actor==player || actor->IsDead()) return;
        const float healthBefore=actor->GetActorValue(RE::ActorValue::kHealth);
        if (healthBefore<=0) return; // A dying actor cannot award another kill.
        {
            std::scoped_lock lock(noticeMutex);
            reportedKills.erase(actor->GetFormID()); // Positive health begins a new life.
            pendingDamage[actor->GetFormID()]=hit.Damage;
        }
        const bool damageReturn=actor->DoDamage(float(hit.Damage),player,true);
        actor->StartCombat(player); ++hits;
        const float healthAfter=actor->GetActorValue(RE::ActorValue::kHealth);
        const bool applied=healthAfter<healthBefore;
        {
            std::scoped_lock lock(noticeMutex);
            if (applied) notices.push_back({false,hit.Damage,{},{}});
            // Show the feed in this hit's frame instead of waiting for the death animation.
            // Essential actors may reach zero health while entering bleedout.
            if (applied && healthAfter<=0 && !actor->IsEssential())
                QueueKill(actor,hit.Damage,"lethal hit");
            if (!applied || healthAfter>0 || reportedKills.contains(actor->GetFormID()))
                pendingDamage.erase(actor->GetFormID());
        }
        // Skyrim.esm WPNImpactBladeVsFleshSD: play the actual vanilla blade-on-flesh
        // descriptor directly. PlayImpactEffect may spawn a visual impact while selecting
        // no sound for the actor's race/material, so it is not sufficient for HERO hits.
        constexpr RE::FormID bladeHitSoundID=0x0003C74D;
        auto descriptor=RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(bladeHitSoundID);
        auto audio=RE::BSAudioManager::GetSingleton();
        RE::BSSoundHandle sound;
        const bool soundReady=audio && descriptor && audio->GetSoundHandle(sound,descriptor);
        if (soundReady) {
            if (auto node=actor->Get3D()) sound.SetObjectToFollow(node);
            else sound.SetPosition(actor->GetPosition());
        }
        const bool soundPlayed=soundReady && sound.Play();
        SKSE::log::info("HERO hit actor={:08X}; damage={}; applied={}; damageReturn={}; health={}->{}; sound={}; descriptor={}",actor->GetFormID(),hit.Damage,applied,damageReturn,healthBefore,healthAfter,soundPlayed,descriptor!=nullptr);
    }
    std::vector<SkyrimWorld::Notice> SkyrimWorld::DrainNotices() {
        std::scoped_lock lock(noticeMutex); std::vector<Notice> result; result.swap(notices); return result;
    }
    void SkyrimWorld::ResetCombat() { std::scoped_lock lock(noticeMutex); notices.clear(); pendingDamage.clear(); reportedKills.clear(); }
    void SkyrimWorld::QueueKill(RE::Actor* actor,int damage,const char* trigger) {
        if (!reportedKills.insert(actor->GetFormID()).second) return;
        auto player=RE::PlayerCharacter::GetSingleton();
        const char* killer=player->GetDisplayFullName(); const char* victim=actor->GetDisplayFullName();
        notices.push_back({true,damage,killer?killer:"",victim?victim:""});
        SKSE::log::info("HERO kill reported; actor={:08X}; trigger={}; killer={}; victim={}; damage={}",
            actor->GetFormID(),trigger,killer?killer:"",victim?victim:"",damage);
    }
    RE::BSEventNotifyControl SkyrimWorld::ProcessEvent(const RE::TESDeathEvent* event,RE::BSTEventSource<RE::TESDeathEvent>*) {
        if (!event || !event->dead || !event->actorDying) return RE::BSEventNotifyControl::kContinue;
        std::scoped_lock lock(noticeMutex);
        auto it=pendingDamage.find(event->actorDying->GetFormID());
        if (it==pendingDamage.end()) return RE::BSEventNotifyControl::kContinue;
        if (event->actorKiller.get()==RE::PlayerCharacter::GetSingleton())
            if (auto actor=event->actorDying->As<RE::Actor>()) QueueKill(actor,it->second,"death event");
        pendingDamage.erase(it); return RE::BSEventNotifyControl::kContinue;
    }

}
