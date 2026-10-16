// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <RE/Skyrim.h>
#include <LibAottg/Core.hpp>
#include <array>
#include <LibAottg/Hud.hpp>
#include <map>
#include <mutex>
#include <unordered_set>

namespace Aottg {
    inline RE::NiPoint3 ToSkyrim(LibAottg::Vec3 p) { return {p.X,p.Z,p.Y}; }
    inline LibAottg::Vec3 ToHero(RE::NiPoint3 p) { return {p.x,p.z,p.y}; }
    class SkyrimWorld final : public LibAottg::HeroWorld, public RE::BSTEventSink<RE::TESDeathEvent> {
    public:
        bool Raycast(LibAottg::Vec3,LibAottg::Vec3,float,LibAottg::HeroQuery,LibAottg::HookHit&) override;
        LibAottg::CharacterContacts GetContacts(LibAottg::CharacterMotor*,LibAottg::HeroPose) override;
        LibAottg::CollisionResult Move(LibAottg::CharacterMotor*,LibAottg::Vec3,LibAottg::HeroPose,float) override;
        bool ResolveHookAnchor(int,LibAottg::Vec3,LibAottg::Vec3&) override;
        void CollectWeaponHits(LibAottg::HeroWeaponSweep,std::vector<LibAottg::HeroHit>&) override;
        void OnHeroHit(LibAottg::HeroHit) override;
        float scoreMultiplier=1, bladeHitboxMultiplier=1;
        struct Notice { bool kill; int damage; std::string killer,victim; };
        std::vector<Notice> DrainNotices();
        void ResetCombat();
        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent*,RE::BSTEventSource<RE::TESDeathEvent>*) override;
        unsigned rayQueries=0, sweeps=0, hits=0, filtered=0, rayExhausted=0;
        unsigned filteredSweeps=0, filteredProbes=0;
    private:
        // Contact-point collector for one capsule sweep. Nested so it can ask the owner which
        // collidables are allowed to stop the HERO (see SkyrimWorld.cpp).
        class Collector;
        bool Ray(RE::NiPoint3,RE::NiPoint3,RE::NiPoint3&,RE::NiPoint3&,RE::TESObjectREFR*&);
        // Skyrim's own collision data decides what may hold a character or a physics object back:
        // trigger volumes, sensors and no-collision shapes only report, they never block.
        bool Blocks(const RE::hkpCollidable&) const;
        // Records a volume that was stepped over instead of treated as a wall, and counts it per
        // source: "sweep" for the capsule contact collector, "probe" for the ray queries.
        void NoteIgnored(const RE::hkpCollidable&,const char*);
        std::mutex noticeMutex;
        std::vector<Notice> notices;
        std::map<RE::FormID,int> pendingDamage;
        std::unordered_set<RE::FormID> reportedKills;
        // Caller holds noticeMutex; shared by lethal hits and confirmed death events.
        void QueueKill(RE::Actor*,int,const char*);
        bool filterReported=false;
        std::array<unsigned,64> ignoredLayers{};
    };
}
