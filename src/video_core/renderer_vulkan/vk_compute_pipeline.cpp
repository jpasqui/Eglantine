// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/nx2_vol_trace.h"
#include "video_core/shader_handle_program.h"
#include "video_core/shader_notify.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "common/logging.h"
#include "common/settings.h"

namespace Vulkan {

using Shader::ImageBufferDescriptor;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_WORDS_OFFSET;
using Tegra::Texture::TexturePair;

ComputePipeline::ComputePipeline(const Device& device_, Scheduler& scheduler, vk::PipelineCache& pipeline_cache_,
                                 DescriptorPool& descriptor_pool,
                                 GuestDescriptorQueue& guest_descriptor_queue_,
                                 DescriptorBufferRing& descriptor_buffer_ring_,
                                 Common::ThreadWorker* thread_worker,
                                 PipelineStatistics* pipeline_statistics,
                                 VideoCore::ShaderNotify* shader_notify, const Shader::Info& info_,
                                 vk::ShaderModule spv_module_, u64 shader_hash_)
    : device{device_},
      pipeline_cache(pipeline_cache_), guest_descriptor_queue{guest_descriptor_queue_},
      descriptor_buffer_ring{descriptor_buffer_ring_}, info{info_},
      shader_hash{shader_hash_}, spv_module(std::move(spv_module_)) {
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    std::copy_n(info.constant_buffer_used_sizes.begin(), uniform_buffer_sizes.size(),
                uniform_buffer_sizes.begin());
    num_descriptor_entries = NumDescriptorEntries(info);

    DescriptorLayoutBuilder builder{device};
    builder.Add(info, VK_SHADER_STAGE_COMPUTE_BIT);

    uses_push_descriptor = builder.CanUsePushDescriptor();
    uses_descriptor_buffer = builder.CanUseDescriptorBuffer() && descriptor_buffer_ring.IsValid();
    descriptor_set_layout =
        builder.CreateDescriptorSetLayout(uses_push_descriptor, uses_descriptor_buffer);
    if (uses_descriptor_buffer) {
        descriptor_buffer_layout = builder.MakeDescriptorBufferLayout(*descriptor_set_layout);
        if (!descriptor_buffer_ring.CanAllocate(descriptor_buffer_layout.size)) {
            LOG_DEBUG(Render_Vulkan,
                        "Compute shader {:016X} needs {} descriptor bytes per dispatch, falling "
                        "back to sets",
                        shader_hash, descriptor_buffer_layout.size);
            uses_descriptor_buffer = false;
            descriptor_buffer_layout = {};
            descriptor_set_layout = builder.CreateDescriptorSetLayout(false);
        }
    }
    pipeline_layout = builder.CreatePipelineLayout(*descriptor_set_layout);
    if (!uses_descriptor_buffer) {
        descriptor_update_template =
            builder.CreateTemplate(*descriptor_set_layout, *pipeline_layout, uses_push_descriptor);
        if (!uses_push_descriptor) {
            descriptor_allocator =
                descriptor_pool.Allocator(device, scheduler, *descriptor_set_layout, info);
        }
    }

    auto func{[this, shader_notify, pipeline_statistics] {
        const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
            .pNext = nullptr,
            .requiredSubgroupSize = GuestWarpSize,
        };
        VkPipelineCreateFlags flags{};
        if (device.IsKhrPipelineExecutablePropertiesEnabled() && Settings::values.renderer_debug.GetValue()) {
            flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        }
        if (uses_descriptor_buffer) {
            flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        }
        const VkComputePipelineCreateInfo compute_ci{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext =
                    device.IsExtSubgroupSizeControlSupported() ? &subgroup_size_ci : nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *spv_module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            .layout = *pipeline_layout,
            .basePipelineHandle = 0,
            .basePipelineIndex = 0,
        };
        try {
            pipeline = device.GetLogical().CreateComputePipeline(compute_ci, *pipeline_cache);
        } catch (const vk::Exception& exception) {
            LOG_CRITICAL(Render_Vulkan, "Adreno rejected compute shader {:016X}: {}", shader_hash,
                         exception.what());
            std::scoped_lock lock{build_mutex};
            is_built = true;
            build_condvar.notify_one();
            if (shader_notify) {
                shader_notify->MarkShaderComplete();
            }
            return;
        }

        // Log compute pipeline creation
        if (GPU::Logging::IsActive()) {
            GPU::Logging::GPULogger::GetInstance().LogPipelineStateChange(
                "ComputePipeline created"
            );
        }

        if (pipeline_statistics) {
            pipeline_statistics->Collect(device, *pipeline);
        }
        // Nothing reads the module once the pipeline exists; the driver keeps its SPIR-V while it
        // lives.
        spv_module.reset();
        std::scoped_lock lock{build_mutex};
        is_built = true;
        build_condvar.notify_one();
        if (shader_notify) {
            shader_notify->MarkShaderComplete();
        }
    }};
    if (thread_worker) {
        thread_worker->QueueWork(std::move(func));
    } else {
        func();
    }
}

bool ComputePipeline::Configure(Tegra::Engines::KeplerCompute& kepler_compute,
                                Tegra::MemoryManager& gpu_memory, Scheduler& scheduler,
                                BufferCache& buffer_cache, TextureCache& texture_cache) {
    guest_descriptor_queue.Acquire(scheduler, num_descriptor_entries, uses_descriptor_buffer);

    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &uniform_buffer_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    size_t ssbo_index{};
    for (const auto& desc : info.storage_buffers_descriptors) {
        ASSERT(desc.count == 1);
        buffer_cache.BindComputeStorageBuffer(ssbo_index, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written);
        ++ssbo_index;
    }

    texture_cache.SynchronizeDescriptors(true);

    boost::container::small_vector<VideoCommon::ImageViewInOut, 64> views;
    boost::container::small_vector<VideoCommon::SamplerId, 64> samplers;

    const auto& qmd{kepler_compute.launch_description};
    const auto& cbufs{qmd.const_buffer_config};
    const bool via_header_index{qmd.linked_tsc != 0};
    const auto read_handle{[&](const auto& desc, u32 index) {
        if (desc.handle_program != ~0U) {
            // Read by the shader from a storage buffer: computed for this dispatch.
            return TexturePair(VideoCommon::EvaluateTextureHandle(
                                   info, desc.handle_program, gpu_memory,
                                   [&](u32 cbuf) -> GPUVAddr {
                                       return cbuf < cbufs.size() &&
                                                      ((qmd.const_buffer_enable_mask >> cbuf) &
                                                       1) != 0
                                                  ? cbufs[cbuf].Address()
                                                  : GPUVAddr{};
                                   }),
                               via_header_index);
        }
        ASSERT(((qmd.const_buffer_enable_mask >> desc.cbuf_index) & 1) != 0);
        const u32 index_offset{index << desc.size_shift};
        const u32 offset{desc.cbuf_offset + index_offset};
        const GPUVAddr addr{cbufs[desc.cbuf_index].Address() + offset};
        if constexpr (std::is_same_v<decltype(desc), const Shader::TextureDescriptor&> ||
                      std::is_same_v<decltype(desc), const Shader::TextureBufferDescriptor&>) {
            if (desc.has_secondary) {
                ASSERT(((qmd.const_buffer_enable_mask >> desc.secondary_cbuf_index) & 1) != 0);
                const u32 secondary_offset{desc.secondary_cbuf_offset + index_offset};
                const GPUVAddr separate_addr{cbufs[desc.secondary_cbuf_index].Address() +
                                             secondary_offset};
                const u32 lhs_raw{gpu_memory.Read<u32>(addr) << desc.shift_left};
                const u32 rhs_raw{gpu_memory.Read<u32>(separate_addr) << desc.secondary_shift_left};
                return TexturePair(lhs_raw | rhs_raw, via_header_index);
            }
        }
        return TexturePair(gpu_memory.Read<u32>(addr), via_header_index);
    }};
    const auto add_image{[&](const auto& desc, bool blacklist) {
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle{read_handle(desc, index)};
            views.push_back({
                .index = handle.first,
                .blacklist = blacklist,
                .id = {},
            });
        }
    }};
    for (const auto& desc : info.texture_buffer_descriptors) {
        add_image(desc, false);
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        add_image(desc, false);
    }
    for (const auto& desc : info.texture_descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle{read_handle(desc, index)};
            views.push_back({handle.first});

            VideoCommon::SamplerId sampler = texture_cache.GetSamplerId(handle.second, true);
            samplers.push_back(sampler);
        }
    }
    for (const auto& desc : info.image_descriptors) {
        add_image(desc, desc.is_written);
    }
    texture_cache.FillImageViews(std::span(views.data(), views.size()), true);
    if (VideoCommon::Nx2VolTrace::Enabled()) {
        // Each dispatch that reads or writes a 3D image: which ones, in order with the uploads.
        size_t textures_begin = 0;
        for (const auto& desc : info.texture_buffer_descriptors) textures_begin += desc.count;
        for (const auto& desc : info.image_buffer_descriptors) textures_begin += desc.count;
        size_t images_begin = textures_begin;
        for (const auto& desc : info.texture_descriptors) images_begin += desc.count;
        std::string touched;
        for (size_t i = textures_begin; i < views.size(); ++i) {
            if (views[i].id == VideoCommon::NULL_IMAGE_VIEW_ID) continue;
            const auto& view = texture_cache.GetImageView(views[i].id);
            if (view.type != VideoCommon::ImageViewType::e3D) continue;
            touched += fmt::format(" {}{:#x}#{}/{}", i >= images_begin ? "w" : "r", view.gpu_addr,
                                   view.image_id.index, static_cast<u32>(view.format));
        }
        static u64 dispatch{};
        ++dispatch;
        if (!touched.empty() && VideoCommon::Nx2VolTrace::Budget()) {
            const u64 program = (u64{kepler_compute.regs.code_loc.address_high} << 32) |
                                kepler_compute.regs.code_loc.address_low;
            LOG_INFO(HW_GPU, "vol dispatch {} program {:#x} grid {}x{}x{}:{}", dispatch, program,
                     qmd.grid_dim_x.Value(), qmd.grid_dim_y.Value(), qmd.grid_dim_z.Value(), touched);
        }
    }
    // EDEN_NX2_IMAGE_TRACE: the storage images (and image buffers) a compute program binds, once
    // per program and handle, to find what a pass writes.
    if (static const bool image_trace = std::getenv("EDEN_NX2_IMAGE_TRACE") != nullptr; image_trace) {
        static std::vector<std::pair<u64, size_t>> logged;
        const u64 program = (u64{kepler_compute.regs.code_loc.address_high} << 32) |
                            kepler_compute.regs.code_loc.address_low;
        // views: texture buffers, image buffers, textures, images (in that order).
        const size_t image_buffers_begin = info.texture_buffer_descriptors.size();
        const size_t textures_begin = image_buffers_begin + info.image_buffer_descriptors.size();
        size_t images_begin = textures_begin;
        for (const auto& desc : info.texture_descriptors) images_begin += desc.count;
        if (std::none_of(logged.begin(), logged.end(), [&](const auto& e) { return e.first == program; }) &&
            logged.size() < 256) {
            for (size_t s = 0; s < info.storage_buffers_descriptors.size(); ++s) {
                const auto& sb = info.storage_buffers_descriptors[s];
                const GPUVAddr cbuf = sb.cbuf_index < cbufs.size() ? cbufs[sb.cbuf_index].Address() : 0;
                LOG_INFO(HW_GPU, "image trace: program {:#x} storage {} = c{}[{:#x}] holds {:#x}{}",
                         program, s, sb.cbuf_index, sb.cbuf_offset,
                         cbuf ? gpu_memory.Read<u64>(cbuf + sb.cbuf_offset) : 0,
                         sb.is_written ? " (written)" : "");
            }
        }
        for (size_t i = 0; i < views.size(); ++i) {
            const bool image_buffer = i >= image_buffers_begin && i < textures_begin;
            const bool image = i >= images_begin;
            if ((!image_buffer && !image) || logged.size() >= 256) continue;
            if (std::find(logged.begin(), logged.end(), std::make_pair(program, i)) != logged.end()) continue;
            logged.emplace_back(program, i);
            const auto& desc_integer = image ? info.image_descriptors[std::min(i - images_begin, info.image_descriptors.size() - 1)].is_integer
                                             : info.image_buffer_descriptors[i - image_buffers_begin].is_integer;
            if (views[i].id == VideoCommon::NULL_IMAGE_VIEW_ID) {
                LOG_INFO(HW_GPU, "image trace: program {:#x} {} {} handle {:#x} -> no image", program,
                         image ? "image" : "image buffer", i, views[i].index);
                continue;
            }
            const auto& view = texture_cache.GetImageView(views[i].id);
            LOG_INFO(HW_GPU, "image trace: program {:#x} {} {} handle {:#x} shader integer {} -> view {}x{}x{} format {} integer {} at {:#x}",
                     program, image ? "image" : "image buffer", i, views[i].index, desc_integer,
                     view.size.width, view.size.height, view.size.depth, static_cast<u32>(view.format),
                     VideoCore::Surface::IsPixelFormatInteger(view.format), view.gpu_addr);
        }
    }

    buffer_cache.UnbindComputeTextureBuffers();
    size_t index{};
    const auto add_buffer{[&](const auto& desc) {
        constexpr bool is_image = std::is_same_v<decltype(desc), const ImageBufferDescriptor&>;
        for (u32 i = 0; i < desc.count; ++i) {
            bool is_written{false};
            if constexpr (is_image) {
                is_written = desc.is_written;
            }
            ImageView& image_view = texture_cache.GetImageView(views[index].id);
            PixelFormat format{image_view.format};
            if constexpr (is_image) {
                if (const auto explicit_format{PixelFormatFromImageFormat(desc.format)}) {
                    format = *explicit_format;
                }
            }
            if (format == PixelFormat::Invalid) {
                // The handle selected no image (the null view has no format): bind it as words
                // instead of aborting in the format table.
                format = PixelFormat::R32_UINT;
            }
            if (static const bool image_trace = std::getenv("EDEN_NX2_IMAGE_TRACE") != nullptr;
                image_trace && is_written && image_view.GpuAddr() != 0) {
                const u64 program = (u64{kepler_compute.regs.code_loc.address_high} << 32) |
                                    kepler_compute.regs.code_loc.address_low;
                static const auto start = std::chrono::steady_clock::now();
                const u64 ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - start)
                                                    .count());
                if (VideoCommon::Nx2VolTrace::NoteWrittenBuffer(image_view.GpuAddr(),
                                                                image_view.BufferSize(), program, ms)) {
                    const auto device_addr = gpu_memory.GpuToCpuAddress(image_view.GpuAddr());
                    if (device_addr) {
                        VideoCommon::Nx2VolTrace::NoteWrittenDeviceRange(*device_addr,
                                                                         image_view.BufferSize());
                    }
                    std::array<u32, 8> words{};
                    gpu_memory.ReadBlockUnsafe(image_view.GpuAddr(), words.data(), sizeof(words));
                    LOG_INFO(HW_GPU,
                             "image trace: program {:#x} writes image buffer {:#x} (device {:#x}) size {:#x} handle "
                             "{:#x}; guest words there now {:08x}",
                             program, image_view.GpuAddr(), device_addr.value_or(0),
                             image_view.BufferSize(), views[index].index, fmt::join(words, " "));
                }
            }
            buffer_cache.BindComputeTextureBuffer(index, image_view.GpuAddr(),
                                                  image_view.BufferSize(), format,
                                                  is_written, is_image);
            ++index;
        }
    }};
    std::ranges::for_each(info.texture_buffer_descriptors, add_buffer);
    std::ranges::for_each(info.image_buffer_descriptors, add_buffer);

    buffer_cache.UpdateComputeBuffers();
    buffer_cache.BindHostComputeBuffers();
    if (buffer_cache.any_buffer_uploaded) {
        buffer_cache.runtime.PostCopyBarrier();
        buffer_cache.any_buffer_uploaded = false;
    }

    RescalingPushConstant rescaling;
    const VideoCommon::SamplerId* samplers_it{samplers.data()};
    const VideoCommon::ImageViewInOut* views_it{views.data()};
    PushImageDescriptors(texture_cache, guest_descriptor_queue, info, rescaling, samplers_it,
                         views_it);

    if (!is_built.load(std::memory_order::relaxed)) {
        // Wait for the pipeline to be built
        scheduler.Record([this](vk::CommandBuffer) {
            std::unique_lock lock{build_mutex};
            build_condvar.wait(lock, [this] { return is_built.load(std::memory_order::relaxed); });
        });
    }

    // Log compute pipeline binding
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogPipelineBind(true, "compute pipeline");
    }

    const DescriptorUpdateEntry* const descriptor_data{guest_descriptor_queue.UpdateData()};
    VkDeviceSize descriptor_buffer_offset{};
    u32 descriptor_buffer_chunk{};
    if (uses_descriptor_buffer) {
        const DescriptorBufferRing::Allocation alloc{
            descriptor_buffer_ring.Allocate(scheduler, descriptor_buffer_layout.size)};
        if (!alloc.host) {
            LOG_DEBUG(Render_Vulkan, "Failed to reserve descriptor memory, skipping dispatch");
            return false;
        }
        WriteDescriptorBuffer(device, descriptor_buffer_layout, descriptor_data, alloc.host);
        descriptor_buffer_offset = alloc.offset;
        descriptor_buffer_chunk = alloc.chunk;
    }

    const bool bind_descriptor_buffer{
        uses_descriptor_buffer && scheduler.UpdateDescriptorBufferChunk(descriptor_buffer_chunk)};

    const bool is_rescaling = !info.texture_descriptors.empty() || !info.image_descriptors.empty();
    scheduler.Record([this, descriptor_data, is_rescaling, descriptor_buffer_offset,
                      descriptor_buffer_chunk, bind_descriptor_buffer,
                      rescaling_data = rescaling.Data()](vk::CommandBuffer cmdbuf) {
        if (bind_descriptor_buffer) {
            const VkDescriptorBufferBindingInfoEXT binding_info{
                descriptor_buffer_ring.BindingInfo(descriptor_buffer_chunk)};
            cmdbuf.BindDescriptorBuffersEXT(binding_info);
        }
        if (!pipeline) {
            return;
        }
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        if (!descriptor_set_layout) {
            return;
        }
        if (is_rescaling) {
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                 RESCALING_LAYOUT_WORDS_OFFSET, sizeof(rescaling_data),
                                 rescaling_data.data());
        }
        if (uses_descriptor_buffer) {
            const u32 buffer_index{};
            cmdbuf.SetDescriptorBufferOffsetsEXT(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout,
                                                 0, buffer_index, descriptor_buffer_offset);
        } else if (uses_push_descriptor) {
            cmdbuf.PushDescriptorSetWithTemplateKHR(*descriptor_update_template, *pipeline_layout,
                                                    0, descriptor_data);
        } else {
            const VkDescriptorSet descriptor_set{descriptor_allocator.Commit()};
            const vk::Device& dev{device.GetLogical()};
            dev.UpdateDescriptorSet(descriptor_set, *descriptor_update_template, descriptor_data);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout, 0,
                                      descriptor_set, nullptr);
        }
    });
    return true;
}

} // namespace Vulkan
