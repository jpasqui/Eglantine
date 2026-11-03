// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include <cmath>
#include <functional>
#include <unordered_map>
#include <string>
#include <fmt/format.h>

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

// EDEN_NX2_STALE_TRACE=<present>: from that present to the next, every draw (programs, targets, counts,
// depth state) and the images it samples. Each image is logged once with its flags and the hash of
// its guest data now and at its last upload; at the next frame each sampled image's host contents
// are downloaded and hashed, to compare a good and a bad run image by image.
inline u64 StaleFrame() {
    static const u64 frame = [] {
        const char* value = std::getenv("EDEN_NX2_STALE_TRACE");
        return value ? std::strtoull(value, nullptr, 10) : u64{0};
    }();
    return frame;
}

/// Presents so far (set by nx2:vi2), so the trace starts at a present number.
inline std::atomic<u64>& PresentCount() {
    static std::atomic<u64> presents{};
    return presents;
}

inline std::atomic<bool>& StaleActive() {
    static std::atomic<bool> active{};
    return active;
}

inline std::atomic<u32>& StaleDraw() {
    static std::atomic<u32> draw{};
    return draw;
}

struct UploadRecord {
    u64 gpu_addr;
    u64 hash;
    u64 frame;
};

/// Last upload of each image (by slot index) while the stale trace is enabled.
inline std::vector<UploadRecord>& Uploads() {
    static std::vector<UploadRecord> uploads;
    return uploads;
}

/// Images sampled in the traced frame (slot index, GPU address), in first-use order.
inline std::vector<std::pair<u32, u64>>& StaleSampled() {
    static std::vector<std::pair<u32, u64>> sampled;
    return sampled;
}

inline u64 CountNonzeroWords(std::span<const u8> data) {
    u64 count = 0;
    for (size_t i = 0; i + 8 <= data.size(); i += 8) {
        u64 word;
        std::memcpy(&word, data.data() + i, 8);
        count += word != 0 ? 1 : 0;
    }
    return count;
}

inline Digest Hash(std::span<const u8> data) {
    u64 hash = 0xcbf29ce484222325ULL;
    u64 nonzero = 0;
    for (const u8 byte : data) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
        nonzero += byte != 0 ? 1 : 0;
    }
    return {hash, nonzero};
}

/// EDEN_NX2_RANGE_HISTORY: a bounded history of what touched GPU memory (see NoteRange), printed for the range of
/// each new 3D image of at least 16x16x16.
inline bool RangeHistory() {
    static const bool enabled = std::getenv("EDEN_NX2_RANGE_HISTORY") != nullptr;
    return enabled;
}

struct RangeEvent {
    u64 gpu_addr;
    u64 size;
    u64 frame;
    std::string what;
};

inline std::mutex& RangeHistoryMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::vector<RangeEvent>& RangeEvents() {
    static std::vector<RangeEvent> events;
    return events;
}

inline std::atomic<u64>& RangeFrame() {
    static std::atomic<u64> frame{};
    return frame;
}

/// Records an event on [gpu_addr, gpu_addr + size); the oldest half is dropped past 400k events.
inline void NoteRange(u64 gpu_addr, u64 size, std::string what) {
    if (!RangeHistory() || gpu_addr == 0) return;
    std::scoped_lock lock{RangeHistoryMutex()};
    auto& events = RangeEvents();
    if (events.size() >= 400000) {
        events.erase(events.begin(), events.begin() + events.size() / 2);
    }
    events.push_back({gpu_addr, size, RangeFrame().load(std::memory_order_relaxed), std::move(what)});
}

/// The recorded events overlapping [gpu_addr, gpu_addr + size), oldest first (at most the last 200).
inline std::vector<RangeEvent> RangeEventsOverlapping(u64 gpu_addr, u64 size) {
    std::scoped_lock lock{RangeHistoryMutex()};
    std::vector<RangeEvent> result;
    for (const RangeEvent& event : RangeEvents()) {
        if (event.gpu_addr < gpu_addr + size && gpu_addr < event.gpu_addr + event.size) {
            result.push_back(event);
        }
    }
    if (result.size() > 200) {
        result.erase(result.begin(), result.end() - 200);
    }
    return result;
}

/// With EDEN_NX2_RANGE_HISTORY: the program of the draw (pixel shader) or dispatch being run, for NoteDeviceWrite.
inline std::atomic<u64>& CurrentProgram() {
    static std::atomic<u64> program{};
    return program;
}

/// Buffer-cache GPU writes (device ranges): first and last frame, count and the program of the last one.
struct DeviceWrite {
    u64 addr;
    u64 size;
    u64 first_frame;
    u64 last_frame;
    u64 count;
    u64 program;
};

inline std::mutex& DeviceWriteMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::unordered_map<u64, DeviceWrite>& DeviceWrites() {
    static std::unordered_map<u64, DeviceWrite> writes;
    return writes;
}

/// Records a buffer-cache GPU write; a repeat of a range and program only updates it (at most 50000 kept).
inline void NoteDeviceWrite(u64 addr, u64 size) {
    if (!RangeHistory() || size == 0) return;
    const u64 program = CurrentProgram().load(std::memory_order_relaxed);
    const u64 frame = RangeFrame().load(std::memory_order_relaxed);
    const u64 key = (addr * 0x9e3779b97f4a7c15ULL) ^ (size * 0xc2b2ae3d27d4eb4fULL) ^ program;
    std::scoped_lock lock{DeviceWriteMutex()};
    auto& writes = DeviceWrites();
    if (const auto it = writes.find(key); it != writes.end()) {
        it->second.last_frame = frame;
        ++it->second.count;
        return;
    }
    if (writes.size() < 50000) {
        writes.emplace(key, DeviceWrite{addr, size, frame, frame, 1, program});
    }
}

/// "addr+size frames a-b xN program P" for the recorded writes overlapping a device range (at most 12).
inline std::string DeviceWritesOverlapping(u64 addr, u64 size) {
    std::scoped_lock lock{DeviceWriteMutex()};
    std::string text;
    u32 shown = 0;
    for (const auto& [key, write] : DeviceWrites()) {
        if (write.addr < addr + size && addr < write.addr + write.size && shown++ < 12) {
            text += fmt::format(" [{:#x}+{:#x} frames {}-{} x{} program {:#x}]", write.addr, write.size,
                                write.first_frame, write.last_frame, write.count, write.program);
        }
    }
    return text.empty() ? std::string{" (no recorded buffer writes)"} : text;
}

/// Set by the Vulkan rasterizer: whether the buffer cache holds GPU-written data in a device range, and downloading it
/// into guest memory (the callers hold both caches' mutexes).
inline std::function<bool(u64, u64)>& BufferGpuModifiedHook() {
    static std::function<bool(u64, u64)> hook;
    return hook;
}

inline std::function<void(u64, u64)>& BufferDownloadHook() {
    static std::function<void(u64, u64)> hook;
    return hook;
}

/// EDEN_NX2_UPLOAD_SYNC=1: texture uploads first take the buffer cache's GPU-written data for their range.
inline bool UploadSync() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_UPLOAD_SYNC");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_PASS_VALUES: with the stale trace, statistics of each pass's colour targets when the targets change, and
/// of the images a traced dispatch bound, right after it.
inline bool PassValues() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PASS_VALUES");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_STALE_CBUFS=<first>-<last>: the traced draws whose constant buffers are scanned (0-0: none).
inline std::pair<u32, u32> StaleCbufDraws() {
    static const std::pair<u32, u32> range = [] {
        const char* value = std::getenv("EDEN_NX2_STALE_CBUFS");
        if (value == nullptr) return std::pair<u32, u32>{0, 0};
        char* end = nullptr;
        const u32 first = static_cast<u32>(std::strtoul(value, &end, 10));
        const u32 last = (end && *end == '-') ? static_cast<u32>(std::strtoul(end + 1, nullptr, 10)) : first;
        return std::pair<u32, u32>{first, last};
    }();
    return range;
}

/// ValueStats kind and bytes per texel for a VideoCore::Surface::PixelFormat value ({0, 0}: not decoded).
inline std::pair<u32, u32> ValueKind(u32 pixel_format) {
    switch (pixel_format) {
    case 16: return {1, 8};  // R16G16B16A16_FLOAT
    case 21: return {2, 4};  // B10G11R11_FLOAT
    case 35: return {3, 16}; // R32G32B32A32_FLOAT
    case 39: return {4, 4};  // R32_FLOAT
    case 40: return {5, 2};  // R16_FLOAT
    case 37: return {6, 8};  // R32G32_FLOAT
    case 46: return {7, 4};  // R16G16_FLOAT
    case 0: case 34: case 51: case 63: return {8, 4}; // RGBA8 / BGRA8, unorm and srgb
    case 7: return {9, 4};   // A2B10G10R10_UNORM
    default: return {0, 0};
    }
}

/// EDEN_NX2_VOL_SNAP=<dir>: snapshots of small deep 3D images (lightmap indirection) at frame ends.
inline const char* VolSnap() {
    static const char* const dir = [] {
        const char* value = std::getenv("EDEN_NX2_VOL_SNAP");
        return (value != nullptr && value[0] != '\0') ? value : nullptr;
    }();
    return dir;
}

/// EDEN_NX2_STALE_SAVE=<dir>: with the stale trace, the folder that receives the small sampled images.
inline const char* StaleSave() {
    static const char* const dir = [] {
        const char* value = std::getenv("EDEN_NX2_STALE_SAVE");
        return (value != nullptr && value[0] != '\0') ? value : nullptr;
    }();
    return dir;
}

/// EDEN_NX2_READBACK_TRACE=1: GPU->CPU readback paths (copy-engine block->pitch copies, DMA image lookups,
/// async buffer downloads, small image downloads, CPU reads of GPU-written memory); false once 3000 lines are used.
inline bool ReadbackTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_READBACK_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (!enabled) {
        return false;
    }
    static std::atomic<u32> lines{};
    return lines.fetch_add(1, std::memory_order_relaxed) < 3000;
}

inline bool ReadbackTraceOn() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_READBACK_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

struct SemEvent {
    u64 time_us;
    u64 addr;
    u32 value;
    char kind;
};

inline u64 SteadyMicros() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline std::mutex& SemRingMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::array<SemEvent, 256>& SemRing() {
    static std::array<SemEvent, 256> ring{};
    return ring;
}

inline u64& SemRingCount() {
    static u64 count{};
    return count;
}

/// kind: 'f' fence release written, 'd' direct release, 'm' mirrored (either path).
inline void RecordSem(char kind, u64 addr, u32 value) {
    if (!ReadbackTraceOn()) {
        return;
    }
    std::scoped_lock lock{SemRingMutex()};
    SemRing()[SemRingCount()++ % 256] = SemEvent{SteadyMicros(), addr, value, kind};
}

inline std::string SemRingText(size_t last) {
    std::scoped_lock lock{SemRingMutex()};
    const u64 now = SteadyMicros();
    const u64 count = SemRingCount();
    const u64 first = count > last ? count - last : 0;
    std::string text;
    for (u64 i = first; i < count; ++i) {
        const SemEvent& event = SemRing()[i % 256];
        text += fmt::format(" {}{:#x}={:#x}@-{}", event.kind, event.addr, event.value, now - event.time_us);
    }
    return text;
}

/// EDEN_NX2_READBACK_SYNC=1: guest CPU reads of readback destination pages wait for the GPU thread first.
inline bool ReadbackSync() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_READBACK_SYNC");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_SEM_READ_WATCH=1: guest CPU reads of semaphore pages go into the semaphore ring.
inline bool SemReadWatch() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_READ_WATCH");
        return value != nullptr && value[0] != '\0' && value[0] != '0' && ReadbackTraceOn();
    }();
    return enabled;
}

inline std::vector<u64>& SemWatchPages() {
    static std::vector<u64> pages;
    return pages;
}

/// True when the page of `addr` was not watched yet (the caller read-protects it).
inline bool SemWatchAdd(u64 addr) {
    std::scoped_lock lock{SemRingMutex()};
    auto& pages = SemWatchPages();
    const u64 page = addr >> 12;
    if (std::find(pages.begin(), pages.end(), page) != pages.end() || pages.size() >= 64) {
        return false;
    }
    pages.push_back(page);
    return true;
}

inline bool SemWatchHit(u64 addr) {
    if (!SemReadWatch()) {
        return false;
    }
    std::scoped_lock lock{SemRingMutex()};
    const auto& pages = SemWatchPages();
    return std::find(pages.begin(), pages.end(), addr >> 12) != pages.end();
}

/// Device pages of readback destinations (see EDEN_NX2_READBACK_TRACE), for the CPU read log.
inline std::mutex& ReadbackWatchMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::vector<u64>& ReadbackWatchPages() {
    static std::vector<u64> pages;
    return pages;
}

inline std::atomic<bool>& ReadbackWatchActive() {
    static std::atomic<bool> active{};
    return active;
}

/// Adds the pages of [addr, addr + size); returns the pages that were not watched yet.
inline std::vector<u64> ReadbackWatchAdd(u64 addr, u64 size) {
    std::vector<u64> added;
    std::scoped_lock lock{ReadbackWatchMutex()};
    auto& pages = ReadbackWatchPages();
    for (u64 page = addr >> 12; page <= (addr + (std::max<u64>)(size, 1) - 1) >> 12; ++page) {
        if (std::find(pages.begin(), pages.end(), page) == pages.end() && pages.size() < 256) {
            pages.push_back(page);
            added.push_back(page);
        }
    }
    ReadbackWatchActive() = true;
    return added;
}

inline bool ReadbackWatched(u64 addr, u64 size) {
    if (!ReadbackWatchActive().load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{ReadbackWatchMutex()};
    for (const u64 page : ReadbackWatchPages()) {
        if ((page << 12) < addr + size && addr < ((page + 1) << 12)) {
            return true;
        }
    }
    return false;
}

inline std::string HexBytes(const u8* data, size_t size) {
    std::string text;
    for (size_t i = 0; i + 4 <= size && i < 64; i += 4) {
        u32 word;
        std::memcpy(&word, data + i, 4);
        text += fmt::format(" {:08x}", word);
    }
    return text;
}

/// EDEN_NX2_INPUT_PRINT=<dir>: with the stale trace and EDEN_NX2_STALE_CBUFS, the contents (up to 64 KiB) of every
/// buffer the scanned draws read, saved once per hash as <dir>/<hash>.bin; storage and texel buffers get an
/// "input buffer" line at bind time (round 22).
inline const char* InputPrint() {
    static const char* const dir = [] {
        const char* value = std::getenv("EDEN_NX2_INPUT_PRINT");
        return (value != nullptr && value[0] != '\0' && value[0] != '0') ? value : nullptr;
    }();
    return dir;
}

inline u64 Fnv64(const void* data, size_t size) {
    const u8* bytes = static_cast<const u8*>(data);
    u64 hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
    }
    return hash;
}

inline void SaveInputBlob(u64 hash, const void* data, size_t size) {
    const char* const dir = InputPrint();
    if (dir == nullptr || size == 0 || size > 0x10000) {
        return;
    }
    static std::mutex mutex;
    static std::vector<u64> saved;
    std::scoped_lock lock{mutex};
    if (std::find(saved.begin(), saved.end(), hash) != saved.end() || saved.size() >= 20000) {
        return;
    }
    saved.push_back(hash);
    const std::string path = fmt::format("{}/{:016x}.bin", dir, hash);
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, 1, size, file);
        std::fclose(file);
    }
}

/// EDEN_NX2_STALE_VALUES: with the stale trace, value statistics of each sampled image at the end of the frame.
inline bool StaleValues() {
    static const bool enabled = std::getenv("EDEN_NX2_STALE_VALUES") != nullptr;
    return enabled;
}

inline float HalfToFloat(u32 h) {
    const u32 sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? std::nanf("") : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

/// Unsigned small float with `mantissa_bits` and a 5-bit exponent (R11G11B10F channels).
inline float SmallFloat(u32 bits, u32 mantissa_bits) {
    const u32 exponent = bits >> mantissa_bits, mantissa = bits & ((1u << mantissa_bits) - 1);
    if (exponent == 0) {
        return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
    }
    if (exponent == 31) {
        return mantissa ? std::nanf("") : INFINITY;
    }
    return std::ldexp(static_cast<float>(mantissa | (1u << mantissa_bits)),
                      static_cast<int>(exponent) - 15 - static_cast<int>(mantissa_bits));
}

/// Decodes texel `index` into up to 4 floats; returns the channel count (0: format not handled).
/// `kind`: 1 RGBA16F, 2 R11G11B10F, 3 RGBA32F, 4 R32F, 5 R16F, 6 RG32F, 7 RG16F, 8 RGBA8 unorm, 9 RGB10A2 unorm.
inline u32 DecodeTexel(u32 kind, const u8* p, float out[4]) {
    u32 w[4]{};
    switch (kind) {
    case 1: {
        u16 h[4];
        std::memcpy(h, p, 8);
        for (int i = 0; i < 4; ++i) out[i] = HalfToFloat(h[i]);
        return 4;
    }
    case 2:
        std::memcpy(w, p, 4);
        out[0] = SmallFloat(w[0] & 0x7ff, 6);
        out[1] = SmallFloat((w[0] >> 11) & 0x7ff, 6);
        out[2] = SmallFloat(w[0] >> 22, 5);
        return 3;
    case 3:
        std::memcpy(out, p, 16);
        return 4;
    case 4:
        std::memcpy(out, p, 4);
        return 1;
    case 5: {
        u16 h;
        std::memcpy(&h, p, 2);
        out[0] = HalfToFloat(h);
        return 1;
    }
    case 6:
        std::memcpy(out, p, 8);
        return 2;
    case 7: {
        u16 h[2];
        std::memcpy(h, p, 4);
        out[0] = HalfToFloat(h[0]);
        out[1] = HalfToFloat(h[1]);
        return 2;
    }
    case 8:
        for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.0f;
        return 4;
    case 9:
        std::memcpy(w, p, 4);
        out[0] = (w[0] & 0x3ff) / 1023.0f;
        out[1] = ((w[0] >> 10) & 0x3ff) / 1023.0f;
        out[2] = ((w[0] >> 20) & 0x3ff) / 1023.0f;
        out[3] = (w[0] >> 30) / 3.0f;
        return 4;
    default:
        return 0;
    }
}

/// "ch0 min/max/mean ... nan N inf N huge N" over `texels` texels of `bytes_per_texel` from `data`.
inline std::string ValueStats(u32 kind, u32 bytes_per_texel, std::span<const u8> data, u64 texels) {
    texels = std::min<u64>(texels, data.size() / std::max<u32>(bytes_per_texel, 1));
    double sum[4]{}, lo[4], hi[4];
    for (int i = 0; i < 4; ++i) {
        lo[i] = INFINITY;
        hi[i] = -INFINITY;
    }
    u64 nan = 0, inf = 0, huge = 0, finite[4]{};
    u32 channels = 0;
    std::string small;
    for (u64 t = 0; t < texels; ++t) {
        float v[4]{};
        channels = DecodeTexel(kind, data.data() + t * bytes_per_texel, v);
        if (channels == 0) return {};
        if (texels <= 4) {
            small += " [";
            for (u32 c = 0; c < channels; ++c) small += fmt::format("{}{}", c ? "," : "", v[c]);
            small += "]";
        }
        for (u32 c = 0; c < channels; ++c) {
            if (std::isnan(v[c])) {
                ++nan;
            } else if (std::isinf(v[c])) {
                ++inf;
            } else {
                if (std::fabs(v[c]) > 1000.0f) ++huge;
                lo[c] = std::min<double>(lo[c], v[c]);
                hi[c] = std::max<double>(hi[c], v[c]);
                sum[c] += v[c];
                ++finite[c];
            }
        }
    }
    std::string text;
    for (u32 c = 0; c < channels; ++c) {
        text += fmt::format(" c{} {:.4g}/{:.4g}/{:.4g}", c, lo[c], hi[c], finite[c] ? sum[c] / finite[c] : 0.0);
    }
    text += fmt::format(" nan {} inf {} huge {}", nan, inf, huge);
    return text + small;
}

} // namespace VideoCommon::Nx2VolTrace
