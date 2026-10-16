// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Character.h"
#include <memory>
#include <LibAottg/HudPackage.hpp>

namespace Aottg::Render {
    struct Mesh { std::vector<Vertex> vertices; int material; };
    struct Frame { std::vector<Mesh> meshes; std::vector<Vertex> ropes; std::vector<LibAottg::HeroSmokeParticle> smoke; float ropeThicknessMultiplier=3; LibAottg::HeroHud heroHud; bool resourceHud=true; LibAottg::HeroDamageHud damage; LibAottg::HeroKillInfo kills; bool damageHud=true, killInfo=true, hudComparison=false; };
    bool ValidateHooks();
    bool Install();
    void SetAssets(const Character& character,const std::filesystem::path& directory);
    void Prepare();
    void SetHudVisible(bool visible);
    void Publish(std::shared_ptr<const Frame> frame);
    bool Ready();
    unsigned Draws();
    std::string Failure();
}
