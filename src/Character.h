// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <LibAottg/Core.hpp>
#include <DirectXMath.h>
#include <filesystem>

namespace Aottg
{
    struct Vertex { DirectX::XMFLOAT3 position, normal; DirectX::XMFLOAT2 uv; };
    struct LocalPose { DirectX::XMFLOAT3 translation; DirectX::XMFLOAT4 rotation; DirectX::XMFLOAT3 scale; };
    struct Node { std::string name; int parent; LocalPose rest; };
    struct Material { DirectX::XMFLOAT4 color; unsigned alphaMode; float cutoff; std::string texture; };
    struct SourceVertex { Vertex vertex; unsigned joints[4]; float weights[4]; };
    struct Primitive {
        std::string name; unsigned node; int material;
        std::vector<SourceVertex> source; std::vector<unsigned> indices, joints;
        std::vector<DirectX::XMFLOAT4X4> binds;
    };
    struct Clip { float duration; unsigned frames; std::vector<float> times; std::vector<LocalPose> poses; };
    struct DrawMesh { std::vector<Vertex> vertices; bool visible = true; };
    class Character final : public LibAottg::IHeroPresentation
    {
    public:
        void Load(const std::filesystem::path& file);
        void Reset();
        void Evaluate(const std::string& clip, float time, bool loop = false, float blendDelta = 0, float fade = 0, bool restart = false);
        void Sample(LibAottg::CharacterMotor* motor, LibAottg::HeroPose pose, LibAottg::CharacterInput input, float dt) override;
        LibAottg::Vec3 Head() const override;
        LibAottg::Vec3 HookOrigin(LibAottg::CharacterMotor*, LibAottg::HookSide) override;
        bool WeaponSegment(LibAottg::HookSide, bool, LibAottg::Vec3&, LibAottg::Vec3&) override;
        LibAottg::Vec3 Point(int node) const;
        std::vector<Node> nodes;
        std::vector<unsigned> order;
        std::vector<Primitive> primitives;
        std::vector<Material> materials;
        std::unordered_map<std::string, Clip> clips;
        std::vector<DrawMesh> meshes;
        std::vector<DirectX::XMFLOAT4X4> world;
        LibAottg::HeroPose pose{};
        float scale = 70, metersPerUnit = 1;
        float radius = 0.5f, height = 1.7f;
    private:
        std::vector<LocalPose> locals, fadeSource;
        std::string current;
        float fadeTime = 0, fadeDuration = 0;
        float sampledTime = -1;
        int revision = -1, head = -1, hooks[2]{-1,-1}, bladeStart[2]{-1,-1}, bladeEnd[2]{-1,-1};
    };
}
