// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_reactive.h"

// EDEN_NX2_ASYNC_READBACK (after Lichii's read-back): a guest CPU read of GPU-written buffer
// memory (GPU::OnCPURead) has the GPU thread record the copies and submit them without waiting
// for the host GPU (BufferCache::DownloadMemoryForRead). The reading thread waits for that
// submission alone, then the GPU thread writes the bytes to guest memory
// (BufferCache::FinishReadDownloads), in order with its other writes there.
// - unset or 0: off, the GPU thread waits for the host GPU at every such read (runtime.Finish).
// - 1: on.
// - 2: on, and each landing is compared with a synchronous download of the bytes no GPU write
//   changed since (DIAGNOSTIC: a Finish per read).
namespace VideoCommon::Nx2AsyncReadback {

[[nodiscard]] inline int Mode() noexcept {
    static const int mode = [] {
        const char* value = Common::Nx2Reactive::GetEnv("EDEN_NX2_ASYNC_READBACK");
        if (value == nullptr || value[0] == '\0' || value[0] == '0') {
            return 0;
        }
        return value[0] == '2' ? 2 : 1;
    }();
    return mode;
}

[[nodiscard]] inline bool Enabled() noexcept {
    return Mode() != 0;
}

[[nodiscard]] inline bool Verify() noexcept {
    return Mode() == 2;
}

struct Stats {
    std::atomic<u64> reads{};       ///< Reads served by a read download
    std::atomic<u64> recorded{};    ///< Read downloads recorded (the rest found theirs in flight)
    std::atomic<u64> bytes{};
    std::atomic<u64> waits{};       ///< Reads whose download the host GPU had not run yet
    std::atomic<u64> wait_us{};
    std::atomic<u64> max_wait_us{};
    std::atomic<u64> fallbacks{};   ///< Reads that took the synchronous flush
    std::atomic<u64> early_lands{}; ///< Downloads landed before their read asked (run already)
    std::atomic<u64> mismatches{};  ///< Verify: landed bytes a synchronous download disagreed with
};

inline Stats g_stats;

inline void Add(std::atomic<u64>& counter, u64 value) noexcept {
    counter.fetch_add(value, std::memory_order_relaxed);
}

inline void NoteWait(u64 us) noexcept {
    Add(g_stats.waits, 1);
    Add(g_stats.wait_us, us);
    u64 max = g_stats.max_wait_us.load(std::memory_order_relaxed);
    while (us > max &&
           !g_stats.max_wait_us.compare_exchange_weak(max, us, std::memory_order_relaxed)) {
    }
}

/// A read was served: logs the totals every 4096 reads.
inline void NoteRead() noexcept {
    const u64 reads = g_stats.reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((reads & 4095) != 0) {
        return;
    }
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const u64 recorded = take(g_stats.recorded);
    const u64 bytes = take(g_stats.bytes);
    const u64 waits = take(g_stats.waits);
    const u64 wait_us = take(g_stats.wait_us);
    const u64 max_wait_us = take(g_stats.max_wait_us);
    const u64 fallbacks = take(g_stats.fallbacks);
    const u64 early_lands = take(g_stats.early_lands);
    const u64 mismatches = g_stats.mismatches.load(std::memory_order_relaxed);
    LOG_INFO(HW_GPU,
             "Async read-back, last 4096 reads: {} downloads recorded ({} KiB), the rest in "
             "flight | readers waited for the host GPU {} times, {} us (avg {} us, max {} us) | "
             "{} landed before their read asked | {} synchronous fallbacks | {} verify "
             "mismatches so far",
             recorded, bytes >> 10, waits, wait_us, waits != 0 ? wait_us / waits : 0, max_wait_us,
             early_lands, fallbacks, mismatches);
}

} // namespace VideoCommon::Nx2AsyncReadback
