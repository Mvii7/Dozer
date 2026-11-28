// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_SYNC_DIAG=1 (logging only): what the game and the GPU threads wait on each other for.
// This part: the guest threads' time in SVCs, by kind (svc.cpp), against the time they spend
// outside them (running guest code, or ready without a core), and nvhost event waits. Logged
// every 10 s. The GPU side is video_core/nx2_sync_diag.h.
namespace Common::Nx2SyncDiag {

inline bool On() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_SYNC_DIAG");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

enum class SvcKind : u8 {
    Sleep,   ///< svcSleepThread with a time
    Yield,   ///< svcSleepThread 0 / -1 / -2
    Sync,    ///< svcWaitSynchronization
    Lock,    ///< svcArbitrateLock
    CondVar, ///< svcWaitProcessWideKeyAtomic
    Address, ///< svcWaitForAddress
    Ipc,     ///< svcSendSyncRequest(WithUserBuffer), svcReplyAndReceive
    Other,
    Count,
};
constexpr std::array<const char*, static_cast<size_t>(SvcKind::Count)> svc_kind_names{
    "sleep", "yield", "sync", "lock", "cv", "addr", "ipc", "other"};

inline SvcKind ClassifySvc(u32 imm, s64 arg0) {
    switch (imm) {
    case 0x0B:
        return arg0 <= 0 ? SvcKind::Yield : SvcKind::Sleep;
    case 0x18:
        return SvcKind::Sync;
    case 0x1A:
        return SvcKind::Lock;
    case 0x1C:
        return SvcKind::CondVar;
    case 0x34:
        return SvcKind::Address;
    case 0x21:
    case 0x22:
    case 0x43:
        return SvcKind::Ipc;
    default:
        return SvcKind::Other;
    }
}

/// nvhost_ctrl event waits (IocCtrlEventWait) by result: done, not yet (the guest waits on the
/// event or polls again), an error.
enum class EventWait : u8 { Signalled, NotYet, Error, Count };

class GuestThreads {
public:
    static GuestThreads& Get() {
        static GuestThreads threads;
        return threads;
    }

    /// An SVC of the guest thread `id` took `ns` (wall). `cpu_ticks`: the thread's CPU time
    /// (KThread::GetCpuTime, CNTPCT ticks). `name` is called once per thread.
    template <typename NameFunc>
    void NoteSvc(u64 id, u32 imm, s64 arg0, s64 start_ns, s64 ns, s64 cpu_ticks,
                 const NameFunc& name) {
        const SvcKind kind = ClassifySvc(imm, arg0);
        std::scoped_lock lk{mutex};
        Thread* thread = Find(id);
        if (thread == nullptr) {
            threads.push_back(Thread{.id = id, .name = name(), .cpu_start = cpu_ticks});
            thread = &threads.back();
        }
        if (thread->cpu_start < 0) {
            thread->cpu_start = cpu_ticks;
        }
        thread->cpu_end = cpu_ticks;
        auto& entry = thread->kinds[static_cast<size_t>(kind)];
        ++entry.calls;
        entry.ns += ns;
        const s64 end_ns = start_ns + ns;
        if (window_start == 0) {
            window_start = end_ns;
        } else if (end_ns - window_start >= 10'000'000'000LL) {
            Log(end_ns);
            window_start = end_ns;
        }
    }

    void NoteEventWait(EventWait outcome) {
        event_waits[static_cast<size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
    }

private:
    struct Kind {
        u64 calls{};
        s64 ns{};
    };
    struct Thread {
        u64 id{};
        std::string name;
        std::array<Kind, static_cast<size_t>(SvcKind::Count)> kinds{};
        s64 cpu_start{-1};
        s64 cpu_end{};
    };

    Thread* Find(u64 id) {
        for (Thread& thread : threads) {
            if (thread.id == id) {
                return &thread;
            }
        }
        return nullptr;
    }

    // Per second of the window: a thread's run time is the window less its time in SVCs (it
    // includes being ready without a core); cpu is the kernel's count (updated at context
    // switches only, so a thread that never left its core shows less).
    void Log(s64 now_ns) {
        const double seconds = static_cast<double>(now_ns - window_start) / 1e9;
        const auto per_s = [seconds](double value) { return value / seconds; };
        struct Row {
            const Thread* thread;
            s64 svc_ns;
        };
        std::vector<Row> rows;
        for (const Thread& thread : threads) {
            s64 svc_ns = 0;
            u64 calls = 0;
            for (const Kind& kind : thread.kinds) {
                svc_ns += kind.ns;
                calls += kind.calls;
            }
            if (calls != 0) {
                rows.push_back({&thread, svc_ns});
            }
        }
        std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.svc_ns < b.svc_ns; });
        std::string line;
        for (size_t i = 0; i < rows.size() && i < 8; ++i) {
            const Thread& thread = *rows[i].thread;
            const double run_ms =
                (std::max)(0.0, per_s(static_cast<double>((now_ns - window_start) - rows[i].svc_ns)) / 1e6);
            const double cpu_ms = per_s(static_cast<double>(thread.cpu_end - thread.cpu_start)) *
                                  1000.0 / 19'200'000.0;
            std::string kinds;
            for (size_t k = 0; k < thread.kinds.size(); ++k) {
                const Kind& kind = thread.kinds[k];
                if (kind.calls == 0) {
                    continue;
                }
                kinds += fmt::format(" {} {:.0f} ({:.0f}/s)", svc_kind_names[k],
                                     per_s(static_cast<double>(kind.ns)) / 1e6,
                                     per_s(static_cast<double>(kind.calls)));
            }
            line += fmt::format("{}{} t{}: run {:.0f} cpu {:.0f} |{}", i == 0 ? "" : "; ",
                                thread.name, thread.id, run_ms, cpu_ms, kinds);
        }
        const auto waits = [this](EventWait outcome) {
            return event_waits[static_cast<size_t>(outcome)].exchange(0, std::memory_order_relaxed);
        };
        const u64 signalled = waits(EventWait::Signalled);
        const u64 not_yet = waits(EventWait::NotYet);
        const u64 error = waits(EventWait::Error);
        LOG_INFO(Kernel_SVC,
                 "Guest threads in {:.1f} s, most running first, ms/s (EDEN_NX2_SYNC_DIAG): {} | "
                 "nvhost event waits: done {}, not yet {}, error {}",
                 seconds, line.empty() ? "none" : line, signalled, not_yet, error);
        // Threads that made no SVC in the window drop out (they may have exited).
        std::erase_if(threads, [](const Thread& thread) {
            return std::ranges::all_of(thread.kinds, [](const Kind& kind) { return kind.calls == 0; });
        });
        for (Thread& thread : threads) {
            thread.kinds = {};
            thread.cpu_start = -1;
        }
    }

    std::mutex mutex;
    std::vector<Thread> threads;
    s64 window_start{};
    std::array<std::atomic<u64>, static_cast<size_t>(EventWait::Count)> event_waits{};
};

} // namespace Common::Nx2SyncDiag
