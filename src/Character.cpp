// SPDX-License-Identifier: GPL-3.0-only
#include "Character.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Aottg
{
    using namespace DirectX;
    namespace {
        class Reader {
            std::ifstream file;
        public:
            explicit Reader(const std::filesystem::path& path) : file(path, std::ios::binary) {
                if (!file || std::filesystem::file_size(path) > 128 * 1024 * 1024) throw std::runtime_error("Invalid hero file");
            }
            void Read(void* out, size_t bytes) {
                if (!file.read(static_cast<char*>(out), static_cast<std::streamsize>(bytes))) throw std::runtime_error("Truncated hero package");
            }
            template<class T> T Value() { T value{}; Read(&value, sizeof(value)); return value; }
            unsigned Count(unsigned max) { auto v = Value<unsigned>(); if (v > max) throw std::runtime_error("Hero count exceeds bounds"); return v; }
            std::string String() { std::string s(Count(4096), '\0'); Read(s.data(), s.size()); return s; }
            template<class T> std::vector<T> Array(unsigned count) { std::vector<T> v(count); Read(v.data(), v.size()*sizeof(T)); return v; }
            void End() { if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected hero trailing data"); }
        };
        LocalPose Blend(const LocalPose& a, const LocalPose& b, float t) {
            LocalPose r;
            XMStoreFloat3(&r.translation, XMVectorLerp(XMLoadFloat3(&a.translation), XMLoadFloat3(&b.translation), t));
            XMStoreFloat3(&r.scale, XMVectorLerp(XMLoadFloat3(&a.scale), XMLoadFloat3(&b.scale), t));
            XMStoreFloat4(&r.rotation, XMQuaternionNormalize(XMQuaternionSlerp(XMLoadFloat4(&a.rotation), XMLoadFloat4(&b.rotation), t)));
            return r;
        }
    }
    void Character::Load(const std::filesystem::path& path) {
        Reader r(path); char magic[8]; r.Read(magic,8);
        if (std::memcmp(magic,"AOTRIG02",8)) throw std::runtime_error("Unsupported hero package");
        auto nn=r.Count(4096), np=r.Count(256), nm=r.Count(256), nc=r.Count(256);
        if (!nn || !np || !nc) throw std::runtime_error("Empty hero package");
        metersPerUnit=r.Value<float>();
        if (!std::isfinite(metersPerUnit) || metersPerUnit<=0) throw std::runtime_error("Invalid asset scale");
        nodes.clear(); primitives.clear(); materials.clear(); clips.clear();
        for (unsigned i=0;i<nn;++i) {
            Node n{r.String(),r.Value<int>(),r.Value<LocalPose>()};
            if (n.parent<-1 || n.parent>=int(nn) || n.parent==int(i)) throw std::runtime_error("Invalid rig parent");
            nodes.push_back(n);
        }
        order=r.Array<unsigned>(nn); std::vector<bool> visited(nn);
        for (auto i:order) {
            if (i>=nn || visited[i] || (nodes[i].parent>=0 && !visited[nodes[i].parent])) throw std::runtime_error("Invalid rig order");
            visited[i]=true;
        }
        for (unsigned i=0;i<nm;++i) materials.push_back({r.Value<XMFLOAT4>(),r.Value<unsigned>(),r.Value<float>(),r.String()});
        for (unsigned i=0;i<np;++i) {
            Primitive p; p.name=r.String(); p.node=r.Value<unsigned>(); p.material=r.Value<int>();
            auto nv=r.Count(100000), ni=r.Count(300000), nj=r.Count(nn);
            if (!nv || ni%3 || p.node>=nn || p.material<-1 || p.material>=int(nm)) throw std::runtime_error("Invalid mesh");
            p.source=r.Array<SourceVertex>(nv); p.indices=r.Array<unsigned>(ni);
            for (auto idx:p.indices) if (idx>=nv) throw std::runtime_error("Invalid vertex index");
            for (unsigned j=0;j<nj;++j) {
                auto node=r.Value<unsigned>(); if (node>=nn) throw std::runtime_error("Invalid skin joint");
                p.joints.push_back(node); p.binds.push_back(r.Value<XMFLOAT4X4>());
            }
            for (const auto& v:p.source) {
                for (int k=0;k<4;++k) if (!std::isfinite(v.weights[k]) || (v.weights[k]!=0 && v.joints[k]>=nj)) throw std::runtime_error("Invalid skin weight");
            }
            primitives.push_back(std::move(p));
        }
        for (unsigned i=0;i<nc;++i) {
            auto name=r.String(); Clip c{r.Value<float>(),r.Count(20000),{}, {}};
            if (!std::isfinite(c.duration) || c.duration<=0 || c.frames<2 || size_t(c.frames)*nn>2000000) throw std::runtime_error("Invalid animation");
            c.times=r.Array<float>(c.frames);
            for (unsigned j=0;j<c.frames;++j) if (!std::isfinite(c.times[j]) || (j && c.times[j]<=c.times[j-1])) throw std::runtime_error("Invalid key times");
            c.poses=r.Array<LocalPose>(c.frames*nn);
            if (!clips.emplace(name,std::move(c)).second) throw std::runtime_error("Duplicate clip");
        }
        r.End(); locals.resize(nn); fadeSource.resize(nn); world.resize(nn); meshes.resize(np);
        for (unsigned i=0;i<nn;++i) {
            locals[i]=nodes[i].rest;
            const auto& n=nodes[i];
            if (n.name=="head") head=int(i);
            if (n.name=="hookRefL1") hooks[0]=int(i);
            if (n.name=="hookRefR1") hooks[1]=int(i);
            if (n.name=="bladeStart" || n.name=="bladeEnd") {
                int parent=n.parent; bool left=false;
                while (parent>=0) { left |= nodes[parent].name=="shoulder_L"; parent=nodes[parent].parent; }
                (n.name=="bladeStart" ? bladeStart : bladeEnd)[left?0:1]=int(i);
            }
        }
        for (unsigned i=0;i<np;++i) meshes[i].vertices.resize(primitives[i].source.size());
        current.clear(); revision=-1;
        Evaluate("stand",0);
    }
    void Character::Evaluate(const std::string& name, float time, bool loop, float dt, float fade, bool restart) {
        auto it=clips.find(name);
        if (it==clips.end()) throw std::runtime_error("Missing animation: "+name);
        if (current!=name || restart) { fadeSource=locals; fadeDuration=fade; fadeTime=0; current=name; }
        const auto& c=it->second;
        time=loop ? std::fmod(time,c.duration) : std::clamp(time,0.0f,c.duration);
        auto upper=std::upper_bound(c.times.begin(),c.times.end(),time);
        auto b=std::min(unsigned(upper-c.times.begin()),c.frames-1),a=b>0?b-1:0;
        float fraction=a==b?0:std::clamp((time-c.times[a])/(c.times[b]-c.times[a]),0.0f,1.0f);
        fadeTime+=dt; float blend=fadeDuration>0 ? std::min(1.0f,fadeTime/fadeDuration) : 1;
        for (auto i:order) {
            auto local=Blend(c.poses[size_t(a)*nodes.size()+i],c.poses[size_t(b)*nodes.size()+i],fraction);
            if (blend<1) local=Blend(fadeSource[i],local,blend);
            locals[i]=local;
            auto m=XMMatrixScalingFromVector(XMLoadFloat3(&local.scale))*XMMatrixRotationQuaternion(XMLoadFloat4(&local.rotation))*XMMatrixTranslationFromVector(XMLoadFloat3(&local.translation));
            if (nodes[i].parent>=0) m*=XMLoadFloat4x4(&world[nodes[i].parent]);
            XMStoreFloat4x4(&world[i],m);
        }
        for (size_t pi=0;pi<primitives.size();++pi) {
            const auto& p=primitives[pi]; auto& mesh=meshes[pi];
            std::vector<XMMATRIX> skin(p.joints.size()), normals(p.joints.size());
            for (size_t j=0;j<skin.size();++j) {
                skin[j]=XMLoadFloat4x4(&p.binds[j])*XMLoadFloat4x4(&world[p.joints[j]]);
                normals[j]=XMMatrixTranspose(XMMatrixInverse(nullptr,skin[j]));
            }
            auto node=XMLoadFloat4x4(&world[p.node]); auto normal=XMMatrixTranspose(XMMatrixInverse(nullptr,node));
            for (size_t vi=0;vi<p.source.size();++vi) {
                const auto& v=p.source[vi]; auto pos=XMLoadFloat3(&v.vertex.position), n=XMLoadFloat3(&v.vertex.normal);
                auto sum=XMVectorZero(), ns=XMVectorZero();
                if (skin.empty()) { sum=XMVector3TransformCoord(pos,node); ns=XMVector3TransformNormal(n,normal); }
                else for (int k=0;k<4;++k) if (v.weights[k]!=0) {
                    sum+=XMVector3TransformCoord(pos,skin[v.joints[k]])*v.weights[k];
                    ns+=XMVector3TransformNormal(n,normals[v.joints[k]])*v.weights[k];
                }
                XMStoreFloat3(&mesh.vertices[vi].position,sum);
                XMStoreFloat3(&mesh.vertices[vi].normal,XMVector3Normalize(ns));
                mesh.vertices[vi].uv=v.vertex.uv;
            }
        }
    }
    void Character::Reset() {
        current.clear(); revision=-1; sampledTime=-1;
        for (size_t i=0;i<nodes.size();++i) locals[i]=nodes[i].rest;
        Evaluate("stand",0);
    }
    void Character::Sample(LibAottg::CharacterMotor* motor, LibAottg::HeroPose root, LibAottg::CharacterInput, float dt) {
        pose=root; scale=motor->Scale; const auto& a=motor->Animation();
        if (sampledTime!=a.Time || revision!=a.Revision || dt>0) Evaluate(a.Clip,a.Time,a.Loop,dt,a.CrossFade,revision!=a.Revision);
        revision=a.Revision; sampledTime=a.Time;
        for (size_t i=0;i<primitives.size();++i) meshes[i].visible=primitives[i].name.find("character_blade")==std::string::npos || motor->BladesEquipped;
    }
    LibAottg::Vec3 Character::Point(int node) const {
        if (node<0) return pose.Position+LibAottg::Vec3::Up()*(1.5f*scale);
        const auto& m=world[node]; return pose.Position+pose.Rotate(LibAottg::Vec3(m._41,m._42,-m._43)*(scale/metersPerUnit));
    }
    LibAottg::Vec3 Character::Head() const { return Point(head); }
    LibAottg::Vec3 Character::HookOrigin(LibAottg::CharacterMotor*,LibAottg::HookSide side) { return Point(hooks[int(side)]); }
    bool Character::WeaponSegment(LibAottg::HookSide side,bool gun,LibAottg::Vec3& a,LibAottg::Vec3& b) {
        if (gun) return false;
        const int s=int(side); a=Point(bladeStart[s]); b=Point(bladeEnd[s]); return bladeStart[s]>=0 && bladeEnd[s]>=0;
    }
}
