// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include "common/logging.h"
#include <chrono>
#include "video_core/nx2_pb_trace.h"

namespace VideoCommon::Nx2PbTrace {

namespace {
struct ChangedWrite {
    const char* source;
    u64 device_addr;
    u64 size;
    u64 changed;
    u64 first_changed;
    std::chrono::steady_clock::time_point when;
};
std::mutex changed_mutex;
std::vector<ChangedWrite> changed_ring;
size_t changed_next = 0;
const std::chrono::steady_clock::time_point changed_epoch = std::chrono::steady_clock::now();
} // Anonymous namespace

bool DownloadWatch() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_DOWNLOAD_WATCH");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

void NoteChangedWrite(const char* source, u64 device_addr, u64 size, u64 changed, u64 first_changed) {
    std::scoped_lock lock{changed_mutex};
    constexpr size_t RingSize = 16384;
    const ChangedWrite write{source, device_addr, size, changed, first_changed, std::chrono::steady_clock::now()};
    if (changed_ring.size() < RingSize) {
        changed_ring.push_back(write);
    } else {
        changed_ring[changed_next] = write;
        changed_next = (changed_next + 1) % RingSize;
    }
}

std::string ChangedWritesOverlapping(u64 device_addr, u64 size) {
    std::scoped_lock lock{changed_mutex};
    std::string text;
    u32 shown = 0;
    const size_t count = changed_ring.size();
    for (size_t i = 0; i < count && shown < 24; ++i) {
        const ChangedWrite& write = changed_ring[(changed_next + i) % count];
        if (write.device_addr < device_addr + size && device_addr < write.device_addr + write.size) {
            ++shown;
            text += fmt::format("\n    at {:.3f} s {} {:#x}+{:#x} changed {} bytes from +{:#x}",
                                std::chrono::duration<double>(write.when - changed_epoch).count(), write.source,
                                write.device_addr, write.size, write.changed, write.first_changed);
        }
    }
    return text.empty() ? std::string{" none"} : text;
}

namespace {

constexpr u64 PageBits = 12;
constexpr u64 Granule = 64;
constexpr size_t MaxGranules = 8;
constexpr u32 LiveLogs = 128;

struct Record {
    u64 seq;
    const char* source; ///< nullptr: a submitted entry
    u64 addr;
    u64 size;
    u32 channel;
    u32 changed; ///< Granules whose bytes changed (writes)
    std::array<u64, MaxGranules> granules;
};

struct State {
    std::mutex mutex;
    std::unordered_set<u64> pages;
    std::array<Record, 8192> ring{};
    u64 seq = 0;
    u64 writes = 0;
    u64 changing_writes = 0;
    u32 live_logs = 0;

    Record& Next() {
        Record& record = ring[seq % ring.size()];
        record = Record{};
        record.seq = seq++;
        return record;
    }
};

State& GetState() {
    static State state;
    return state;
}

bool HitsPage(const State& state, u64 addr, u64 size) {
    for (u64 page = addr >> PageBits; page <= (addr + size - 1) >> PageBits; ++page) {
        if (state.pages.contains(page)) {
            return true;
        }
    }
    return false;
}

std::string Describe(const Record& record) {
    if (record.source == nullptr) {
        return fmt::format("#{} submit ch {} {:#x}+{:#x}", record.seq, record.channel, record.addr,
                           record.size);
    }
    std::string text = fmt::format("#{} {} {:#x}+{:#x}", record.seq, record.source, record.addr,
                                   record.size);
    if (record.changed != 0) {
        text += fmt::format(", {} granule(s) changed:", record.changed);
        for (size_t i = 0; i < std::min<size_t>(record.changed, MaxGranules); ++i) {
            text += fmt::format(" {:#x}", record.granules[i]);
        }
    }
    return text;
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PB_WRITE_TRACE");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

void NoteSubmitted(u64 device_addr, u64 size, u32 channel) {
    if (!Enabled() || size == 0) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    for (u64 page = device_addr >> PageBits; page <= (device_addr + size - 1) >> PageBits; ++page) {
        state.pages.insert(page);
    }
    Record& record = state.Next();
    record.addr = device_addr;
    record.size = size;
    record.channel = channel;
}

std::vector<std::pair<u64, u64>> CommandPieces(u64 device_addr, u64 size) {
    std::vector<std::pair<u64, u64>> pieces;
    if (!Enabled() || size == 0) {
        return pieces;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    const u64 end = device_addr + size;
    for (u64 page = device_addr >> PageBits; page <= (end - 1) >> PageBits; ++page) {
        if (!state.pages.contains(page)) {
            continue;
        }
        const u64 begin = std::max(device_addr, page << PageBits);
        const u64 stop = std::min(end, (page + 1) << PageBits);
        if (!pieces.empty() && pieces.back().first + pieces.back().second == begin) {
            pieces.back().second += stop - begin;
        } else {
            pieces.emplace_back(begin, stop - begin);
        }
    }
    return pieces;
}

void NoteWrite(const char* source, u64 device_addr, u64 size,
               const std::vector<std::pair<u64, u64>>& pieces, const std::vector<u8>& before,
               const std::vector<u8>& after) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    ++state.writes;
    Record record{};
    record.source = source;
    record.addr = device_addr;
    record.size = size;
    size_t offset = 0;
    u64 last_granule = ~u64{};
    for (const auto& [begin, length] : pieces) {
        for (u64 i = 0; i < length; ++i) {
            if (before[offset + i] == after[offset + i]) {
                continue;
            }
            const u64 granule = (begin + i) & ~(Granule - 1);
            if (granule == last_granule) {
                continue;
            }
            last_granule = granule;
            if (record.changed < MaxGranules) {
                record.granules[record.changed] = granule;
            }
            ++record.changed;
        }
        offset += static_cast<size_t>(length);
    }
    if (record.changed == 0) {
        return; // the ring keeps only writes that changed something
    }
    Record& slot = state.Next();
    record.seq = slot.seq;
    slot = record;
    ++state.changing_writes;
    if (state.live_logs < LiveLogs) {
        ++state.live_logs;
        LOG_INFO(HW_GPU, "pb write trace: {}", Describe(record));
    } else if ((state.changing_writes % 1000) == 0) {
        LOG_INFO(HW_GPU, "pb write trace: {} writes into command pages, {} changed bytes",
                 state.writes, state.changing_writes);
    }
}

void NoteEvent(const char* source, u64 device_addr, u64 size) {
    if (!Enabled() || size == 0) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!HitsPage(state, device_addr, size)) {
        return;
    }
    Record& record = state.Next();
    record.source = source;
    record.addr = device_addr;
    record.size = size;
}

void ReportOverlapping(u64 device_addr, u64 size) {
    if (!Enabled() || size == 0) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    LOG_WARNING(HW_GPU,
                "pb write trace: records over {:#x}+{:#x} (of {} kept; {} writes into command "
                "pages, {} changed bytes):",
                device_addr, size, std::min<u64>(state.seq, state.ring.size()), state.writes,
                state.changing_writes);
    const u64 first = state.seq > state.ring.size() ? state.seq - state.ring.size() : 0;
    u32 shown = 0;
    for (u64 seq = state.seq; seq-- > first && shown < 48;) {
        const Record& record = state.ring[seq % state.ring.size()];
        if (record.addr < device_addr + size && device_addr < record.addr + record.size) {
            LOG_WARNING(HW_GPU, "  {}", Describe(record));
            ++shown;
        }
    }
}

} // namespace VideoCommon::Nx2PbTrace
