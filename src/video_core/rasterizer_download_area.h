// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "common/common_types.h"

namespace VideoCore {

struct RasterizerDownloadArea {
    VAddr start_address;
    VAddr end_address;
    bool preemtive;
    /// A read-trapped buffer flush is among it (EDEN_NX2_READ_TRAP_BUDGET_US counts its wait).
    bool read_trap = false;
};

/// EDEN_NX2_ASYNC_READBACK: where a read's buffer bytes are: the tick its downloads were
/// submitted in, and the newest one it lands (newer ones belong to later reads).
struct ReadPoint {
    u64 tick;
    u64 seq;
};

/// EDEN_NX2_ASYNC_READBACK: read downloads submitted and not yet in guest memory, and the hull of
/// the bytes they write. Set under the buffer cache's lock, read without it by writes to guest
/// memory from the GPU's side (MemoryManager), which first finish the ones they overlap.
struct ReadDownloadsState {
    std::atomic<u32> pending{};
    std::atomic<DAddr> begin{~DAddr{0}};
    std::atomic<DAddr> end{0};
};
inline ReadDownloadsState g_read_downloads;
/// EDEN_NX2_ASYNC_READBACK=2: writes from the GPU's side are told about (FinishGpuSideWrite) even
/// with no download pending, so the read checks know guest memory changed there.
inline std::atomic<bool> g_read_checks{};

[[nodiscard]] inline bool ReadDownloadsMayCover(DAddr addr, u64 size) noexcept {
    return g_read_downloads.pending.load(std::memory_order_acquire) != 0 &&
           addr < g_read_downloads.end.load(std::memory_order_acquire) &&
           addr + size > g_read_downloads.begin.load(std::memory_order_acquire);
}

/// Calls its argument for each guest CPU write collected and not yet applied to the caches (set
/// by the GPU: the dirty memory managers and the writes InvalidateGPUCache is applying). A
/// download landing off the GPU thread keeps those bytes.
using PendingCpuWritesFn = void (*)(void* context, const std::function<void(DAddr, u64)>& func);
inline PendingCpuWritesFn g_pending_cpu_writes{};
inline void* g_pending_cpu_writes_context{};

/// What DownloadLanding publishes.
struct DownloadLandingState {
    std::atomic<u64> sequence{}; ///< Odd while a landing is published
    std::atomic<DAddr> begin{};
    std::atomic<DAddr> end{};
    std::atomic<u64> waits{};
    std::atomic<bool> enabled{};
};

/// The buffer cache writing downloaded GPU bytes to guest memory [begin, end) off the GPU thread
/// (after Lichii). A guest core collects a write (Core::GPUDirtyMemoryManager::Collect) and
/// stores its bytes right after, without the buffer cache's lock: one collected after a landing
/// looked at the collected writes would have its bytes written over by the GPU's older ones. A
/// write collected inside a landing's range stores its bytes once the landing has ended (Await).
/// Each side fences between its store and its load: either the landing finds the write, or the
/// write finds the landing. Landings happen under the buffer cache's lock, one at a time.
class DownloadLanding {
public:
    explicit DownloadLanding(DAddr begin, DAddr end) noexcept {
        state.begin.store(begin, std::memory_order_relaxed);
        state.end.store(end, std::memory_order_relaxed);
        state.sequence.fetch_add(1, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    ~DownloadLanding() {
        state.sequence.fetch_add(1, std::memory_order_release);
    }
    DownloadLanding(const DownloadLanding&) = delete;
    DownloadLanding& operator=(const DownloadLanding&) = delete;

    /// Turned on with the first landing off the GPU thread (until then nothing can race).
    static void Enable() noexcept {
        state.enabled.store(true, std::memory_order_release);
    }

    /// A guest CPU write of [addr, addr + size) was collected: returns once its bytes may be
    /// stored.
    static void Await(DAddr addr, u64 size) noexcept {
        if (!state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const u64 sequence = state.sequence.load(std::memory_order_acquire);
        if ((sequence & 1) != 0) {
            Wait(sequence, addr, size);
        }
    }

    [[nodiscard]] static u64 Waits() noexcept {
        return state.waits.load(std::memory_order_relaxed);
    }

private:
    static void Wait(u64 sequence, DAddr addr, u64 size) noexcept {
        const DAddr begin = state.begin.load(std::memory_order_relaxed);
        const DAddr end = state.end.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (state.sequence.load(std::memory_order_acquire) != sequence || addr >= end ||
            addr + size <= begin) {
            return;
        }
        state.waits.fetch_add(1, std::memory_order_relaxed);
        while (state.sequence.load(std::memory_order_acquire) == sequence) {
            std::this_thread::yield();
        }
    }

    static inline DownloadLandingState state;
};

} // namespace VideoCore
