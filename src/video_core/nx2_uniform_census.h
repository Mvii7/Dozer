// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>

#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::Nx2UniformCensus {

/// What moving uniform buffer work and memory writes off the memory stage would meet: counted
/// on the thread that runs the rasterizer, logged every 2^20 draws (RasterizerVulkan::Draw,
/// DrawIndirect).
/// Relaxed loads and stores, no read-modify-write: one thread writes them.
enum Counter : u32 {
    Draws,
    UniformBinds,       ///< BindHostGraphicsUniformBuffer calls
    UniformRootTable,   ///< Of them streamed from root table words (EDEN_NX2_ROOT_TABLES)
    UniformFast,        ///< Streamed from guest memory (the fast path)
    UniformGpuWritten,  ///< Cached path: the range has GPU-written pages
    UniformTooBig,      ///< Cached path: larger than the skip size
    UniformNoSkip,      ///< Cached path: the skip size is 0 (cache hits preferred)
    UniformNoBuffer,    ///< Cached path: no host buffer
    UniformBytes,       ///< Bytes of the fast path's binds
    CbWrites,           ///< Constant buffer data writes (CbWriteImpl)
    CbWriteBytes,
    InlineWrites,       ///< Inline uploads into the buffer cache (InlineMemory, InlineMemoryBatched)
    InlineWriteBytes,
    InlineGpuWritten,   ///< Of them on pages with GPU-written data
    Count,
};

inline std::array<std::atomic<u64>, Count> g_counters{};

inline void Add(Counter counter, u64 value) {
    auto& slot = g_counters[counter];
    slot.store(slot.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

inline u64 Get(Counter counter) {
    return g_counters[counter].load(std::memory_order_relaxed);
}

/// Counts a draw; every 2^20 draws logs the totals since the start.
inline void NoteDraw() {
    Add(Draws, 1);
    const u64 draws = Get(Draws);
    if ((draws & ((u64{1} << 20) - 1)) != 0) {
        return;
    }
    const auto per_draw = [draws](Counter counter) {
        return static_cast<double>(Get(counter)) / static_cast<double>(draws);
    };
    const u64 binds = (std::max)(Get(UniformBinds), u64{1});
    const auto share = [binds](Counter counter) {
        return 100.0 * static_cast<double>(Get(counter)) / static_cast<double>(binds);
    };
    LOG_INFO(HW_GPU,
             "nx2 uniform census: {} draws; per draw {:.2f} uniform binds ({:.1f}% fast, "
             "{:.1f}% GPU-written, {:.1f}% too big, {:.1f}% skip size 0, {:.1f}% no buffer, "
             "{:.1f}% root table; {:.0f} fast bytes), {:.2f} constant buffer writes ({:.0f} "
             "bytes), {:.2f} inline writes ({:.0f} bytes, {:.2f} on GPU-written pages)",
             draws, per_draw(UniformBinds), share(UniformFast), share(UniformGpuWritten),
             share(UniformTooBig), share(UniformNoSkip), share(UniformNoBuffer),
             share(UniformRootTable), per_draw(UniformBytes), per_draw(CbWrites),
             per_draw(CbWriteBytes), per_draw(InlineWrites), per_draw(InlineWriteBytes),
             per_draw(InlineGpuWritten));
}

} // namespace VideoCommon::Nx2UniformCensus
