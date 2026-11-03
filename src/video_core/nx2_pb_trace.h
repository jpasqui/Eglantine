// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_PB_WRITE_TRACE=1: who writes into command memory. Every submitted GPFIFO entry's
// device pages are remembered (command memory is a ring the game refills, so a page stays a
// command page once it held an entry). Emulator writes into those pages from cache downloads
// (buffer and image), inline-to-memory uploads and FlushRegion are recorded with the 64-byte
// granules whose bytes they changed; the pushbuffer desync guard prints the records and submits
// that overlap the entry it dropped. The state lives in nx2_pb_trace.cpp.
namespace VideoCommon::Nx2PbTrace {

bool Enabled();

/// A submitted entry's words at [device_addr, device_addr + size) (one contiguous device piece).
void NoteSubmitted(u64 device_addr, u64 size, u32 channel);

/// The parts of [device_addr, device_addr + size) on command pages (empty when none or off).
std::vector<std::pair<u64, u64>> CommandPieces(u64 device_addr, u64 size);

/// Records a write into command pages: `before` and `after` hold the bytes of `pieces` in order.
void NoteWrite(const char* source, u64 device_addr, u64 size,
               const std::vector<std::pair<u64, u64>>& pieces, const std::vector<u8>& before,
               const std::vector<u8>& after);

/// Records a FlushRegion (or other event) over command pages without its bytes.
void NoteEvent(const char* source, u64 device_addr, u64 size);

/// Logs the recorded writes, events and submits that overlap [device_addr, device_addr + size).
void ReportOverlapping(u64 device_addr, u64 size);

/// Snapshots the command-page bytes of a write's range on construction and records the granules
/// it changed on destruction. Memory: anything with ReadBlockUnsafe(device address, dest, size).
template <typename Memory>
class WriteWatch {
public:
    WriteWatch(const char* source_, Memory& memory_, u64 device_addr_, u64 size_)
        : source{source_}, memory{memory_}, device_addr{device_addr_}, size{size_} {
        if (!Enabled() || size == 0) {
            return;
        }
        pieces = CommandPieces(device_addr, size);
        Read(before);
    }
    ~WriteWatch() {
        if (pieces.empty()) {
            return;
        }
        std::vector<u8> after;
        Read(after);
        NoteWrite(source, device_addr, size, pieces, before, after);
    }
    WriteWatch(const WriteWatch&) = delete;
    WriteWatch& operator=(const WriteWatch&) = delete;

private:
    void Read(std::vector<u8>& out) {
        size_t total = 0;
        for (const auto& piece : pieces) {
            total += static_cast<size_t>(piece.second);
        }
        out.resize(total);
        size_t offset = 0;
        for (const auto& [begin, length] : pieces) {
            memory.ReadBlockUnsafe(begin, out.data() + offset, static_cast<size_t>(length));
            offset += static_cast<size_t>(length);
        }
    }

    const char* source;
    Memory& memory;
    u64 device_addr;
    u64 size;
    std::vector<std::pair<u64, u64>> pieces;
    std::vector<u8> before;
};

} // namespace VideoCommon::Nx2PbTrace
