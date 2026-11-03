// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/nx2_video_decoder.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::Host1x {

namespace {

std::optional<NvdecCommon::VideoCodec> CodecFromMime(std::string_view mime) {
    if (mime.find("vp9") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP9;
    }
    if (mime.find("vp8") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP8;
    }
    // "video/av01" or "video/av1". FFmpeg decodes AV1 with libdav1d.
    if (mime.find("av01") != std::string_view::npos || mime.find("av1") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::AV1;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

} // namespace

struct Nx2VideoDecoder::Impl {
    std::string name;
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::optional<FFmpeg::DecoderContext> context;
    SwsContext* scaler{};
    /// Band scalers for the threaded conversion (EDEN_NX2_VDEC_THREADS).
    std::vector<SwsContext*> band_scalers;
    bool open{};

    /// Worker threads for Parallel(): started on first use, each runs job(index) for its share.
    struct Pool {
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable done;
        std::vector<std::thread> threads;
        const std::function<void(u32)>* job{};
        u32 count{};
        u32 next{};
        u32 finished{};
        u64 generation{};
        bool quit{};

        void Run(u32 workers, u32 jobs, const std::function<void(u32)>& work) {
            {
                std::scoped_lock lk{mutex};
                while (threads.size() < workers) {
                    threads.emplace_back([this] { Loop(); });
                }
                job = &work;
                count = jobs;
                next = 0;
                finished = 0;
                ++generation;
            }
            wake.notify_all();
            Help();
            std::unique_lock lk{mutex};
            done.wait(lk, [&] { return finished == count; });
            job = nullptr;
        }

        void Help() {
            for (;;) {
                u32 index;
                const std::function<void(u32)>* work;
                {
                    std::scoped_lock lk{mutex};
                    if (job == nullptr || next >= count) return;
                    index = next++;
                    work = job;
                }
                (*work)(index);
                std::scoped_lock lk{mutex};
                if (++finished == count) done.notify_all();
            }
        }

        void Loop() {
            u64 seen = 0;
            for (;;) {
                {
                    std::unique_lock lk{mutex};
                    wake.wait(lk, [&] { return quit || (generation != seen && job != nullptr); });
                    if (quit) return;
                    seen = generation;
                }
                Help();
            }
        }

        ~Pool() {
            {
                std::scoped_lock lk{mutex};
                quit = true;
            }
            wake.notify_all();
            for (auto& thread : threads) thread.join();
        }
    } pool;
    /// Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder
    /// lose theirs and take the earliest one instead.
    std::multiset<u64> pending_pts;
    std::vector<u8> packet; ///< FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data

    ~Impl() {
        sws_freeContext(scaler);
        for (SwsContext* band : band_scalers) sws_freeContext(band);
        context.reset();
        hardware.reset();
    }

    void Receive(std::vector<Nx2Picture>& pictures) {
        while (auto frame = context->ReceiveFrame()) {
            const AVFrame* f = frame->GetFrame();
            const s64 stamp = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp
                                                                         : f->pts;
            u64 pts{};
            if (stamp != AV_NOPTS_VALUE) {
                pts = static_cast<u64>(stamp);
                if (const auto it = pending_pts.find(pts); it != pending_pts.end()) {
                    pending_pts.erase(it);
                }
            } else if (!pending_pts.empty()) {
                pts = *pending_pts.begin();
                pending_pts.erase(pending_pts.begin());
            }
            pictures.push_back({static_cast<u32>(f->width), static_cast<u32>(f->height), pts,
                                std::move(frame)});
        }
    }
};

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime) : impl{std::make_unique<Impl>()} {
    impl->name = std::string{mime};
    const auto codec = CodecFromMime(mime);
    if (!codec) {
        LOG_ERROR(HW_GPU, "nx2 video: no decoder for codec \"{}\"", mime);
        return;
    }
    impl->decoder.emplace(*codec);
    if (impl->decoder->GetCodec() == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for \"{}\"", mime);
        return;
    }
    impl->context.emplace(*impl->decoder);
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
        impl->hardware.emplace();
        impl->hardware->InitializeForDecoder(*impl->context, *impl->decoder);
    }
    impl->open = impl->context->OpenContext(*impl->decoder);
    LOG_INFO(HW_GPU, "nx2 video: decoder for \"{}\" {}", mime, impl->open ? "open" : "failed to open");
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

bool Nx2VideoDecoder::IsOpen() const {
    return impl->open;
}

const std::string& Nx2VideoDecoder::Name() const {
    return impl->name;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (!impl->open || data.empty()) {
        return;
    }
    impl->packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl->packet.data(), data.data(), data.size());
    FFmpeg::Packet packet{std::span<const u8>{impl->packet.data(), data.size()}};
    packet.GetPacket()->pts = static_cast<s64>(pts);
    // Hidden VP9 frames never come out: keep the set from growing over a long movie.
    if (impl->pending_pts.size() >= 64) {
        impl->pending_pts.erase(impl->pending_pts.begin());
    }
    impl->pending_pts.insert(pts);
    impl->context->SendPacket(packet);
    impl->Receive(pictures);
}

void Nx2VideoDecoder::Drain(std::vector<Nx2Picture>& pictures) {
    if (!impl->open) {
        return;
    }
    avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
    impl->Receive(pictures);
    Reset();
}

void Nx2VideoDecoder::Reset() {
    if (impl->open) {
        avcodec_flush_buffers(impl->context->GetCodecContext());
    }
    impl->pending_pts.clear();
}

u32 Nx2VideoDecoder::Threads() {
    static const u32 threads = [] {
        if (const char* value = std::getenv("EDEN_NX2_VDEC_THREADS")) {
            return std::clamp<u32>(static_cast<u32>(std::strtoul(value, nullptr, 10)), 1, 16);
        }
        return 1U;
    }();
    return threads;
}

void Nx2VideoDecoder::Parallel(u32 count, const std::function<void(u32)>& job) {
    const u32 threads = Threads();
    if (threads <= 1 || count <= 1) {
        for (u32 i = 0; i < count; ++i) job(i);
        return;
    }
    impl->pool.Run((std::min)(threads, count) - 1, count, job);
}

void Nx2VideoDecoder::Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width,
                             u32 height, u64 pitch, u64 chroma_offset, std::span<u8> out) {
    const AVFrame* f = picture.frame->GetFrame();
    const u64 needed = format == Nx2PictureFormat::Nv12
                           ? chroma_offset + pitch * ((u64{height} + 1) / 2)
                           : pitch * height;
    if (width == 0 || height == 0 || out.size() < needed) {
        return;
    }
    const AVPixelFormat target = format == Nx2PictureFormat::Nv12    ? AV_PIX_FMT_NV12
                                 : format == Nx2PictureFormat::Rgba8 ? AV_PIX_FMT_RGBA
                                                                     : AV_PIX_FMT_BGRA;
    impl->scaler = sws_getCachedContext(impl->scaler, f->width, f->height,
                                        static_cast<AVPixelFormat>(f->format),
                                        static_cast<int>(width), static_cast<int>(height), target,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl->scaler == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: cannot convert {} to {}",
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)),
                  av_get_pix_fmt_name(target));
        return;
    }
    if (format != Nx2PictureFormat::Nv12) {
        // The stream's matrix and range; untagged HD video is BT.709.
        int space = SWS_CS_ITU601;
        if (f->colorspace == AVCOL_SPC_BT709 ||
            (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
            space = SWS_CS_ITU709;
        } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
            space = SWS_CS_BT2020;
        }
        const int* coefficients = sws_getCoefficients(space);
        sws_setColorspaceDetails(impl->scaler, coefficients, f->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);
    }
    const AVPixFmtDescriptor* source = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
    const u32 bands = (std::min<u32>)(Threads(), height / 64);
    if (format != Nx2PictureFormat::Nv12 && bands > 1 && source != nullptr &&
        (source->flags & AV_PIX_FMT_FLAG_PLANAR) != 0 && (source->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0 &&
        source->log2_chroma_h <= 1 && static_cast<u32>(f->width) == width &&
        static_cast<u32>(f->height) == height) {
        // Bands start on even rows so 4:2:0 chroma rows are not split.
        const u32 rows = ((height + bands - 1) / bands + 1) & ~1U;
        impl->band_scalers.resize(bands, nullptr);
        bool ok = true;
        for (u32 band = 0; band < bands; ++band) {
            const u32 first = band * rows;
            const u32 count = first >= height ? 0 : (std::min)(rows, height - first);
            if (count == 0) continue;
            SwsContext*& context = impl->band_scalers[band];
            context = sws_getCachedContext(context, f->width, static_cast<int>(count),
                                           static_cast<AVPixelFormat>(f->format), static_cast<int>(width),
                                           static_cast<int>(count), target, SWS_BILINEAR, nullptr, nullptr,
                                           nullptr);
            if (context == nullptr) {
                ok = false;
                break;
            }
            int space = SWS_CS_ITU601;
            if (f->colorspace == AVCOL_SPC_BT709 ||
                (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
                space = SWS_CS_ITU709;
            } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
                space = SWS_CS_BT2020;
            }
            const int* coefficients = sws_getCoefficients(space);
            sws_setColorspaceDetails(context, coefficients, f->color_range == AVCOL_RANGE_JPEG, coefficients, 1,
                                     0, 1 << 16, 1 << 16);
        }
        if (ok) {
            Parallel(bands, [&](u32 band) {
                const u32 first = band * rows;
                if (first >= height) return;
                const u32 count = (std::min)(rows, height - first);
                std::array<const u8*, 4> src{};
                for (int plane = 0; plane < 4 && f->data[plane] != nullptr; ++plane) {
                    const u32 shift = (plane == 1 || plane == 2) ? source->log2_chroma_h : 0;
                    src[plane] = f->data[plane] + static_cast<ptrdiff_t>(first >> shift) * f->linesize[plane];
                }
                std::array<u8*, 4> dst{out.data() + first * pitch, nullptr, nullptr, nullptr};
                std::array<int, 4> dst_strides{static_cast<int>(pitch), 0, 0, 0};
                sws_scale(impl->band_scalers[band], src.data(), f->linesize, 0, static_cast<int>(count),
                          dst.data(), dst_strides.data());
            });
            return;
        }
    }
    std::array<u8*, 4> planes{out.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{static_cast<int>(pitch), 0, 0, 0};
    if (format == Nx2PictureFormat::Nv12) {
        planes[1] = out.data() + chroma_offset;
        strides[1] = static_cast<int>(pitch);
    }
    sws_scale(impl->scaler, f->data, f->linesize, 0, f->height, planes.data(), strides.data());
}

} // namespace Tegra::Host1x
