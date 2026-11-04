// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"

namespace Service {

// EDEN_NX2_SYNCPT_TRACE=1: a bounded ring of nx2:syncpt requests (201), cancels (202), counter
// queries (103), fired slot events and the video decoder's syncpoint steps, printed when the game
// sets a terminate result (its abort path, 2162-0001). A debug aid for what the game waited on.
struct Nx2SyncptTraceEntry {
    s64 time_us;
    const char* what;
    u32 slot;
    u32 syncpoint;
    u64 value;
    u64 guest; ///< the guest syncpoint value at that moment
    u32 extra;
};

inline bool Nx2SyncptTraceOn() {
    static const bool on = std::getenv("EDEN_NX2_SYNCPT_TRACE") != nullptr;
    return on;
}

struct Nx2SyncptTraceRing {
    static constexpr size_t Size = 512;
    std::mutex mutex;
    std::array<Nx2SyncptTraceEntry, Size> entries{};
    size_t count{};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

inline Nx2SyncptTraceRing& Nx2SyncptTrace() {
    static Nx2SyncptTraceRing ring;
    return ring;
}

inline void Nx2SyncptTraceAdd(const char* what, u32 slot, u32 syncpoint, u64 value, u64 guest,
                              u32 extra = 0) {
    if (!Nx2SyncptTraceOn()) {
        return;
    }
    auto& ring = Nx2SyncptTrace();
    std::scoped_lock lk{ring.mutex};
    const s64 now = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - ring.start)
                        .count();
    ring.entries[ring.count++ % Nx2SyncptTraceRing::Size] =
        Nx2SyncptTraceEntry{now, what, slot, syncpoint, value, guest, extra};
}

/// Prints the last `last` entries.
inline void Nx2SyncptTraceDump(const char* reason, size_t last = 200) {
    if (!Nx2SyncptTraceOn()) {
        return;
    }
    auto& ring = Nx2SyncptTrace();
    std::scoped_lock lk{ring.mutex};
    const size_t shown = (std::min)({last, ring.count, Nx2SyncptTraceRing::Size});
    LOG_INFO(Service, "nx2:syncpt trace ({}): last {} of {} entries", reason, shown, ring.count);
    for (size_t i = ring.count - shown; i < ring.count; ++i) {
        const auto& e = ring.entries[i % Nx2SyncptTraceRing::Size];
        LOG_INFO(Service, "  {:>10} us {:<14} slot {:#x} syncpoint {} value {:#x} guest {:#x} extra {:#x}",
                 e.time_us, e.what, e.slot, e.syncpoint, e.value, e.guest, e.extra);
    }
}

} // namespace Service
