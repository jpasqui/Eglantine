// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_VOL_TRACE: the life of every 3D image (creation, uploads with a hash of the guest data,
// CPU writes, GPU writes, downloads, deletion) and the dispatches, DMA copies and inline uploads
// that touch one, in order, to follow a volume texture from its first write to its reads.
namespace VideoCommon::Nx2VolTrace {

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_VOL_TRACE") != nullptr;
    return enabled;
}

/// False once the trace has logged its line budget.
inline bool Budget() {
    static std::atomic<u32> lines{};
    return lines.fetch_add(1, std::memory_order_relaxed) < 20000;
}

inline std::mutex& RangeMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::vector<std::pair<u64, u64>>& Ranges() {
    static std::vector<std::pair<u64, u64>> ranges;
    return ranges;
}

/// Remembers the GPU range of a 3D image so copies into it can be recognised.
inline void AddRange(u64 gpu_addr, u64 size) {
    std::scoped_lock lock{RangeMutex()};
    auto& ranges = Ranges();
    for (const auto& range : ranges) {
        if (range.first == gpu_addr && range.second == size) {
            return;
        }
    }
    ranges.emplace_back(gpu_addr, size);
}

inline bool Overlaps(u64 gpu_addr, u64 size) {
    std::scoped_lock lock{RangeMutex()};
    for (const auto& range : Ranges()) {
        if (gpu_addr < range.first + range.second && range.first < gpu_addr + size) {
            return true;
        }
    }
    return false;
}

/// Image buffers compute programs write (EDEN_NX2_IMAGE_TRACE): range, writer, first dispatch time.
struct WrittenBuffer {
    u64 gpu_addr;
    u64 size;
    u64 program;
    u64 first_tick;
    u64 dispatches;
};

inline std::vector<WrittenBuffer>& WrittenBuffers() {
    static std::vector<WrittenBuffer> buffers;
    return buffers;
}

/// Returns true the first time this range is written by this program.
inline bool NoteWrittenBuffer(u64 gpu_addr, u64 size, u64 program, u64 tick) {
    std::scoped_lock lock{RangeMutex()};
    auto& buffers = WrittenBuffers();
    for (auto& buffer : buffers) {
        if (buffer.gpu_addr == gpu_addr && buffer.size == size && buffer.program == program) {
            ++buffer.dispatches;
            return false;
        }
    }
    if (buffers.size() < 4096) {
        buffers.push_back({gpu_addr, size, program, tick, 1});
    }
    return true;
}

inline bool ImageTraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_IMAGE_TRACE") != nullptr;
    return enabled;
}

/// Device-address ranges of the written image buffers, for the buffer cache's own events.
inline std::vector<std::pair<u64, u64>>& WrittenDeviceRanges() {
    static std::vector<std::pair<u64, u64>> ranges;
    return ranges;
}

inline void NoteWrittenDeviceRange(u64 device_addr, u64 size) {
    std::scoped_lock lock{RangeMutex()};
    auto& ranges = WrittenDeviceRanges();
    for (const auto& range : ranges) {
        if (range.first == device_addr && range.second == size) {
            return;
        }
    }
    if (ranges.size() < 4096) {
        ranges.emplace_back(device_addr, size);
    }
}

inline bool HitsWrittenDeviceRange(u64 device_addr, u64 size) {
    if (!ImageTraceEnabled()) {
        return false;
    }
    std::scoped_lock lock{RangeMutex()};
    for (const auto& range : WrittenDeviceRanges()) {
        if (device_addr < range.first + range.second && range.first < device_addr + size) {
            return true;
        }
    }
    return false;
}

inline std::vector<WrittenBuffer> WrittenBuffersOverlapping(u64 gpu_addr, u64 size) {
    std::scoped_lock lock{RangeMutex()};
    std::vector<WrittenBuffer> result;
    for (const auto& buffer : WrittenBuffers()) {
        if (gpu_addr < buffer.gpu_addr + buffer.size && buffer.gpu_addr < gpu_addr + size) {
            result.push_back(buffer);
        }
    }
    return result;
}

struct Digest {
    u64 hash;
    u64 nonzero;
};

inline Digest Hash(std::span<const u8> data) {
    u64 hash = 0xcbf29ce484222325ULL;
    u64 nonzero = 0;
    for (const u8 byte : data) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
        nonzero += byte != 0 ? 1 : 0;
    }
    return {hash, nonzero};
}

} // namespace VideoCommon::Nx2VolTrace
