// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

class Device;
class PipelineCache;
class PipelineStatistics;
class Scheduler;

class ComputePipeline {
public:
    explicit ComputePipeline(const Device& device, Scheduler& scheduler, vk::PipelineCache& pipeline_cache,
                             DescriptorPool& descriptor_pool,
                             GuestDescriptorQueue& guest_descriptor_queue,
                             DescriptorBufferRing& descriptor_buffer_ring,
                             Common::ThreadWorker* thread_worker,
                             PipelineStatistics* pipeline_statistics,
                             VideoCore::ShaderNotify* shader_notify, const Shader::Info& info,
                             vk::ShaderModule spv_module, u64 shader_hash);

    ComputePipeline& operator=(ComputePipeline&&) noexcept = delete;
    ComputePipeline(ComputePipeline&&) noexcept = delete;

    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(const ComputePipeline&) = delete;

    enum class ConfigureResult {
        Ready,
        Failed,
        Skipped, ///< Still compiling, and its outputs keep what the GPU wrote before.
    };

    /// With unbuilt_policy, a dispatch whose pipeline is still compiling may be skipped when
    /// every output already holds GPU writes (PipelineCache::WaitForUnbuiltDispatch).
    [[nodiscard]] ConfigureResult Configure(Tegra::Engines::KeplerCompute& kepler_compute,
                                            Tegra::MemoryManager& gpu_memory,
                                            Scheduler& scheduler, BufferCache& buffer_cache,
                                            TextureCache& texture_cache,
                                            PipelineCache* unbuilt_policy = nullptr);

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    /// Waits up to timeout for the build; true if it is done.
    [[nodiscard]] bool WaitBuilt(std::chrono::microseconds timeout) {
        std::unique_lock lock{build_mutex};
        return build_condvar.wait_for(lock, timeout,
                                      [this] { return is_built.load(std::memory_order::relaxed); });
    }

    bool IsBound() const noexcept {
        return static_cast<bool>(pipeline);
    }

private:
    /// Runs the build once, whoever gets here first, and releases everything waiting on it.
    void RunBuild();

    [[nodiscard]] bool OutputsHoldGpuData(
        const BufferCache& buffer_cache, const TextureCache& texture_cache,
        std::span<const VideoCommon::ImageViewInOut> views) const;

    const Device& device;
    vk::PipelineCache& pipeline_cache;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    Shader::Info info;
    u64 shader_hash{};
    u32 num_descriptor_entries{};

    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};

    vk::ShaderModule spv_module;
    vk::DescriptorSetLayout descriptor_set_layout;
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
    DescriptorBufferLayout descriptor_buffer_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    std::atomic_bool build_claimed{false};
    std::function<void()> build_task;
    VideoCore::ShaderNotify* build_notify{};
};

} // namespace Vulkan
