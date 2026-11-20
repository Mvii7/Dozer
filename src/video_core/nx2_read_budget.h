// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_READ_TRAP_BUDGET_US=<n> (unset: 1000, 0: off). With read traps, the guest CPU's reads of
// GPU-written buffer memory wait for its data (a copy back or a download) at most n microseconds
// in each 1/60 s; past that, a read goes on with what memory holds, as with read traps off, and a
// copy back already on its way still lands. Big scenes read little and stay exact; Donkey Kong
// Bananza's destruction reads ~2,400 times per 10 s and waited 0.5-0.8 s for it. A download (no
// copy back on its way) is skipped when what is left is less than downloads have been taking
// (EDEN_NX2_READ_TRAP_BUDGET_PREDICT=0: started anyway, and overshoots the budget).
namespace VideoCommon::Nx2ReadBudget {

[[nodiscard]] inline u64 BudgetUs() noexcept {
    static const u64 budget = [] {
        const char* value = std::getenv("EDEN_NX2_READ_TRAP_BUDGET_US");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : u64{1000};
    }();
    return budget;
}

[[nodiscard]] inline bool Enabled() noexcept {
    return BudgetUs() != 0;
}

[[nodiscard]] inline bool Predict() noexcept {
    static const bool predict = [] {
        const char* value = std::getenv("EDEN_NX2_READ_TRAP_BUDGET_PREDICT");
        return value == nullptr || value[0] != '0';
    }();
    return predict;
}

[[nodiscard]] inline s64 NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct State {
    std::atomic<s64> slice{-1};
    std::atomic<u64> used_us{};
    // Since the last line.
    std::atomic<u64> waited{};
    std::atomic<u64> waited_us{};
    std::atomic<u64> lenient{};
    std::atomic<u64> skipped{};
    std::atomic<s64> next_log_ns{};
    // Running average of a download's wait (1/8 weight to the newest).
    std::atomic<u64> download_avg_us{};
};
inline State g_state;

inline constexpr s64 SliceNs = 16'666'667;
inline constexpr s64 LogPeriodNs = 10'000'000'000;

inline void MaybeLog(s64 now) {
    s64 next = g_state.next_log_ns.load(std::memory_order_relaxed);
    if (now < next ||
        !g_state.next_log_ns.compare_exchange_strong(next, now + LogPeriodNs,
                                                     std::memory_order_relaxed)) {
        return;
    }
    if (next == 0) {
        return; // the first call starts the period
    }
    const u64 waited = g_state.waited.exchange(0, std::memory_order_relaxed);
    const u64 waited_us = g_state.waited_us.exchange(0, std::memory_order_relaxed);
    const u64 lenient = g_state.lenient.exchange(0, std::memory_order_relaxed);
    const u64 skipped = g_state.skipped.exchange(0, std::memory_order_relaxed);
    LOG_INFO(HW_GPU,
             "Read trap budget {} us per 1/60 s (EDEN_NX2_READ_TRAP_BUDGET_US), last 10 s: {} "
             "waits for {} ms, {} reads went on without waiting ({} downloads skipped, a "
             "download ~{} us)",
             BudgetUs(), waited, waited_us / 1000, lenient, skipped,
             g_state.download_avg_us.load(std::memory_order_relaxed));
}

/// What is left of this slice's budget (none with the budget off).
[[nodiscard]] inline u64 RemainingUs() {
    if (!Enabled()) {
        return 0;
    }
    const s64 now = NowNs();
    const s64 slice = now / SliceNs;
    if (s64 seen = g_state.slice.load(std::memory_order_relaxed);
        seen != slice &&
        g_state.slice.compare_exchange_strong(seen, slice, std::memory_order_relaxed)) {
        g_state.used_us.store(0, std::memory_order_relaxed);
    }
    MaybeLog(now);
    const u64 used = g_state.used_us.load(std::memory_order_relaxed);
    return used < BudgetUs() ? BudgetUs() - used : 0;
}

/// A read went on without its data (counted).
inline void NoteLenient() {
    g_state.lenient.fetch_add(1, std::memory_order_relaxed);
}

/// Whether this slice's budget is spent: the read then goes on without waiting (counted).
[[nodiscard]] inline bool TakeLenient() {
    if (!Enabled() || RemainingUs() != 0) {
        return false;
    }
    NoteLenient();
    return true;
}

/// Whether a download would not fit in what is left: the read then goes on without it (counted).
[[nodiscard]] inline bool SkipDownload() {
    if (!Enabled() || !Predict() ||
        RemainingUs() >= g_state.download_avg_us.load(std::memory_order_relaxed)) {
        return false;
    }
    NoteLenient();
    g_state.skipped.fetch_add(1, std::memory_order_relaxed);
    // Each skip lowers the average a little, so one slow download does not stop all later ones.
    const u64 avg = g_state.download_avg_us.load(std::memory_order_relaxed);
    g_state.download_avg_us.store(avg - avg / 32, std::memory_order_relaxed);
    return true;
}

/// A read waited since `start_ns` (NowNs; 0: not timed); `download`: for a download.
inline void AddSince(s64 start_ns, bool download = false) {
    if (!Enabled() || start_ns == 0) {
        return;
    }
    const u64 us = static_cast<u64>((NowNs() - start_ns) / 1000);
    g_state.used_us.fetch_add(us, std::memory_order_relaxed);
    g_state.waited.fetch_add(1, std::memory_order_relaxed);
    g_state.waited_us.fetch_add(us, std::memory_order_relaxed);
    if (download) {
        const u64 avg = g_state.download_avg_us.load(std::memory_order_relaxed);
        g_state.download_avg_us.store(avg == 0 ? us : (avg * 7 + us) / 8,
                                      std::memory_order_relaxed);
    }
}

} // namespace VideoCommon::Nx2ReadBudget
