// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Service::Nx2FrameStats {

/// The per_mille-th permille (nearest rank) of the frame times, 0 with none; reorders `frames`.
inline u32 Percentile(std::span<u32> frames, u32 per_mille) {
    if (frames.empty()) {
        return 0;
    }
    const auto rank =
        frames.begin() + static_cast<std::ptrdiff_t>((frames.size() * per_mille - 1) / 1000);
    std::nth_element(frames.begin(), rank, frames.end());
    return *rank;
}

/// The 95th percentile (nearest rank) of the frame times, 0 with none; reorders `frames`.
inline u32 P95(std::span<u32> frames) {
    return Percentile(frames, 950);
}

/// Frame times since the game started, in 100 us buckets up to 1 s, for the 1% and 0.1% lows
/// (the 99th and 99.9th percentile frame times): a 600-present block holds less than one 0.1%
/// frame.
class Histogram {
public:
    static constexpr u32 BucketUs = 100;
    static constexpr u32 Buckets = 10'000;

    Histogram() : buckets(Buckets) {}

    void Add(u32 frame_us) {
        ++buckets[(std::min)(frame_us / BucketUs, Buckets - 1)];
        ++count;
        max_us = (std::max)(max_us, frame_us);
    }

    u64 Count() const {
        return count;
    }

    /// The upper edge of the bucket holding the per_mille-th permille frame (nearest rank),
    /// the longest frame for the last bucket, 0 with none.
    u32 Percentile(u32 per_mille) const {
        if (count == 0) {
            return 0;
        }
        const u64 rank = (count * per_mille - 1) / 1000 + 1;
        u64 seen = 0;
        for (u32 i = 0; i < Buckets - 1; ++i) {
            seen += buckets[i];
            if (seen >= rank) {
                return (std::min)((i + 1) * BucketUs, max_us);
            }
        }
        return max_us;
    }

private:
    std::vector<u32> buckets;
    u64 count{};
    u32 max_us{};
};

} // namespace Service::Nx2FrameStats
