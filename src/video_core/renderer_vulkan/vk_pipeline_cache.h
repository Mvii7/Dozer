// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_shader_reuse.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
class Scheduler;

using VideoCommon::GraphicsEnvironment;
using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    /// texture_cache_locked: the caller holds texture_cache.mutex.
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline(bool texture_cache_locked = false);

    /// EDEN_NX2_DRAW_MERGE: a pipeline for `key` whose vertex shader reads constant buffer
    /// `cbuf` at instance * stride. Null when it cannot be built. Built here, on the GPU thread.
    [[nodiscard]] GraphicsPipeline* InstanceMergedPipeline(const GraphicsPipelineCacheKey& key,
                                                           u32 cbuf, u32 stride);

    [[nodiscard]] ComputePipeline* CurrentComputePipeline(u64* native_key = nullptr);

    /// Gives native draws their per-frame wait budget again.
    void TickFrame() noexcept;

    /// A native dispatch whose pipeline is still compiling, with outputs that already hold GPU
    /// writes: whether to go on with it (built in time, or EDEN_NX2_ASYNC_SKIP=0) or skip it.
    [[nodiscard]] bool WaitForUnbuiltDispatch(ComputePipeline& pipeline);

    /// What a native draw does when its pipeline is still compiling (EDEN_NX2_ASYNC_SKIP).
    enum class NativeDrawWait {
        Full,    ///< Waits until it is built.
        Bounded, ///< Waits within the frame's budget, then is skipped.
        Skip,    ///< Is skipped at once.
    };

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline);

    /// Whether the current draw may be skipped while its pipeline is being built.
    [[nodiscard]] bool CanSkipDraw() const noexcept;

    /// The stricter test for native draws once they have waited their budget.
    [[nodiscard]] bool CanSkipNativeDraw();

    void ChargeNativeDrawWait(std::chrono::steady_clock::duration waited) noexcept;

    void NoteNativeSkip(bool draw) noexcept;

    /// Builds the pipeline of graphics_key, which is not in graphics_cache. Returns false when it
    /// was left translating on a worker thread (the draw is skipped), true when graphics_cache
    /// now holds it (null if the build failed).
    bool BuildNewGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    /// Builds graphics_key's pipeline on the GPU thread if the shaders of an earlier pipeline
    /// fit it, so only the driver compile is left (on the pipeline workers). Null otherwise.
    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipelineFromReusedShaders();

    // Asynchronous translation of native graphics pipelines (NX2_SHADER_STUTTER.md).
    using GraphicsEnvironmentArray = std::array<GraphicsEnvironment, Maxwell::MaxShaderProgram>;

    /// A graphics pipeline translated on a worker thread, waiting for the GPU thread.
    struct TranslatedGraphicsPipeline {
        GraphicsPipelineCacheKey key;
        std::unique_ptr<GraphicsPipeline> pipeline; ///< Null when the build failed.
        GraphicsEnvironmentArray envs;              ///< For the disk cache.
        std::chrono::steady_clock::duration build_time{};
    };

    [[nodiscard]] bool CanTranslateAsynchronously() const;

    /// Queues the translation of graphics_key's pipeline on a worker thread. Returns a future
    /// that becomes ready when the result is waiting to be adopted, or nothing when the
    /// environments could not be detached from the engine.
    std::optional<std::future<void>> QueueGraphicsTranslation();

    /// Waits up to the wait budget for a queued translation. True if it was adopted in time.
    bool WaitForGraphicsTranslation(std::future<void>& done);

    /// Runs on a translation worker thread.
    void TranslateGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                   GraphicsEnvironmentArray envs, std::promise<void> done);

    /// Moves the pipelines finished by the workers into graphics_cache (GPU thread).
    void AdoptTranslatedGraphicsPipelines();

    void SerializeGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                   GraphicsEnvironmentArray envs);

    /// With `allow_translation` false, only builds pipelines from reused shaders (else null).
    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, bool allow_translation = true);

    // Reuse of translated shaders between graphics pipelines (vk_shader_reuse.h).

    /// Shaders an earlier pipeline built that fit `envs`, null if none (or reuse is off).
    [[nodiscard]] std::shared_ptr<const ReusableShaders> FindReusableShaders(
        const ShaderTranslationKey& key, std::span<Shader::Environment* const> envs,
        u64 pipeline_hash) const;

    /// Whether shaders with key's translation key exist (they may still not fit).
    [[nodiscard]] bool HasReusableShaders(const GraphicsPipelineCacheKey& key) const;

    /// Translates the programs and emits their SPIR-V. Null for programs with known issues.
    [[nodiscard]] std::shared_ptr<ReusableShaders> BuildGraphicsShaders(
        ShaderPools& pools, const ShaderTranslationKey& key,
        std::span<Shader::Environment* const> pipeline_envs, u64 pipeline_hash);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                           const ShaderInfo* shader);

    std::unique_ptr<ComputePipeline> CreateComputePipeline(ShaderPools& pools,
                                                           const ComputePipelineCacheKey& key,
                                                           Shader::Environment& env,
                                                           PipelineStatistics* statistics,
                                                           bool build_in_parallel);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};
    bool use_async_translation{};
    bool use_shader_reuse{};
    std::chrono::microseconds translation_wait_budget{};
    NativeDrawWait native_draw_wait{};
    std::chrono::microseconds native_draw_wait_budget{};
    /// What is left of native_draw_wait_budget this frame (GPU thread only).
    std::chrono::microseconds native_draw_wait_left{};
    /// Set by CurrentGraphicsPipeline for the lookup it makes.
    bool texture_cache_locked{};
    u64 skipped_native_draws{};
    u64 skipped_native_dispatches{};
    size_t num_translation_threads{};

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};
    u32 merge_cbuf{0xffffffffu};
    u32 merge_stride{};
    struct InstanceMerge {
        GraphicsPipelineCacheKey key;
        u32 cbuf{};
        u32 stride{};
        std::unique_ptr<GraphicsPipeline> pipeline;
    };
    std::vector<InstanceMerge> instance_merges;

    ::Common::unordered_map<ComputePipelineCacheKey, std::unique_ptr<ComputePipeline>> compute_cache;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;

    /// Declared before every thread pool, which all use it.
    ShaderReuseCache shader_reuse;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};

    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;

    /// Keys whose pipeline is being translated on a worker (GPU thread only).
    ::Common::unordered_set<GraphicsPipelineCacheKey> translating_graphics;
    /// Finished translations, handed from the workers to the GPU thread.
    std::mutex translated_mutex;
    std::vector<TranslatedGraphicsPipeline> translated_graphics;
    std::atomic<size_t> num_translated{};
    /// Declared last so it is destroyed first: its threads finish the translation they are
    /// running (and drop the queued ones) while everything they use is still alive.
    Common::ThreadWorker translation_workers;
};

} // namespace Vulkan
