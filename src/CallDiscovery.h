// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include "hde64.h"

namespace Aottg::Hooks {
    // Decode from a known function entry, never from a possible opcode inside data.
    // A padded copy keeps the decoder's lookahead inside owned memory.
    inline bool InstructionBoundary(std::span<const std::uint8_t> code, std::size_t offset) {
        if (offset >= code.size()) return false;
        for (std::size_t cursor=0; cursor<=offset;) {
            if (cursor==offset) return true;
            std::array<std::uint8_t,32> bytes{};
            const auto remaining=code.size()-cursor;
            std::memcpy(bytes.data(),code.data()+cursor,(std::min)(remaining,bytes.size()));
            hde64s instruction{};
            hde64_disasm(bytes.data(),&instruction);
            if ((instruction.flags & F_ERROR) || !instruction.len || instruction.len>remaining) return false;
            cursor+=instruction.len;
        }
        return false;
    }
    inline std::vector<std::uintptr_t> DirectCalls(std::span<const std::uint8_t> code,
        std::uintptr_t base,std::uintptr_t target) {
        std::vector<std::uintptr_t> sites;
        for (std::size_t i=0; i+5<=code.size(); ++i) {
            if (code[i]!=0xE8) continue;
            std::int32_t displacement;
            std::memcpy(&displacement,code.data()+i+1,4);
            if (base+i+5+static_cast<std::intptr_t>(displacement)==target && InstructionBoundary(code,i))
                sites.push_back(base+i);
        }
        return sites;
    }
    std::vector<std::uintptr_t> FunctionCalls(std::uintptr_t function,std::uintptr_t target);
    std::vector<std::uintptr_t> ExecutableCalls(std::uintptr_t target);
    bool ExecutableAddress(std::uintptr_t address);
}
