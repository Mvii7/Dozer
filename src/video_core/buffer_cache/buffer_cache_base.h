// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "common/container/unordered_map.h"
#include <boost/container/static_vector.hpp>
#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/spin_recursive_mutex.h"
#include "common/div_ceil.h"
#include "common/literals.h"
#include "common/lru_cache.h"
#include "common/range_sets.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/slot_vector.h"
#include "video_core/buffer_cache/buffer_base.h"
#include "video_core/buffer_cache/nx2_draw_memo.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/buffer_cache/virtual_range_cache.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/nx2.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_reactive_flush.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

/// A 0/1/2 switch (unset or another value: 1), read at the first call: not at static
/// initialization, before main loads nx2_switches.txt into the environment (yuzu main.cpp), and
/// without a function-local static's thread-safe initialization check on every call (MSVC: a
/// thread-local read). Two threads racing on the first call both compute the same value.
inline int ReadNx2Mode(std::atomic<s8>& mode, const char* name) {
    s8 value = mode.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv(name);
        value = static_cast<s8>(env == nullptr ? 1 : env[0] == '0' ? 0 : env[0] == '2' ? 2 : 1);
        mode.store(value, std::memory_order_relaxed);
    }
    return value;
}
// EDEN_NX2_VERTEX_SKIP (BufferCache::VertexSkipEnabled) and EDEN_NX2_SYNC_MEMO (SynchronizeBuffer):
// per-draw queries, read through ReadNx2Mode (as function-local statics every query paid a
// thread-safe initialization check: VertexSkipEnabled alone ~0.8% of the split back thread).
inline bool Nx2VertexSkip() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_VERTEX_SKIP") != 0;
}
/// EDEN_NX2_SYNC_MEMO=0 synchronizes every range as before.
inline bool Nx2SyncMemo() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_SYNC_MEMO") != 0;
}
/// EDEN_NX2_SYNC_SETS (with EDEN_NX2_SYNC_MEMO): ranges are remembered as the 4 KiB pages they
/// left clean, four to a set (SyncMemoSets). =0 the direct-mapped byte ranges as before; =2 still
/// walks a remembered range and reports any upload found there (as EDEN_NX2_SYNC_SKIP=2).
inline int Nx2SyncSets() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_SYNC_SETS");
}
/// EDEN_NX2_SSBO_FAST: a storage buffer binding's layout size from a remembered run of one page
/// kind (LayoutRunMemo), its device address from the aligned one's when both lie on one 4 KiB
/// page, and no multi-range query for a range within one 4 KiB page (one segment at most). =0
/// as before; =2 also does each the old way and logs any difference ("EDEN_NX2_SSBO_FAST: ...").
inline int Nx2SsboFast() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_SSBO_FAST");
}
/// EDEN_NX2_GPU_WRITTEN_FILTER: the GPU-written ranges and the download range sets answer queries
/// far from their ranges from a summary instead of their trees (SummarizedRangeSet: hull, and for
/// gpu_modified_ranges a 4 KiB page bitmap). =0 as before; =2 also asks the trees and counts any
/// disagreement (the "GPU-written ranges" line, every 2^20 queries, shows it).
inline int Nx2GpuWrittenFilter() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_GPU_WRITTEN_FILTER");
}
/// EDEN_NX2_UNIFORM_SYNC_SKIP: EDEN_NX2_LIGHT_SYNC_SKIP for the uniform way's light draws too
/// (GraphicsPipeline::ConfigureUniforms; same anchor chain, frame and epoch guard). =0 as before;
/// =2 synchronizes the streams it would skip and logs any that uploaded.
/// EDEN_NX2_UBO_KEEP: a uniform buffer bound to the same stage slot and range as the slot's last
/// bind takes that bind's slice again without reading guest memory while nothing could have
/// changed the range (Common::Nx2SyncSerial, BufferCache::uniform_write_log). =0 as before; =2
/// reads and compares anyway, binds as before and logs any range that changed.
inline int Nx2UniformKeep() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_UBO_KEEP");
}
inline int Nx2UniformSyncSkip() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_UNIFORM_SYNC_SKIP");
}
/// EDEN_NX2_STORAGE_KEEP: UpdateStorageBuffers leaves a stage whose storage bindings are as it
/// resolved them last (no bind or unbind since, page_table, the GPU mappings and the address space
/// unchanged, no binding over several segments): FindBuffer would return the same buffers and
/// ResolveMultiRangeStorage no segments again. =0 as before; =2 resolves anyway and logs a
/// binding that resolved differently.
inline int Nx2StorageKeep() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_STORAGE_KEEP");
}
/// EDEN_NX2_UBO_BIND_SKIP (elBart0 0015): BindGraphicsUniformBuffer keeps a binding resolved to
/// the same device address and size whose buffer FindBuffer would return again, instead of
/// clearing it for UpdateUniformBuffers to find; an unmapped address binds NULL_BINDING. =0 as
/// before; =2 only counts them ("Uniform buffer binds" line, every 2^22 binds).
inline int Nx2UboBindSkip() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_UBO_BIND_SKIP");
}
/// EDEN_NX2_INLINE_FIX (Duck912 4a6b890171): the inline index buffer, when it grows or the
/// collector takes it, is deleted as any other buffer (DeleteBuffer: unregistered from page_table
/// and the LRU list, destroyed after the GPU is done with it). =0 erases its slot as before, which
/// left page_table entries and an LRU item naming a freed slot and destroyed the buffer at once.
inline bool Nx2InlineFix() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_INLINE_FIX") != 0;
}
/// EDEN_NX2_VERTEX_FAST=0: UpdateVertexBuffer tests page_table on every memo hit and keeps no
/// per-stream memo, and vertex slots are reached through VertexBufferSlot's index check, as before.
inline bool Nx2VertexFast() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_VERTEX_FAST") != 0;
}
/// EDEN_NX2_VERTEX_CLEAN (with EDEN_NX2_VERTEX_SKIP): a stream its registers disable (enable bit
/// clear or an empty range) is resolved to NULL_BINDING once and its VertexBuffer<n> flag cleared.
/// BindHostVertexBuffers visits enabled streams only, so that flag was never cleared before and
/// every draw with Dirty::VertexBuffers set resolved each disabled stream again (~18 of 32 a draw
/// in title 4). Those registers are in the stream's dirty block (dirty_flags.cpp), so writing any
/// of them sets the flag again; InvalidateCommandBufferState does too.
/// =0 as before; =2 also checks, each time the streams are resolved again, that no cleaned stream
/// got enabled without its flag (logs "EDEN_NX2_VERTEX_CLEAN: stream N enabled without its dirty
/// flag", first 16, and resolves it).
inline int Nx2VertexClean() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_VERTEX_CLEAN");
}
/// EDEN_NX2_LIGHT_SYNC_SKIP: in a plain light draw (RasterizerVulkan::TryLightDraw: nothing but
/// stream/index/draw registers changed, no memory changed but same-byte constant buffer writes,
/// same frame) an enabled stream whose flag is clean keeps the binding the draw before
/// synchronized and touched; BindHostVertexBuffers skips its TouchBuffer and SynchronizeBuffer
/// while no CPU-modified or untracked page can have appeared in the stream's range since its
/// last synchronization there (vertex_synced_epoch, MemoryTracker::UnsetSince), and does both as
/// before otherwise. =0 as before; =2 synchronizes the streams it would skip anyway and logs any
/// that uploaded ("EDEN_NX2_LIGHT_SYNC_SKIP: clean stream N of a plain light draw uploaded"; none
/// expected).
inline int Nx2LightSyncSkip() {
    static std::atomic<s8> mode{-1};
    return ReadNx2Mode(mode, "EDEN_NX2_LIGHT_SYNC_SKIP");
}

using BufferId = Common::SlotId;

using VideoCore::Surface::PixelFormat;
using namespace Common::Literals;

#ifdef __APPLE__
constexpr u32 NUM_VERTEX_BUFFERS = 16;
#else
constexpr u32 NUM_VERTEX_BUFFERS = 32;
#endif
constexpr u32 NUM_TRANSFORM_FEEDBACK_BUFFERS = 4;
constexpr u32 NUM_GRAPHICS_UNIFORM_BUFFERS = 18;
constexpr u32 NUM_COMPUTE_UNIFORM_BUFFERS = 8;
// Native compute shaders bind up to 18 global memory ranges; the enable masks are 32 bits wide.
constexpr u32 NUM_STORAGE_BUFFERS = 32;
constexpr u32 NUM_TEXTURE_BUFFERS = 32;
constexpr u32 NUM_STAGES = 5;

static_assert(NUM_GRAPHICS_UNIFORM_BUFFERS <= 32, "fast bitmask must fit u32");

using UniformBufferSizes = std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>;
using ComputeUniformBufferSizes = std::array<u32, NUM_COMPUTE_UNIFORM_BUFFERS>;

enum class ObtainBufferSynchronize : u32 {
    NoSynchronize = 0,
    FullSynchronize = 1,
    SynchronizeNoDirty = 2,
};

enum class ObtainBufferOperation : u32 {
    DoNothing = 0,
    MarkAsWritten = 1,
    DiscardWrite = 2,
    MarkQuery = 3,
};

static constexpr BufferId NULL_BUFFER_ID{0};
static constexpr u32 DEFAULT_SKIP_CACHE_SIZE = static_cast<u32>(4_KiB);

struct Binding {
    DAddr device_addr{};
    GPUVAddr gpu_addr{};
    u32 size{};
    BufferId buffer_id;
    u32 segment_first{};
    u32 segment_count{};
};

struct MultiRangeSegment {
    BufferId buffer_id;
    DAddr device_addr{};
    u32 size{};
};

struct TextureBufferBinding : Binding {
    PixelFormat format;
};

static constexpr Binding NULL_BINDING{
    .device_addr = 0,
    .size = 0,
    .buffer_id = NULL_BUFFER_ID,
};

template <typename Buffer>
struct HostBindings {
    boost::container::static_vector<Buffer*, NUM_VERTEX_BUFFERS> buffers;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> offsets;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> sizes;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> strides;
    u32 min_index{NUM_VERTEX_BUFFERS};
    u32 max_index{0};
};

class BufferCacheChannelInfo : public ChannelInfo {
public:
    BufferCacheChannelInfo() = delete;
    BufferCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept : ChannelInfo(state) {}
    BufferCacheChannelInfo(const BufferCacheChannelInfo& state) = delete;
    BufferCacheChannelInfo& operator=(const BufferCacheChannelInfo&) = delete;

    Binding index_buffer;
    std::array<Binding, NUM_VERTEX_BUFFERS> vertex_buffers;
    std::array<std::array<Binding, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES> uniform_buffers;
    std::array<std::array<Binding, NUM_STORAGE_BUFFERS>, NUM_STAGES> storage_buffers;
    std::array<std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS>, NUM_STAGES> texture_buffers;
    std::array<Binding, NUM_TRANSFORM_FEEDBACK_BUFFERS> transform_feedback_buffers;
    Binding count_buffer_binding;
    Binding indirect_buffer_binding;

    std::array<Binding, NUM_COMPUTE_UNIFORM_BUFFERS> compute_uniform_buffers;
    std::array<Binding, NUM_STORAGE_BUFFERS> compute_storage_buffers;
    std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS> compute_texture_buffers;

    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    u32 enabled_compute_uniform_buffer_mask = 0;

    const UniformBufferSizes* uniform_buffer_sizes{};
    const ComputeUniformBufferSizes* compute_uniform_buffer_sizes{};

    std::array<u32, NUM_STAGES> enabled_storage_buffers{};
    std::array<u32, NUM_STAGES> written_storage_buffers{};
    u32 enabled_compute_storage_buffers = 0;
    u32 written_compute_storage_buffers = 0;

    u32 total_graphics_storage_buffers = 0;
    u32 total_compute_storage_buffers = 0;

    std::array<u32, NUM_STAGES> enabled_texture_buffers{};
    std::array<u32, NUM_STAGES> written_texture_buffers{};
    std::array<u32, NUM_STAGES> image_texture_buffers{};
    u32 enabled_compute_texture_buffers = 0;
    u32 written_compute_texture_buffers = 0;
    u32 image_compute_texture_buffers = 0;

    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_hits{};
    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_shots{};

    u32 uniform_buffer_skip_cache_size = DEFAULT_SKIP_CACHE_SIZE;

    bool has_deleted_buffers = false;

    /// EDEN_NX2_STORAGE_KEEP: what each stage's storage bindings were last resolved under.
    struct StorageResolved {
        u64 layout_epoch;
        u64 map_epoch;
        size_t owner;
        bool valid;
    };
    std::array<StorageResolved, NUM_STAGES> storage_resolved{};

    std::array<u32, NUM_STAGES> dirty_uniform_buffers{};
    std::array<u32, NUM_STAGES> fast_bound_uniform_buffers{};
    std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>
        uniform_buffer_binding_sizes{};
};

template <class P>
class BufferCache : public VideoCommon::ChannelSetupCaches<BufferCacheChannelInfo> {
    // Page size for caching purposes.
    // This is unrelated to the CPU page size and it can be changed as it seems optimal.
#ifdef YUZU_LEGACY
    static constexpr u32 CACHING_PAGEBITS = 12;
#else
    static constexpr u32 CACHING_PAGEBITS = 16;
#endif
    static constexpr u64 CACHING_PAGESIZE = u64{1} << CACHING_PAGEBITS;

    static constexpr bool IS_OPENGL = P::IS_OPENGL;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS =
        P::HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT =
        P::HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = P::NEEDS_BIND_UNIFORM_INDEX;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = P::NEEDS_BIND_STORAGE_INDEX;
    static constexpr bool USE_MEMORY_MAPS = P::USE_MEMORY_MAPS;
    static constexpr bool SEPARATE_IMAGE_BUFFERS_BINDINGS = P::SEPARATE_IMAGE_BUFFER_BINDINGS;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = P::USE_MEMORY_MAPS_FOR_UPLOADS;

#ifdef YUZU_LEGACY
    static constexpr s64 TARGET_THRESHOLD = 3_GiB;
#else
    static constexpr s64 TARGET_THRESHOLD = 4_GiB;
#endif

    static constexpr s64 DEFAULT_EXPECTED_MEMORY = 512_MiB;
    static constexpr s64 DEFAULT_CRITICAL_MEMORY = 1_GiB;

    // Debug Flags.

    static constexpr bool DISABLE_DOWNLOADS = true;

    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    using Runtime = typename P::Runtime;
    using Buffer = typename P::Buffer;
    using Async_Buffer = typename P::Async_Buffer;
    using MemoryTracker = typename P::MemoryTracker;

    struct OverlapResult {
        boost::container::small_vector<BufferId, 16> ids;
        DAddr begin;
        DAddr end;
        bool has_stream_leap = false;
    };

public:
    explicit BufferCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, Runtime& runtime_);

    ~BufferCache();

    void TickFrame();

    bool BindMultiRangeStorage(const Binding& binding, bool is_written,
                               std::span<const MultiRangeSegment> pool);

    void ResolveMultiRangeStorage(Binding& binding, bool is_written,
                                  std::vector<MultiRangeSegment>& pool);

    void UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size);

    /// `engine_write`: the GPU's own engines wrote it (inline constant buffer data,
    /// MemoryManager::FlushCaching), not the guest CPU.
    void WriteMemory(DAddr device_addr, u64 size, bool engine_write = false);

    /// WriteMemory for each range, in order (the pre-draw invalidation of engine writes).
    void WriteMemoryRanges(std::span<const std::pair<DAddr, std::size_t>> ranges);

    void CachedWriteMemory(DAddr device_addr, u64 size);

    bool OnCPUWrite(DAddr device_addr, u64 size);

    void DownloadMemory(DAddr device_addr, u64 size);

    /// EDEN_NX2_ASYNC_READBACK: DownloadMemory for a guest CPU read, on the GPU thread, without
    /// waiting for the host GPU. Returns where the read's bytes are (the tick to wait for and the
    /// newest download the read lands), nothing once guest memory is up to date there. The bytes
    /// stay GPU-modified until FinishReadDownloads writes them.
    std::optional<VideoCore::ReadPoint> DownloadMemoryForRead(DAddr device_addr, u64 size);

    /// Waits for a tick DownloadMemoryForRead returned, on any thread without the lock.
    void WaitForReadDownload(u64 tick) {
        if constexpr (!IS_OPENGL && USE_MEMORY_MAPS) {
            runtime.WaitForRead(tick);
        }
    }

    /// Writes the read downloads over the range (up to max_seq), and those before them, to guest
    /// memory. `lock` (the caller's, of `mutex`) is let go while waiting for the host GPU; without
    /// it the wait holds the lock. A Gpu landing submits what is not submitted yet; a Writer one
    /// takes its bytes out of those instead.
    void FinishReadDownloads(DAddr device_addr, u64 size, VideoCommon::Nx2AsyncReadback::Landing who,
                             std::unique_lock<Common::SpinRecursiveMutex>* lock = nullptr,
                             u64 max_seq = ~u64{0});

    /// A GPU write the cache doesn't record (a query result copy, QueryCacheRuntime::SyncValues;
    /// guest memory gets the value when the query cache writes it): read downloads taken before
    /// it no longer bring the newest bytes, nor are early-landed ones the GPU's any more.
    void OutdateReadDownloads(DAddr device_addr, u64 size) {
        if (!read_downloads.empty()) [[unlikely]] {
            for (ReadDownload& download : read_downloads) {
                download.clean_ranges.Subtract(device_addr, size);
            }
            read_download_ranges.Subtract(device_addr, size);
        }
        if (!early_landed_ranges.Empty()) [[unlikely]] {
            early_landed_ranges.Subtract(device_addr, size);
        }
    }

    /// A write to guest memory from the GPU's side, after FinishReadDownloads landed what was
    /// under it.
    void NoteGpuSideWrite(DAddr device_addr, u64 size) {
        NoteGuestChange(device_addr, size);
    }

    /// EDEN_NX2_ASYNC_READBACK=2: early downloads brought bytes here no write changed since (a
    /// read of them is flushed, to be checked).
    [[nodiscard]] bool EarlyLandedIn(DAddr device_addr, u64 size) const {
        return early_landed_ranges.Intersects(device_addr, size);
    }

    /// EDEN_NX2_EARLY_DOWNLOADS: a guest CPU read of [begin, end) (GetFlushArea), which found
    /// GPU writes there or not.
    void NoteCpuReadBack(DAddr begin, DAddr end, bool gpu_modified) {
        NoteReadBack(begin, end, gpu_modified);
    }

    /// EDEN_NX2_ASYNC_READBACK=2: compares guest memory with the checks of the reads up to seq
    /// over the range, once their downloads landed.
    void CompareReadChecks(DAddr device_addr, u64 size, u64 seq);

    /// EDEN_NX2_EARLY_DOWNLOADS: records the early downloads due and lands those the host GPU
    /// ran (after a dispatch, at fences and frames). dispatch: EDEN_NX2_EARLY_SUBMIT may submit.
    void NoteReadBackPoint(bool dispatch = false);

    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(DAddr device_addr, u64 size);

    bool InlineMemory(DAddr dest_address, size_t copy_size, std::span<const u8> inlined_buffer);

    /// EDEN_NX2_RESET_BATCH: uploads gathered by InlineMemoryBatched and not recorded yet, all
    /// into one buffer, each after the previous one's end.
    struct InlineBatch {
        static constexpr size_t MaxBytes = 256 * 1024;
        BufferId buffer_id{};
        bool can_reorder{}; ///< Every copy's CanReorderUpload answer (all the same)
        std::vector<BufferCopy> copies; ///< src_offset: where the copy's bytes start in `bytes`
        std::vector<u8> bytes;
    };

    /// InlineMemory with the upload's copy left in `batch` (FlushInlineBatch records it).
    bool InlineMemoryBatched(DAddr dest_address, size_t copy_size,
                             std::span<const u8> inlined_buffer, InlineBatch& batch);

    /// Records the uploads gathered in `batch`: one staging buffer, one copy command between
    /// the two barriers. Returns whether there were any.
    bool FlushInlineBatch(InlineBatch& batch);

    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size);

    void DisableGraphicsUniformBuffer(size_t stage, u32 index);

    void UpdateGraphicsBuffers(bool is_indexed);

    /// EDEN_NX2_LIGHT_DRAW: updates the index, vertex and indirect buffers only, what a draw
    /// that changed nothing else needs. Returns false when a buffer was deleted on the way (the
    /// other bindings may have named it): the draw then needs the full setup.
    [[nodiscard]] bool UpdateGeometryBuffers(bool is_indexed);

    void UpdateComputeBuffers();

    void BindHostGeometryBuffers(bool is_indexed);

    void BindHostStageBuffers(size_t stage);

    void BindHostComputeBuffers();

    void SetUniformBuffersState(const std::array<u32, NUM_STAGES>& mask,
                                const UniformBufferSizes* sizes);

    void SetComputeUniformBufferState(u32 mask, const ComputeUniformBufferSizes* sizes);

    void UnbindGraphicsStorageBuffers(size_t stage);

    bool BindGraphicsStorageBuffer(size_t stage, size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                   bool is_written);

    void UnbindGraphicsTextureBuffers(size_t stage);

    void BindGraphicsTextureBuffer(size_t stage, size_t tbo_index, GPUVAddr gpu_addr, u32 size,
                                   PixelFormat format, bool is_written, bool is_image);

    void UnbindComputeStorageBuffers();

    void BindComputeStorageBuffer(size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                  bool is_written);

    void UnbindComputeTextureBuffers();

    void BindComputeTextureBuffer(size_t tbo_index, GPUVAddr gpu_addr, u32 size, PixelFormat format,
                                  bool is_written, bool is_image);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainBuffer(GPUVAddr gpu_addr, u32 size,
                                                       ObtainBufferSynchronize sync_info,
                                                       ObtainBufferOperation post_op);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainCPUBuffer(DAddr gpu_addr, u32 size,
                                                          ObtainBufferSynchronize sync_info,
                                                          ObtainBufferOperation post_op);

    /// Profiling: the buffer of vertex stream `index`'s current binding (32: the index buffer).
    [[nodiscard]] BufferId BoundGeometryBuffer(u32 index) const noexcept {
        if (index == 32) {
            return channel_state->index_buffer.buffer_id;
        }
        return index < NUM_VERTEX_BUFFERS ? channel_state->vertex_buffers[index].buffer_id
                                          : NULL_BUFFER_ID;
    }

    void FlushCachedWrites();

    /// Return true when there are uncommitted buffers to be downloaded
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;

    void AccumulateFlushes();

    /// Return true when the caller should wait for async downloads
    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept;

    /// Commit asynchronous downloads
    void CommitAsyncFlushes();
    void CommitAsyncFlushesHigh();

    /// Pop asynchronous downloads
    void PopAsyncFlushes();
    void PopAsyncBuffers();

    bool DMACopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount);

    bool DMAClear(GPUVAddr src_address, u64 amount, u32 value);

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(DAddr addr, size_t size);

    /// Whether all of [addr, addr + size) holds GPU writes.
    [[nodiscard]] bool IsRegionFullyGpuModified(DAddr addr, size_t size) const;

    /// Whether every buffer the bound compute program writes already holds GPU writes in full
    /// (after UpdateComputeBuffers, before BindHostComputeBuffers marks them written).
    [[nodiscard]] bool ComputeWritesHoldGpuData() const;

    /// The guest CPU reads [addr, addr + size) (pages its reads are tracked on): without
    /// AsyncBufferDownloads, GPU writes there are still copied back at fences from now on.
    /// Returns whether part of it was not noted before.
    bool NoteCpuRead(DAddr addr, size_t size);

    /// Read traps (ZendNevs): with reactive flushing off, GPU writes the CPU has not read yet are
    /// trapped instead (QueueDownload), and a trapped read flushes them (GetFlushArea in the
    /// rasterizer). EDEN_NX2_READ_TRAPS=0 turns them off.
    [[nodiscard]] bool ReadTrapsActive() const {
        static const bool disabled = [] {
            const char* value = std::getenv("EDEN_NX2_READ_TRAPS");
            return value != nullptr && value[0] == '0';
        }();
        return !disabled && !Settings::values.use_reactive_flushing.GetValue() &&
               !AsyncBufferDownloads();
    }

    /// EDEN_NX2_RX_COALESCE: a fence leaves the copies back pending so far for a later one; they
    /// are kept apart, so reads that fetch them count (NoteDeferredRead).
    void DeferCopyBacks() {
        if (uncommitted_gpu_modified_ranges.Empty()) {
            return;
        }
        // Into the last held back set while nothing came after it, so the sets stay few.
        if (deferred_copy_back_sets != 0 &&
            deferred_copy_back_sets == committed_gpu_modified_ranges.size()) {
            auto& held = committed_gpu_modified_ranges.back();
            uncommitted_gpu_modified_ranges.ForEach(
                [&held](DAddr start, DAddr end) { held.Add(start, end - start); });
            uncommitted_gpu_modified_ranges.Clear();
            return;
        }
        AccumulateFlushes();
        deferred_copy_back_sets = committed_gpu_modified_ranges.size();
    }

    /// Reads that fetched data a fence held back, since the last call.
    [[nodiscard]] u64 TakeDeferredReads() noexcept {
        return std::exchange(deferred_reads, 0);
    }

    /// The guest CPU read these pages, which used up their read traps: the next GPU write here
    /// arms them again, so a read after it flushes instead of finding data the fences have not
    /// copied back yet. EDEN_NX2_TRAP_REARM=0: they stay unarmed after their first read.
    void ForgetReadTraps(DAddr device_addr, u64 size) {
        if (Nx2Reactive::TrapRearm()) {
            read_trapped_ranges.Subtract(device_addr, size);
        }
    }

    /// A read trap found GPU writes here not in guest memory yet. When all of them are in copies
    /// back already committed at a fence, waits for the fence thread to land them instead of a
    /// sync flush (EDEN_NX2_COPYBACK_WAIT=0: never). Returns whether none is left.
    bool WaitForCopyBack(std::unique_lock<Common::SpinRecursiveMutex>& lock, DAddr addr,
                         u64 size);

    /// 4 KiB pages in the CPU-read ranges (EDEN_NX2_REACTIVE_STATS; walks them).
    [[nodiscard]] u64 CpuReadPages() const;

    /// Return true when a region is registered on the cache
    [[nodiscard]] bool IsRegionRegistered(DAddr addr, size_t size);

    /// Return true when a CPU region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(DAddr addr, size_t size);

    void SetDrawIndirect(
        const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect_) {
        current_draw_indirect = current_draw_indirect_;
    }

    /// Whether a draw or dispatch has written buffers (MarkWrittenBuffer) since the last call:
    /// an indirect draw must then wait for those writes before it reads its arguments.
    [[nodiscard]] bool TakeIndirectBarrier() {
        return std::exchange(written_since_indirect, false);
    }

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectCount();

    /// The vertex streams BindHostVertexBuffers binds (a non-null binding of non-zero size): back
    /// stages, cut c, the geometry mirror's live streams (DrawToC::vertex_live).
    [[nodiscard]] u32 EnabledVertexBuffersMask() const noexcept {
        return enabled_vertex_buffers_mask;
    }

    /// Back stages, cut c (geo-markusage-1): marks every enabled vertex stream's range used, for
    /// each draw packet of B's. BindHostVertexBuffers marks only the streams it binds again (dirty
    /// ones); B does not see the command buffer C records into, so a stream bound in an older one
    /// would go unmarked there once TickFrame reset its buffer's marks, and an upload to it could
    /// be moved ahead of the draws that read it (CanReorderUpload). A stream whose range its
    /// buffer tracked in the tracker's current generation (vertex_usage_marks) is only stamped:
    /// TickFrame's reset starts a new generation, the next packet tracks it again.
    void MarkVertexStreamsUsed();

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectBuffer();

    template <typename Func>
    void BufferOperations(Func&& func) {
        do {
            channel_state->has_deleted_buffers = false;
            func();
        } while (channel_state->has_deleted_buffers);
    }

    Common::SpinRecursiveMutex mutex;
    Runtime& runtime;
    bool any_buffer_uploaded = false;
    bool written_since_indirect = false; ///< See TakeIndirectBarrier
    /// EDEN_NX2_LIGHT_SYNC_SKIP: set by the rasterizer around a plain light draw's
    /// BindHostGeometryBuffers only.
    bool plain_light_draw = false;
    /// EDEN_NX2_UNIFORM_SYNC_SKIP: the same, set by GraphicsPipeline::ConfigureUniforms.
    bool uniform_light_draw = false;

    /// EDEN_NX2_UBO_KEEP: the last ranges of guest memory written with a notice to this cache
    /// (constant buffer and inline data, DMA, CPU writes the caches watch), in order.
    struct UniformWriteLog {
        static constexpr u64 SIZE = 128;
        std::array<std::pair<DAddr, DAddr>, SIZE> ranges{};
        u64 count = 0;
    } uniform_write_log;
    u64 uniform_keeps = 0;           ///< Binds EDEN_NX2_UBO_KEEP made without reading
    u64 uniform_keep_checks = 0;     ///< =2: keeps compared with guest memory instead
    u64 uniform_keep_mismatches = 0; ///< =2: of them, ranges that had changed
    void NoteUniformWrite(DAddr device_addr, u64 size) {
        UniformWriteLog& log = uniform_write_log;
        log.ranges[log.count % UniformWriteLog::SIZE] = {device_addr, device_addr + size};
        ++log.count;
    }
    /// Whether a write noted since the log counted `since` may have reached the range.
    [[nodiscard]] bool UniformWrittenSince(u64 since, DAddr device_addr, u64 size) const {
        const UniformWriteLog& log = uniform_write_log;
        if (log.count - since > UniformWriteLog::SIZE) {
            return true;
        }
        const DAddr end = device_addr + size;
        for (u64 i = since; i < log.count; ++i) {
            const auto& [begin, write_end] = log.ranges[i % UniformWriteLog::SIZE];
            if (begin < end && device_addr < write_end) {
                return true;
            }
        }
        return false;
    }
    /// EDEN_NX2_VERTEX_CLEAN=2: streams whose flag UpdateVertexBuffer cleared as disabled.
    u32 vertex_clean_mask = 0;

    /// EDEN_NX2_GPU_PROFILE: counts the vertex streams the last BindHostVertexBuffers bound, once
    /// its draw is recorded (the rasterizer calls it then, with the profile on; a light draw that
    /// falls back binds them again in the full setup and is counted once, as set up there).
    void CommitVertexStreamNote();

private:
    /// Marks a written range CPU-modified; an engine write keeps its pages tracked.
    void MarkCpuWritten(DAddr device_addr, u64 size, bool engine_write);

    template <typename Func>
    static void ForEachEnabledBit(u32 enabled_mask, Func&& func) {
        for (u32 index = 0; enabled_mask != 0; ++index, enabled_mask >>= 1) {
            const int disabled_bits = std::countr_zero(enabled_mask);
            index += disabled_bits;
            enabled_mask >>= disabled_bits;
            func(index);
        }
    }

    template <typename Func>
    void ForEachBufferInRange(DAddr device_addr, u64 size, Func&& func) {
        const u64 page_end = Common::DivCeil(device_addr + size, CACHING_PAGESIZE);
        for (u64 page = device_addr >> CACHING_PAGEBITS; page < page_end;) {
            const BufferId buffer_id = page_table[page];
            if (!buffer_id) {
                ++page;
                continue;
            }
            Buffer& buffer = slot_buffers[buffer_id];
            func(buffer_id, buffer);

            const DAddr end_addr = buffer.CpuAddr() + buffer.SizeBytes();
            page = Common::DivCeil(end_addr, CACHING_PAGESIZE);
        }
    }

    static bool IsRangeGranular(DAddr device_addr, size_t size) {
        return (device_addr & ~Core::DEVICE_PAGEMASK) ==
               ((device_addr + size) & ~Core::DEVICE_PAGEMASK);
    }

    void RunGarbageCollector(bool aggressive_gc);

    void BindHostIndexBuffer();

    void BindHostVertexBuffers();

    /// EDEN_NX2_GPU_PROFILE: notes the streams a draw binds (BindHostVertexBuffers).
    void NoteVertexStreams(u32 enabled_mask);

    void BindHostDrawIndirectBuffers();

    void BindHostGraphicsUniformBuffers(size_t stage);

    void BindHostGraphicsUniformBuffer(size_t stage, u32 index, u32 binding_index, bool needs_bind);

    void BindHostGraphicsStorageBuffers(size_t stage);

    void BindHostGraphicsTextureBuffers(size_t stage);

    void BindHostTransformFeedbackBuffers();

    void BindHostVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride);

    void BindHostComputeUniformBuffers();

    void BindHostComputeStorageBuffers();

    void BindHostComputeTextureBuffers();

    void DoUpdateGraphicsBuffers(bool is_indexed);

    void DoUpdateComputeBuffers();

    void UpdateIndexBuffer();

    void UpdateVertexBuffers();

    void UpdateVertexBuffer(u32 index);

    void UpdateDrawIndirect();

    void UpdateUniformBuffers(size_t stage);

    void UpdateStorageBuffers(size_t stage);

    void UpdateTextureBuffers(size_t stage);

    void UpdateTransformFeedbackBuffers();

    void UpdateTransformFeedbackBuffer(u32 index);

    void UpdateComputeUniformBuffers();

    void UpdateComputeStorageBuffers();

    void UpdateComputeTextureBuffers();

    void MarkWrittenBuffer(BufferId buffer_id, DAddr device_addr, u32 size, bool needs_sync = false);

    [[nodiscard]] BufferId FindBuffer(DAddr device_addr, u32 size, bool sparse_compatible);

    void SynchronizeBufferWrites(Buffer& buffer);

    [[nodiscard]] OverlapResult ResolveOverlaps(DAddr device_addr, u32 wanted_size);

    void JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);

    [[nodiscard]] BufferId CreateBuffer(DAddr device_addr, u32 wanted_size,
                                        bool sparse_compatible);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    void TouchBuffer(Buffer& buffer, BufferId buffer_id) noexcept;

    bool SynchronizeBuffer(Buffer& buffer, DAddr device_addr, u32 size);

    void UploadMemory(Buffer& buffer, u64 total_size_bytes, u64 largest_copy,
                      std::span<BufferCopy> copies);

    void ImmediateUploadMemory(Buffer& buffer, u64 largest_copy,
                               std::span<const BufferCopy> copies);

    void MappedUploadMemory(Buffer& buffer, u64 total_size_bytes, std::span<BufferCopy> copies);

    void DownloadBufferMemory(Buffer& buffer_id);

    void DownloadBufferMemory(Buffer& buffer_id, DAddr device_addr, u64 size);

    /// Downloads the GPU-written data of several buffers with one wait for the GPU.
    void DownloadBuffersMemory(std::span<const BufferId> buffer_ids);

    void DeleteBuffer(BufferId buffer_id, bool do_not_mark = false);

    [[nodiscard]] Binding StorageBufferBinding(GPUVAddr ssbo_addr, u32 cbuf_index,
                                               bool is_written) const;

    /// StorageBufferBinding from the two words already read at ssbo_addr (gpu_addr non-zero).
    [[nodiscard]] Binding StorageBufferBindingAt(GPUVAddr gpu_addr, u64 next_qword,
                                                 u32 cbuf_index, bool is_written) const;

    /// StorageBufferBinding through storage_binding_memo (EDEN_NX2_SSBO_MEMO).
    [[nodiscard]] Binding MemoStorageBufferBinding(GPUVAddr ssbo_addr, u32 cbuf_index,
                                                   bool is_written);

    [[nodiscard]] TextureBufferBinding GetTextureBufferBinding(GPUVAddr gpu_addr, u32 size,
                                                               PixelFormat format);

    [[nodiscard]] std::span<const u8> ImmediateBufferWithData(DAddr device_addr, size_t size);

    [[nodiscard]] std::span<u8> ImmediateBuffer(size_t wanted_capacity);

    [[nodiscard]] bool HasFastUniformBufferBound(size_t stage, u32 binding_index) const noexcept;

    [[nodiscard]] Binding& VertexBufferSlot(u32 index);

    [[nodiscard]] const Binding& VertexBufferSlot(u32 index) const;

    void UpdateVertexBufferSlot(u32 index, const Binding& binding);

    void ClearDownload(DAddr base_addr, u64 size);

    void InlineMemoryImplementation(DAddr dest_address, size_t copy_size,
                                    std::span<const u8> inlined_buffer);

    Tegra::MaxwellDeviceMemoryManager& device_memory;

    Common::SlotVector<Buffer> slot_buffers;
#ifdef YUZU_LEGACY
    static constexpr size_t TICKS_TO_DESTROY = 6;
#else
    static constexpr size_t TICKS_TO_DESTROY = 8;
#endif
    DelayedDestructionRing<Buffer, TICKS_TO_DESTROY> delayed_destruction_ring;

    const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect{};

    u32 last_index_count = 0;

    u32 enabled_vertex_buffers_mask = 0;
    u64 vertex_buffers_serial = 0;
    std::array<Binding, 32> v_buffer{};

    /// EDEN_NX2_VERTEX_SKIP (UpdateVertexBuffer): vertex stream address ranges and what they
    /// resolved to, direct-mapped by address. An entry holds while its memory manager's page
    /// table (MappingEpoch) and its buffer (vertex_memo_generations, bumped by DeleteBuffer) are
    /// unchanged and page_table still names that buffer: then FindBuffer would return it again.
    /// layout_epoch is buffer_layout_epoch when page_table was last seen naming the buffer
    /// (~u64{0} when it did not): while that holds page_table still does (EDEN_NX2_VERTEX_FAST).
    struct VertexBufferMemo {
        GPUVAddr gpu_addr_begin;
        GPUVAddr gpu_addr_end;
        DAddr device_addr;
        u64 map_epoch;
        u64 layout_epoch;
        u32 size;
        BufferId buffer_id;
        u32 generation;
        u32 memo_epoch;
    };
    static constexpr size_t VERTEX_MEMO_SIZE = 4096;
    /// One vertex_memo entry per cache line: at 56 bytes most entries would straddle two.
    struct alignas(64) VertexMemoLine {
        VertexBufferMemo entry;
    };
    std::unique_ptr<VertexMemoLine[]> vertex_memo;
    std::vector<u32> vertex_memo_generations;
    size_t vertex_memo_owner = ~size_t{0};
    u32 vertex_memo_epoch = 0;
    /// EDEN_NX2_INDEX_SKIP (UpdateIndexBuffer): index buffer start addresses, what they translated
    /// to and the buffer that held them, direct-mapped by address. Valid on the same terms as
    /// VertexBufferMemo (same owner check, epoch and generations); the buffer's end is kept so
    /// draws reading a different number of indices from the same start reuse the entry too.
    struct IndexBufferMemo {
        GPUVAddr gpu_addr_begin;
        DAddr device_addr;
        DAddr buffer_end;
        u64 map_epoch;
        BufferId buffer_id;
        u32 generation;
        u32 memo_epoch;
    };
    static constexpr size_t INDEX_MEMO_SIZE = 64;
    std::array<IndexBufferMemo, INDEX_MEMO_SIZE> index_memo{};

    /// Whether UpdateIndexBuffer reuses translations (index_memo). EDEN_NX2_INDEX_SKIP=0 turns it
    /// off.
    static bool IndexSkipEnabled() {
        // Read at the first call, as a function-local static const is, without that static's
        // thread-safe initialization check on every call (MSVC: a thread-local read; this runs
        // for each indexed draw).
        static std::atomic<s8> enabled{-1};
        s8 value = enabled.load(std::memory_order_relaxed);
        if (value < 0) [[unlikely]] {
            const char* env = std::getenv("EDEN_NX2_INDEX_SKIP");
            value = static_cast<s8>(env == nullptr || env[0] != '0');
            enabled.store(value, std::memory_order_relaxed);
        }
        return value != 0;
    }

    /// Back stages, cut c: the range each vertex stream tracked last (MarkVertexStreamsUsed).
    std::array<UsageMarkMemo, 32> vertex_usage_marks{};
    /// The buffer each vertex stream touched last and frame_tick + 1 then (BindHostVertexBuffers).
    std::array<BufferId, 32> vertex_touched_ids{};
    std::array<u64, 32> vertex_touched_ticks{};
    /// EDEN_NX2_LIGHT_SYNC_SKIP: the memory tracker's CPU-set epoch taken before each vertex
    /// stream's last synchronization in BindHostVertexBuffers (0, never matching, before the
    /// first). Its range held no CPU-modified or untracked page as of then.
    std::array<u64, 32> vertex_synced_epoch{};
    /// EDEN_NX2_GPU_PROFILE: what the last BindHostVertexBuffers bound (NoteVertexStreams),
    /// counted by CommitVertexStreamNote.
    struct VertexStreamNote {
        u32 enabled;
        u32 buffers;
        u32 dirty;
        u32 sync_skipped;
        u32 sync_would_skip;
        u32 sync_guarded;
        bool pending;
    };
    VertexStreamNote vertex_note{};

    /// Whether unchanged vertex stream work is skipped (UpdateVertexBuffer(s),
    /// BindHostVertexBuffers). EDEN_NX2_VERTEX_SKIP=0 turns it off.
    static bool VertexSkipEnabled() {
        return Nx2VertexSkip();
    }

    /// Bumped by every page_table change (ChangeRegister) (EDEN_NX2_VERTEX_FAST).
    u64 buffer_layout_epoch = 0;
    /// The memo entry each vertex stream resolved through last (EDEN_NX2_VERTEX_FAST). Valid on
    /// the same terms as vertex_memo's; a stream marked dirty with the range it had (its stride,
    /// enable or frequency changed, or the state was invalidated) finds it here without hashing
    /// into vertex_memo.
    std::array<VertexBufferMemo, NUM_VERTEX_BUFFERS> vertex_stream_memo{};

    boost::container::small_vector<BufferCopy, 4> upload_copies;

    /// EDEN_NX2_SYNC_MEMO (SynchronizeBuffer): ranges left synchronized (no CPU-modified or
    /// untracked page), with the memory tracker's CPU-set epoch at which that held, direct-mapped
    /// by the 64 KiB block of the start; ranges may cross regions, and ranges that overlap or
    /// touch while both hold are joined. A range inside one that still holds (UnsetSince) has no
    /// page to upload, clear or retrack, whichever buffer holds it: the call does nothing.
    struct SyncMemo {
        DAddr begin;
        DAddr end;
        u64 epoch; ///< 0: empty (the tracker's epoch starts at 1)
    };
    static constexpr size_t SYNC_MEMO_SIZE = 2048;
    std::array<SyncMemo, SYNC_MEMO_SIZE> sync_memo{};
    /// EDEN_NX2_SYNC_SETS: the same, in pages and sets (Nx2SyncSets).
    SyncMemoSets sync_sets;

    /// EDEN_NX2_SSBO_MEMO (BindGraphicsStorageBuffer): bindings StorageBufferBindingAt resolved,
    /// keyed by the two constant buffer words it read (address, then the size when packed), the
    /// memory manager and its MappingEpoch read before, direct-mapped by address. Besides those
    /// words the binding depends only on that manager's page table and kind map (both changed
    /// only together with MappingEpoch) and on the device's storage buffer alignment (fixed), so
    /// an entry matching all of them is what StorageBufferBindingAt would return again. Only
    /// resolved bindings are kept: the failing ones still log as before.
    struct StorageBindingMemo {
        GPUVAddr gpu_addr;
        u64 next_qword;
        u64 map_epoch;
        size_t owner;
        Binding binding;
        bool is_written;
    };
    static constexpr size_t STORAGE_BINDING_MEMO_SIZE = 256;
    std::array<StorageBindingMemo, STORAGE_BINDING_MEMO_SIZE> storage_binding_memo{};
    /// EDEN_NX2_SSBO_FAST: GetMemoryLayoutSize answers (StorageBufferBindingAt).
    mutable LayoutRunMemo layout_runs;

    MemoryTracker memory_tracker;
    SummarizedRangeSet<false> uncommitted_gpu_modified_ranges;

    /// Whether GPU-written buffer ranges are copied back to guest memory at the next fence;
    /// otherwise only when something flushes them (as upstream yuzu below High GPU accuracy).
    /// Not for Switch 2 titles: title 1 had 250-330 MB/s copied back (a fence per compute
    /// dispatch, the fencing thread holding the buffer cache while it wrote them), while its CPU
    /// read GPU-written memory at most 64 times in 200 s (counted with reactive flushing) and
    /// the native MME's DMA reads never touched those pages. Without them a fence costs ~1 us
    /// instead of ~5 and draw setup ~7% less. The titles that do read those pages back are listed
    /// in Nx2NeedsBufferDownloads (nx2.h). EDEN_NX2_BUFFER_DOWNLOADS=0/1 overrides both.
    bool AsyncBufferDownloads() const {
        static const int forced = [] {
            const char* value = std::getenv("EDEN_NX2_BUFFER_DOWNLOADS");
            return value == nullptr ? -1 : (value[0] == '0' ? 0 : 1);
        }();
        if (forced >= 0) {
            return forced == 1;
        }
        return gpu_memory == nullptr || !gpu_memory->UsesNativeTiling() ||
               Tegra::Nx2NeedsBufferDownloads();
    }
    SummarizedRangeSet<true> gpu_modified_ranges;
    std::deque<SummarizedRangeSet<false>> committed_gpu_modified_ranges;
    /// The first of them a fence held back (DeferCopyBacks), until the next commit
    size_t deferred_copy_back_sets{};
    u64 deferred_reads{};

    /// A read fetches [addr, addr + size): counts it if a fence held its data back.
    void NoteDeferredRead(DAddr addr, u64 size);
    /// Buffer pages the guest CPU reads (NoteCpuRead). Without AsyncBufferDownloads, GPU writes
    /// to them are still copied back at fences: Pokemon Pokopia builds its sky, fog and exposure
    /// constants from values the GPU computes, and drew a black, flickering sky without them.
    /// Copying every GPU-written range back instead (each written native storage binding is an
    /// 8 MiB window) dropped it from 60 to about 42 frames a second.
    SummarizedRangeSet<false> cpu_read_ranges;
    /// EDEN_NX2_READ_LATCH_AGE (0: off): the frame each of cpu_read_ranges' pages was last read in
    const u64 read_latch_age = Nx2Reactive::ReadLatchAge();
    /// EDEN_NX2_RX_NO_COPYBACK (diagnostic: no CPU-read page, no copy back)
    const bool no_copy_back = Nx2Reactive::NoCopyBack();
    Nx2Reactive::ReadPageAges read_page_ages;
    /// Drops the CPU-read pages not read for read_latch_age frames and makes the cores' reads
    /// reach GetFlushArea again (TickFrame, every AgeStep frames).
    void AgeCpuReads();

    /// Queues the parts of a GPU write to [addr, addr + size) that are copied back at the next
    /// fence.
    void QueueDownload(DAddr addr, u64 size) {
        if (AsyncBufferDownloads()) {
            uncommitted_gpu_modified_ranges.Add(addr, size);
            return;
        }
        if (!read_back_pages.empty() && SkipEarlyCopyBack()) [[unlikely]] {
            // EDEN_NX2_EARLY_SKIP_COPYBACK: early downloads bring these pages, and a read they
            // miss downloads on demand (read traps, or reactive flushing's RX_READBACK).
            cpu_read_ranges.ForEachInRange(addr, size, [this](DAddr start, DAddr end) {
                for (DAddr page = Common::AlignDown(start, Core::DEVICE_PAGESIZE); page < end;
                     page += Core::DEVICE_PAGESIZE) {
                    if (IsEarlyPage(page)) {
                        continue;
                    }
                    const DAddr from = (std::max)(start, page);
                    const DAddr to = (std::min)(end, page + Core::DEVICE_PAGESIZE);
                    uncommitted_gpu_modified_ranges.Add(from, to - from);
                }
            });
        } else {
            cpu_read_ranges.ForEachInRange(addr, size, [this](DAddr start, DAddr end) {
                uncommitted_gpu_modified_ranges.Add(start, end - start);
            });
        }
        if (ReadTrapsActive()) {
            ArmReadTraps(addr, size);
        }
    }

    [[nodiscard]] bool SkipEarlyCopyBack() const {
        namespace AsyncReadback = VideoCommon::Nx2AsyncReadback;
        return AsyncReadback::EarlySkipCopyBack() &&
               (ReadTrapsActive() || (AsyncReadback::RxReadback() &&
                                      Settings::values.use_reactive_flushing.GetValue()));
    }

    /// Traps the guest CPU's next read of these pages (ReadTrapsActive). Its flush notes the
    /// page as CPU-read, so later GPU writes there are copied back at fences instead.
    void ArmReadTraps(DAddr device_addr, u64 size) {
        const DAddr begin = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE);
        const DAddr end = Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE);
        u64 armed_bytes = 0;
        read_trapped_ranges.ForEachInRange(begin, end - begin, [&](DAddr lower, DAddr upper) {
            armed_bytes += upper - lower;
        });
        if (armed_bytes == end - begin) {
            return;
        }
        read_trapped_ranges.Add(begin, end - begin);
        device_memory.ArmReadTraps(begin, end - begin);
    }

    /// Pages handed to ArmReadTraps and not read since (ForgetReadTraps): a GPU write to one of
    /// them arms nothing new.
    Common::RangeSet<DAddr> read_trapped_ranges;

    /// Whether every GPU write in this range is in a committed copy back (async_downloads).
    [[nodiscard]] bool CopyBackCovers(DAddr addr, u64 size) const;
    /// A copy back landed in [addr, addr + size): disarms the read traps of its pages the GPU
    /// has not written since (EDEN_NX2_TRAP_POP_DISARM=0: they stay armed until a read).
    void DisarmCopiedReadTraps(DAddr addr, u64 size);
    /// Notified after each PopAsyncBuffers (WaitForCopyBack).
    std::condition_variable_any copy_back_cv;

    // Async Buffers
    Common::OverlapRangeSet<DAddr> async_downloads;
    std::deque<std::optional<Async_Buffer>> async_buffers;
    std::deque<boost::container::small_vector<BufferCopy, 4>> pending_downloads;
    std::optional<Async_Buffer> current_buffer;

    std::deque<Async_Buffer> async_buffers_death_ring;

    /// EDEN_NX2_ASYNC_READBACK: copies for guest CPU reads (and early ones, before them) not in
    /// guest memory yet
    struct ReadDownload {
        Async_Buffer staging;
        boost::container::small_vector<BufferCopy, 4> copies; ///< src_offset is the device address
        u64 tick;
        u64 seq; ///< Recording order
        bool early{};
        /// The bytes it writes: a CPU write, an inline write or a synchronous download over
        /// them takes them out (ClearDownload).
        Common::RangeSet<DAddr> write_ranges;
        /// The bytes no GPU write changed since: GPU-modified no longer once written.
        Common::RangeSet<DAddr> clean_ranges;
    };
    std::deque<ReadDownload> read_downloads;
    /// What the read downloads bring that is still the GPU's latest
    Common::RangeSet<DAddr> read_download_ranges;
    u64 read_download_seq{};
    /// The newest read download when each of pending_downloads was committed
    std::deque<u64> pending_download_read_seqs;

    /// Records copies of the GPU-modified bytes in `ranges` no read download brings yet. Not
    /// submitted: the caller does, or the next submission takes them. Returns whether it did.
    bool RecordReadDownload(std::span<const std::pair<DAddr, u64>> ranges, bool early);
    /// Writes the read downloads up to (and with) seq, oldest first; their copies have run.
    void WriteReadDownloads(u64 seq, VideoCommon::Nx2AsyncReadback::Landing who);
    /// Writes those the host GPU already ran.
    void WriteRunReadDownloads();
    /// Updates VideoCore::g_read_downloads after read_downloads changed.
    void PublishReadDownloads(DAddr begin, DAddr end, bool added);

    /// EDEN_NX2_ASYNC_READBACK=2: a read's bytes as the GPU had them at its point of the work
    struct ReadCheck {
        Async_Buffer staging;
        struct Piece {
            DAddr addr;
            u64 size;
            u64 offset; ///< In staging, past its own offset
            bool early_landed; ///< Guest memory had them from an early download already
        };
        std::vector<Piece> pieces;
        u64 tick;
        u64 seq; ///< The newest read download at its point
        DAddr begin;
        DAddr end;
        /// Bytes guest memory legitimately got since (CPU writes, writes from the GPU's side,
        /// newer downloads)
        Common::RangeSet<DAddr> changed;
    };
    std::deque<ReadCheck> read_checks;
    /// Bytes early downloads landed that no write changed since (=2 only)
    Common::RangeSet<DAddr> early_landed_ranges;
    void RecordReadCheck(DAddr device_addr, u64 size);
    /// Guest memory got bytes there that no download brought (=2 only).
    void NoteGuestChange(DAddr device_addr, u64 size) {
        if (read_checks.empty() && early_landed_ranges.Empty()) [[likely]] {
            return;
        }
        for (ReadCheck& check : read_checks) {
            check.changed.Add(device_addr, size);
        }
        early_landed_ranges.Subtract(device_addr, size);
    }

    /// EDEN_NX2_EARLY_DOWNLOADS: a page the guest CPU read GPU writes from. Pages read after
    /// each of their writes get their download recorded right behind the write (a dispatch, a
    /// fence) instead, which the read then finds landed or on its way.
    struct ReadBackPage {
        DAddr page{};
        u64 last_read{};        ///< read_back_reads at its last read of a write (eviction)
        u64 last_write_frame{};
        u16 reads{};            ///< Writes in a row the guest read, up to EarlyAfterReads
        u16 unread_frames{};    ///< Frames with GPU writes since its last read
        bool written{};         ///< A GPU write since its last read
        bool queued{};          ///< In early_pages
    };
    static constexpr u16 EarlyAfterReads = 2;
    /// Frames with writes and no read that stop them (Lichii counts 256 writes; Switch 2 storage
    /// windows are marked written at every draw, so frames here).
    static constexpr u16 MaxUnreadFrames = 240;
    static constexpr size_t MaxReadBackPages = 512;
    static constexpr u64 EarlyBytesPerFrame = 8ULL << 20;
    static constexpr u32 MaxPendingEarlyDownloads = 512;
    std::vector<ReadBackPage> read_back_pages; ///< Sorted by page
    std::vector<DAddr> early_pages;            ///< Written since their last early download
    u64 read_back_reads{};
    u64 early_bytes_this_frame{};
    u32 pending_early_downloads{};
    /// A read (GetFlushArea) of [begin, end): learns the pages it finds GPU writes in.
    void NoteReadBack(DAddr begin, DAddr end, bool gpu_modified);
    /// A GPU write: the learned pages under it get an early download.
    void NoteReadBackWrite(DAddr device_addr, u64 size);
    [[nodiscard]] bool IsEarlyPage(DAddr page) const;
    bool RecordEarlyDownloads();

    size_t immediate_buffer_capacity = 0;
    Common::ScratchBuffer<u8> immediate_buffer_alloc;

    struct LRUItemParams {
        using ObjectType = BufferId;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<LRUItemParams> lru_cache;
    /// Frame tick of each buffer slot's last LRU touch (TouchBuffer, EDEN_NX2_TOUCH_SKIP).
    std::vector<u64> touch_ticks;
    VirtualRangeCache virtual_ranges;
    std::vector<MultiRangeSegment> graphics_segments;
    std::vector<MultiRangeSegment> compute_segments;
    u64 frame_tick = 0;
    /// Settings::IsGPUFenceBehaviorAccurate(), read once a frame for the per-draw paths.
    bool fence_accurate = false;
    u64 total_used_memory = 0;
    u64 buffer_bytes = 0; ///< The buffers' own size (total_used_memory may be the device's)
    u64 minimum_memory = 0;
    u64 critical_memory = 0;
    BufferId inline_buffer_id;
#ifdef YUZU_LEGACY
    bool immediately_free = false;
#endif

    // Covers the device address space (MaxwellDeviceTraits::device_virtual_bits).
    std::array<BufferId, ((1ULL << 36) >> CACHING_PAGEBITS)> page_table;
    Common::ScratchBuffer<u8> tmp_buffer;
};

} // namespace VideoCommon
