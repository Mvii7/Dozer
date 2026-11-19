// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_reactive.h"
#include "common/settings.h"

// EDEN_NX2_ASYNC_READBACK (after Lichii's read-back): a guest CPU read of GPU-written buffer
// memory (GPU::OnCPURead) has the GPU thread record the copies and submit them without waiting
// for the host GPU (BufferCache::DownloadMemoryForRead). The reading thread waits for that
// submission, then writes the bytes to guest memory (BufferCache::FinishReadDownloads).
// - 0: off, the GPU thread waits for the host GPU at every such read (runtime.Finish).
// - unset or 1: on.
// - 2: on, and checked: each read also copies the bytes it needs at its point of the GPU's work,
//   and once its downloads landed guest memory is compared with them ("Async read-back check"
//   errors name where the bytes came from).
// With it on, each of these is on unless set to 0:
// - EDEN_NX2_READER_LANDING: the reading thread writes the bytes (0: a second request to the
//   GPU thread does).
// - EDEN_NX2_EARLY_DOWNLOADS: pages the guest reads after each GPU write get their download
//   recorded right behind the write (a dispatch, a fence), so the read finds them landed or on
//   their way. Unset, on with reactive flushing only: with read traps the fences' copies back
//   bring those pages already, and early downloads made their reads trap (Bananza: 30 to 40 fps
//   with them off).
// - EDEN_NX2_RX_READBACK: with reactive flushing, a read of GPU-written buffer memory downloads
//   it the same way (0: it waits for the fences' copies back, as before).
// - EDEN_NX2_EARLY_SKIP_COPYBACK: pages early downloads cover are no longer copied back at
//   every fence.
namespace VideoCommon::Nx2AsyncReadback {

[[nodiscard]] inline bool EnvOn(const char* name) noexcept {
    const char* value = Common::Nx2Reactive::GetEnv(name);
    return value == nullptr || value[0] != '0';
}

[[nodiscard]] inline int Mode() noexcept {
    static const int mode = [] {
        const char* value = Common::Nx2Reactive::GetEnv("EDEN_NX2_ASYNC_READBACK");
        if (value == nullptr || value[0] == '\0') {
            return 1;
        }
        if (value[0] == '0') {
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

[[nodiscard]] inline bool ReaderLanding() noexcept {
    static const bool enabled = Enabled() && EnvOn("EDEN_NX2_READER_LANDING");
    return enabled;
}

[[nodiscard]] inline bool EarlyDownloads() noexcept {
    static const bool enabled = [] {
        if (!Enabled()) {
            return false;
        }
        const char* value = Common::Nx2Reactive::GetEnv("EDEN_NX2_EARLY_DOWNLOADS");
        if (value == nullptr || value[0] == '\0') {
            return Settings::values.use_reactive_flushing.GetValue();
        }
        return value[0] != '0';
    }();
    return enabled;
}

[[nodiscard]] inline bool RxReadback() noexcept {
    static const bool enabled = Enabled() && EnvOn("EDEN_NX2_RX_READBACK");
    return enabled;
}

/// EDEN_NX2_EARLY_SUBMIT=1: early downloads recorded after a dispatch are submitted at once,
/// so the host GPU runs them before the read comes (more submissions). Off: they go with the
/// next submission.
[[nodiscard]] inline bool EarlySubmit() noexcept {
    static const bool enabled = [] {
        const char* value = Common::Nx2Reactive::GetEnv("EDEN_NX2_EARLY_SUBMIT");
        return EarlyDownloads() && value != nullptr && value[0] == '1';
    }();
    return enabled;
}

[[nodiscard]] inline bool EarlySkipCopyBack() noexcept {
    static const bool enabled = EarlyDownloads() && EnvOn("EDEN_NX2_EARLY_SKIP_COPYBACK");
    return enabled;
}

/// Who wrote a read download to guest memory.
enum class Landing : u8 {
    Gpu,    ///< The GPU thread (it may submit and wait)
    Reader, ///< The reading thread, after its wait
    Writer, ///< A write to guest memory from the GPU's side over its bytes
    Fence,  ///< A fence's copy back, after the read downloads older than it
};

struct Stats {
    std::atomic<u64> reads{};         ///< Reads served by read downloads
    std::atomic<u64> recorded{};      ///< Read downloads recorded (the rest found theirs in flight)
    std::atomic<u64> bytes{};
    std::atomic<u64> early{};         ///< Early downloads recorded
    std::atomic<u64> early_bytes{};
    std::atomic<u64> early_capped{};  ///< Early downloads left to the reads (bounds)
    std::atomic<u64> waits{};         ///< Reads whose download the host GPU had not run yet
    std::atomic<u64> wait_us{};
    std::atomic<u64> max_wait_us{};
    std::atomic<u64> fallbacks{};     ///< Reads that took the synchronous flush
    std::atomic<u64> gpu_lands{};     ///< Downloads landed by the GPU thread (run already)
    std::atomic<u64> reader_lands{};
    std::atomic<u64> writer_lands{};  ///< Landed first by a write over them from the GPU's side
    std::atomic<u64> writer_drops{};  ///< Not submitted yet: such a write took its bytes out
    std::atomic<u64> cpu_kept{};      ///< Bytes a pending guest CPU write kept from a landing
    std::atomic<u64> learned{};       ///< Pages learned as read back
    std::atomic<u64> early_served{};  ///< Reads of a learned page's new writes that needed no fetch
    std::atomic<u64> early_submits{}; ///< EDEN_NX2_EARLY_SUBMIT submissions
    std::atomic<u64> checks{};        ///< Verify: reads checked
    std::atomic<u64> mismatches{};    ///< Verify: reads whose bytes differed
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

inline void NoteRead() noexcept {
    Add(g_stats.reads, 1);
}

/// Logs the totals and starts them again (BufferCache::TickFrame, every 600 frames).
inline void Report() noexcept {
    const auto take = [](std::atomic<u64>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const u64 reads = take(g_stats.reads);
    const u64 recorded = take(g_stats.recorded);
    const u64 bytes = take(g_stats.bytes);
    const u64 early = take(g_stats.early);
    const u64 early_bytes = take(g_stats.early_bytes);
    const u64 early_capped = take(g_stats.early_capped);
    const u64 waits = take(g_stats.waits);
    const u64 wait_us = take(g_stats.wait_us);
    const u64 max_wait_us = take(g_stats.max_wait_us);
    const u64 fallbacks = take(g_stats.fallbacks);
    const u64 gpu_lands = take(g_stats.gpu_lands);
    const u64 reader_lands = take(g_stats.reader_lands);
    const u64 writer_lands = take(g_stats.writer_lands);
    const u64 writer_drops = take(g_stats.writer_drops);
    const u64 cpu_kept = take(g_stats.cpu_kept);
    const u64 checks = take(g_stats.checks);
    const u64 early_served = take(g_stats.early_served);
    const u64 early_submits = take(g_stats.early_submits);
    LOG_INFO(HW_GPU,
             "Async read-back, last 600 frames: {} reads flushed, {} read downloads ({} KiB), {} "
             "early "
             "({} KiB, {} capped) | readers waited for the host GPU {} times, {} us (avg {} us, "
             "max {} us) | landed by the GPU thread {}, readers {}, GPU-side writes {} ({} "
             "dropped) | {} bytes kept for pending CPU writes | {} synchronous fallbacks | "
             "{} pages learned, {} reads of their new writes needed no fetch, {} early submits | "
             "checks {} this window, {} mismatching reads so far",
             reads, recorded, bytes >> 10, early, early_bytes >> 10, early_capped, waits, wait_us,
             waits != 0 ? wait_us / waits : 0, max_wait_us, gpu_lands, reader_lands, writer_lands,
             writer_drops, cpu_kept, fallbacks, g_stats.learned.load(std::memory_order_relaxed),
             early_served, early_submits, checks, g_stats.mismatches.load(std::memory_order_relaxed));
}

} // namespace VideoCommon::Nx2AsyncReadback
