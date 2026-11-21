// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>

#include "common/common_types.h"

/// EDEN_NX2_SLOW_FRAME_MS=<ms>: one "nx2 slow frame:" line for each present that came more than
/// <ms> after the previous one, with what happened in between (images, uploads, pipelines, waits
/// for the back, read flushes, game file reads, CPU JIT compiles). Unset or 0: no counter is
/// touched; each hook is a load and a branch.
namespace Common::Nx2SlowFrame {

enum class Counter : size_t {
    ImagesCreated,
    ImageCreateNs,
    ImagesDeleted,
    UploadBytes,
    UploadNs,
    CpuUnswizzleBytes,
    Pipelines,
    LowerNs,
    DriverNs,
    Drains,
    DrainNs,
    DrainMaxNs,
    ReadFlushes,
    ReadFlushNs,
    ReadFlushMaxNs,
    FsReads,
    FsReadBytes,
    FsReadNs,
    FsThrottleNs,
    FastmemRecompiles,
    Count,
};

/// The CPU JIT's compiles since the last call (blocks, ns, full cache clears), set by the CPU
/// backend; null without one.
struct JitCounts {
    u64 blocks;
    u64 ns;
    u64 clears;
    u64 again;     ///< compiled before by the same JIT
    u64 other_jit; ///< compiled before by another core's JIT
    u64 ranges;    ///< ranges invalidated
};
inline JitCounts (*g_take_jit)() = nullptr;

inline std::array<std::atomic<u64>, static_cast<size_t>(Counter::Count)> g_counters{};

[[nodiscard]] inline u64 ThresholdUs() {
    static const u64 threshold = [] {
        const char* value = std::getenv("EDEN_NX2_SLOW_FRAME_MS");
        if (value == nullptr) {
            return u64{0};
        }
        char* end = nullptr;
        const unsigned long long ms = std::strtoull(value, &end, 10);
        return end == value || *end != '\0' ? u64{0} : static_cast<u64>(ms) * 1000;
    }();
    return threshold;
}

[[nodiscard]] inline bool Enabled() {
    return ThresholdUs() != 0;
}

inline void Add(Counter counter, u64 value) {
    if (Enabled()) [[unlikely]] {
        g_counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
    }
}

inline void Max(Counter counter, u64 value) {
    auto& slot = g_counters[static_cast<size_t>(counter)];
    u64 current = slot.load(std::memory_order_relaxed);
    while (current < value &&
           !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

[[nodiscard]] inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// A timed wait: its count, its time and the longest of the frame.
inline void AddWait(Counter count, Counter total, Counter longest, s64 start_ns) {
    if (start_ns == 0) {
        return;
    }
    const u64 ns = static_cast<u64>(NowNs() - start_ns);
    g_counters[static_cast<size_t>(count)].fetch_add(1, std::memory_order_relaxed);
    g_counters[static_cast<size_t>(total)].fetch_add(ns, std::memory_order_relaxed);
    Max(longest, ns);
}

/// Adds the time from its construction to its destruction.
class Timer {
public:
    explicit Timer(Counter counter_) : counter{counter_}, start{Enabled() ? NowNs() : 0} {}
    ~Timer() {
        if (start != 0) {
            g_counters[static_cast<size_t>(counter)].fetch_add(static_cast<u64>(NowNs() - start),
                                                               std::memory_order_relaxed);
        }
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Counter counter;
    s64 start;
};

/// The counters since the last present, cleared.
[[nodiscard]] inline std::array<u64, static_cast<size_t>(Counter::Count)> Take() {
    std::array<u64, static_cast<size_t>(Counter::Count)> values{};
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = g_counters[i].exchange(0, std::memory_order_relaxed);
    }
    return values;
}

} // namespace Common::Nx2SlowFrame
