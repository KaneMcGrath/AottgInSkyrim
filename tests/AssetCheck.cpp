// SPDX-License-Identifier: GPL-3.0-only
#include "Character.h"
#include <fstream>
#include <iostream>
#include <cmath>

int main(int argc,char** argv) {
    try {
        if (argc<2) throw std::runtime_error("Usage: LibAottgAssetCheck hero.bin [oracle.bin]");
        Aottg::Character ch; ch.Load(argv[1]);
        if (ch.primitives.size()!=22 || ch.clips.size()!=68) throw std::runtime_error("Incomplete hero inventory");
        float maxPosition=0,maxNormal=0; unsigned poses=0;
        if (argc>=3) {
            std::ifstream f(argv[2],std::ios::binary);
            auto read=[&](auto& value) { if (!f.read(reinterpret_cast<char*>(&value),sizeof(value))) throw std::runtime_error("Invalid oracle"); };
            unsigned count; read(count);
            for (unsigned i=0;i<count;++i) {
                unsigned size; read(size); std::string clip(size,'\0'); f.read(clip.data(),size); float time; read(time);
                ch.Evaluate(clip,time,false); ++poses;
                float posePosition=0,poseNormal=0;
                for (const auto& m:ch.meshes) for (const auto& v:m.vertices) {
                    float values[6]; f.read(reinterpret_cast<char*>(values),sizeof(values));
                    float p[]={v.position.x,v.position.y,v.position.z},n[]={v.normal.x,v.normal.y,v.normal.z};
                    for (int k=0;k<3;++k) { posePosition=std::max(posePosition,std::abs(p[k]-values[k])); poseNormal=std::max(poseNormal,std::abs(n[k]-values[k+3])); }
                }
                maxPosition=std::max(maxPosition,posePosition); maxNormal=std::max(maxNormal,poseNormal);
                if (posePosition>0.00001f || poseNormal>0.00001f) std::cout<<clip<<" time="<<time<<" position="<<posePosition<<" normal="<<poseNormal<<'\n';
            }
            std::cout<<"C# comparison maxima: position="<<maxPosition<<" normal="<<maxNormal<<"\n";
            if (maxPosition>0.00001f || maxNormal>0.00001f) throw std::runtime_error("C# skinning comparison exceeded 0.00001 position / normal component tolerance");
        }
        // Every animation must produce finite deformations, including endpoints.
        for (const auto& [name,clip]:ch.clips) for (float fraction:{0.0f,0.37f,1.0f}) {
            ch.Evaluate(name,clip.duration*fraction);
            for (const auto& m:ch.meshes) for (const auto& v:m.vertices) {
                if (!std::isfinite(v.position.x+v.position.y+v.position.z+v.normal.x+v.normal.y+v.normal.z)) throw std::runtime_error("Nonfinite pose");
            }
        }
        std::cout<<"Native character check passed: 22 primitives, 68 clips, "<<poses<<" C# poses; maxPosition="<<maxPosition<<" maxNormal="<<maxNormal<<"\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
