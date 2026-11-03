// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

#include "common/common_types.h"

// Guest CPU writes to memory a cache holds GPU-written data for are collected and applied at the
// next gather of dirty memory. A download before that gather would put the older GPU data back
// over them. FlushRegion lists the collected ranges in the flushed region here, and the
// downloads leave those bytes alone.
namespace VideoCommon::Nx2PendingWrites {

using Ranges = std::vector<std::pair<u64, u64>>;

/// EDEN_NX2_PROTECT_CPU_WRITES=1/0; unset: on unless EDEN_NX2_SURFACE_OPS=0.
inline bool Enabled() {
    static const bool enabled = [] {
        if (const char* value = std::getenv("EDEN_NX2_PROTECT_CPU_WRITES")) {
            return value[0] != '0';
        }
        const char* surface_ops = std::getenv("EDEN_NX2_SURFACE_OPS");
        return surface_ops == nullptr || surface_ops[0] != '0';
    }();
    return enabled;
}

inline thread_local const Ranges* current = nullptr;

class Scope {
public:
    explicit Scope(const Ranges* ranges) : previous{current} {
        current = ranges;
    }
    ~Scope() {
        current = previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const Ranges* previous;
};

inline bool Overlaps(u64 addr, u64 size) {
    if (current == nullptr) {
        return false;
    }
    return std::any_of(current->begin(), current->end(), [&](const auto& range) {
        return addr < range.first + range.second && range.first < addr + size;
    });
}

/// Writes data to device memory at addr, skipping the bytes of collected CPU writes.
template <typename DeviceMemory>
void WriteExceptPending(DeviceMemory& memory, u64 addr, const u8* data, u64 size) {
    if (!Overlaps(addr, size)) {
        memory.WriteBlockUnsafe(addr, data, size);
        return;
    }
    Ranges skipped;
    for (const auto& range : *current) {
        const u64 begin = (std::max)(addr, range.first);
        const u64 end = (std::min)(addr + size, range.first + range.second);
        if (begin < end) {
            skipped.emplace_back(begin, end);
        }
    }
    std::sort(skipped.begin(), skipped.end());
    u64 cursor = addr;
    for (const auto& [begin, end] : skipped) {
        if (begin > cursor) {
            memory.WriteBlockUnsafe(cursor, data + (cursor - addr), begin - cursor);
        }
        cursor = (std::max)(cursor, end);
    }
    if (cursor < addr + size) {
        memory.WriteBlockUnsafe(cursor, data + (cursor - addr), addr + size - cursor);
    }
}

} // namespace VideoCommon::Nx2PendingWrites
