// SPDX-License-Identifier: GPL-3.0-only
#include "CallDiscovery.h"
#include <REL/Relocation.h>
#include <Windows.h>

namespace Aottg::Hooks {
    bool ExecutableAddress(std::uintptr_t address) {
        const auto text=REL::Module::get().segment(REL::Segment::textx);
        return address>=text.address() && address-text.address()<text.size();
    }
    namespace {
        std::span<const std::uint8_t> FunctionBytes(std::uintptr_t address) {
            if (!ExecutableAddress(address)) return {};
            DWORD64 imageBase=0;
            const auto entry=RtlLookupFunctionEntry(address,&imageBase,nullptr);
            if (!entry || imageBase!=REL::Module::get().base() || entry->EndAddress<=entry->BeginAddress) return {};
            const auto begin=imageBase+entry->BeginAddress, end=imageBase+entry->EndAddress;
            if (!ExecutableAddress(begin) || !ExecutableAddress(end-1)) return {};
            return {reinterpret_cast<const std::uint8_t*>(begin),std::size_t(end-begin)};
        }
    }
    std::vector<std::uintptr_t> FunctionCalls(std::uintptr_t function,std::uintptr_t target) {
        const auto bytes=FunctionBytes(function);
        // Require the Address Library ID to identify the actual function entry.
        if (bytes.empty() || reinterpret_cast<std::uintptr_t>(bytes.data())!=function || !ExecutableAddress(target)) return {};
        return DirectCalls(bytes,function,target);
    }
    std::vector<std::uintptr_t> ExecutableCalls(std::uintptr_t target) {
        std::vector<std::uintptr_t> sites;
        if (!ExecutableAddress(target)) return sites;
        const auto text=REL::Module::get().segment(REL::Segment::textx);
        const auto code=reinterpret_cast<const std::uint8_t*>(text.address());
        for (std::size_t i=0; i+5<=text.size(); ++i) {
            if (code[i]!=0xE8) continue;
            std::int32_t displacement;
            std::memcpy(&displacement,code+i+1,4);
            const auto site=text.address()+i;
            if (site+5+static_cast<std::intptr_t>(displacement)!=target) continue;
            const auto bytes=FunctionBytes(site);
            if (!bytes.empty() && InstructionBoundary(bytes,site-reinterpret_cast<std::uintptr_t>(bytes.data())))
                sites.push_back(site);
        }
        return sites;
    }
}
