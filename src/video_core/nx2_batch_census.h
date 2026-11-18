// SPDX-FileCopyrightText: Copyright 2026 Dozer Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_BATCH_CENSUS=1 (diagnostic, off by default): how many draws could become one multi
// draw. A run is a full setup and the light draws after it (same pipeline, images, render pass
// and dynamic state, nothing recorded in between). Each light draw is counted by what changed
// since the draw before: draw parameters only; uniform data or binds too; or a vertex stream or
// the index buffer. A batchable run is a stretch of a run whose draws changed no vertex stream
// or index buffer: they could share one multi draw if each draw's uniforms were indexed by its
// draw index. Logged by draws in runs of each length, every 2^20 draws (memory stage thread).
namespace VideoCommon::Nx2BatchCensus {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_BATCH_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct Counts {
    /// Where the run length buckets start after the first (length 1)
    static constexpr std::array<u64, 5> Starts{2, 4, 8, 16, 64};
    u64 draws{};
    u64 full{};
    u64 params_only{};
    u64 uniforms{};
    u64 geometry{};
    std::array<u64, 6> runs{};      ///< Draws in runs of each length bucket
    std::array<u64, 6> batchable{}; ///< The same for batchable runs
    u64 run{};
    u64 batch{};
};

inline Counts g_counts;

inline size_t Bucket(u64 length) {
    size_t bucket = 0;
    for (const u64 start : Counts::Starts) {
        bucket += length >= start ? 1 : 0;
    }
    return bucket;
}

inline void Log() {
    const auto& c = g_counts;
    const auto share = [&](u64 value) { return c.draws != 0 ? 100.0 * value / c.draws : 0.0; };
    const auto spread = [&](const std::array<u64, 6>& buckets) {
        return fmt::format("1 {:.1f}%, 2-3 {:.1f}%, 4-7 {:.1f}%, 8-15 {:.1f}%, 16-63 {:.1f}%, "
                           "64+ {:.1f}%",
                           share(buckets[0]), share(buckets[1]), share(buckets[2]),
                           share(buckets[3]), share(buckets[4]), share(buckets[5]));
    };
    LOG_INFO(HW_GPU,
             "EDEN_NX2_BATCH_CENSUS: {} draws: full setup {:.1f}%, light with draw parameters "
             "only {:.1f}%, with uniforms {:.1f}%, with vertex or index buffers {:.1f}% | draws "
             "in runs of length: {} | in batchable runs: {}",
             c.draws, share(c.full), share(c.params_only), share(c.uniforms), share(c.geometry),
             spread(c.runs), spread(c.batchable));
}

inline void CloseRun(u64 Counts::*length, std::array<u64, 6> Counts::*buckets) {
    auto& c = g_counts;
    if (c.*length != 0) {
        (c.*buckets)[Bucket(c.*length)] += c.*length;
        c.*length = 0;
    }
}

/// A draw: set up in full, or light with these Maxwell3D::LightChange bits since the last draw.
inline void Draw(bool light, bool params_only, bool geometry) {
    auto& c = g_counts;
    ++c.draws;
    if (!light) {
        ++c.full;
        CloseRun(&Counts::run, &Counts::runs);
        CloseRun(&Counts::batch, &Counts::batchable);
    } else if (geometry) {
        ++c.geometry;
        CloseRun(&Counts::batch, &Counts::batchable);
    } else {
        ++(params_only ? c.params_only : c.uniforms);
    }
    ++c.run;
    ++c.batch;
    if ((c.draws & ((u64{1} << 20) - 1)) == 0) {
        Log();
        CloseRun(&Counts::run, &Counts::runs);
        CloseRun(&Counts::batch, &Counts::batchable);
        g_counts = Counts{};
    }
}

} // namespace VideoCommon::Nx2BatchCensus
