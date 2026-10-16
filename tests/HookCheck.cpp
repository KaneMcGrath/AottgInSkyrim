// SPDX-License-Identifier: GPL-3.0-only
#include "CallDiscovery.h"
#include <iostream>
#include <stdexcept>

namespace {
    void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
    void Call(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uintptr_t base,std::uintptr_t target) {
        bytes[offset]=0xE8;
        const auto displacement=static_cast<std::int32_t>(target-(base+offset+5));
        std::memcpy(bytes.data()+offset+1,&displacement,4);
    }
}
int main(int argc,char** argv) {
    try {
        constexpr std::uintptr_t base=0x100000, target=base+0x2000;
        for (auto offset:{3u,17u,120u}) {
            std::vector<std::uint8_t> bytes(offset+6,0x90);
            Call(bytes,offset,base,target);
            const auto sites=Aottg::Hooks::DirectCalls(bytes,base,target);
            Require(sites.size()==1 && sites.front()==base+offset,"Shifted call was not discovered");
        }
        std::vector<std::uint8_t> bytes(24,0x90);
        bytes[0]=0x48; bytes[1]=0xB8; // mov rax, imm64: embedded E8 is data.
        Call(bytes,2,base,target);
        Call(bytes,12,base,target);
        auto sites=Aottg::Hooks::DirectCalls(bytes,base,target);
        Require(sites.size()==1 && sites.front()==base+12,"Immediate bytes mistaken for a call");
        Call(bytes,18,base,target);
        Require(Aottg::Hooks::DirectCalls(bytes,base,target).size()==2,"Ambiguous calls must remain visible to preflight");
        Require(Aottg::Hooks::DirectCalls(bytes,base,target+1).empty(),"Wrong target accepted");
        const std::array<std::uint8_t,4> truncated{0xE8,0,0,0};
        Require(Aottg::Hooks::DirectCalls(truncated,base,base+5).empty(),"Truncated call accepted");
        const std::array<std::uint8_t,3> invalid{0xF0,0x90,0x90};
        Require(!Aottg::Hooks::InstructionBoundary(invalid,2),"Invalid instruction was decoded through");
        std::vector<std::uint8_t> backwards(8,0x90);
        Call(backwards,2,base,base-100);
        Require(Aottg::Hooks::DirectCalls(backwards,base,base-100).size()==1,"Negative displacement failed");
        if (argc==2) {
            // Inspect the export without running DLL initialization or engine code.
            const auto module=LoadLibraryExA(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
            Require(module!=nullptr,"Cannot inspect plugin metadata");
            const auto symbol=GetProcAddress(module,"SKSEPlugin_Version");
            if (!symbol) { FreeLibrary(module); throw std::runtime_error("Plugin metadata export missing"); }
            const auto metadata=reinterpret_cast<const std::uint8_t*>(symbol);
            std::uint32_t structures=0,compatibility=0;
            // Offsets include PluginDeclaration's four-byte dataVersion header.
            std::memcpy(&structures,metadata+0x304,4);
            std::memcpy(&compatibility,metadata+0x308,4);
            bool whitelistUnused=true;
            for (std::size_t i=0;i<16;++i) {
                std::uint32_t version=0;
                std::memcpy(&version,metadata+0x30C+i*4,4);
                // CommonLib VersionNumber defaults to 1.0.0.0, rather than zero.
                // SKSE ignores this array in Address Library compatibility mode.
                whitelistUnused &= version==0 || version==0x01000000;
            }
            FreeLibrary(module);
            Require(structures==2,"Plugin must declare structure dependence with Address Library v5");
            Require(compatibility==5,"Plugin must declare Address Library and post-629 structures");
            Require(whitelistUnused,"Plugin contains an explicit runtime whitelist");
            std::cout << "HERO plugin metadata check passed: Address Library, modern AE structures, no runtime whitelist\n";
        }
        std::cout << "HERO hook discovery checks passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
