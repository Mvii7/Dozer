// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_sync_diag.h"

// EDEN_NX2_SYNC_DIAG=1 (logging only), GPU side: the semaphores the split front waits on and what
// met them, the drains acquires make and how much queued back work they waited for (all of it, and
// how much of that came after the release the acquire needed), how long the front sleeps on a
// suspended channel, and how long releases the front sent take to run on the back (puller and
// engine releases; QMD releases until their fence lands). Logged every 10 s by the front. The guest
// threads' side is common/nx2_sync_diag.h.
namespace VideoCommon::Nx2SyncDiag {

using Common::Nx2SyncDiag::NowNs;
using Common::Nx2SyncDiag::On;

/// How a waiting acquire was met (the puller's ResumeHow, same order).
enum class Met : u8 {
    Queued,
    Memory,
    Latched,
    PeekLatched,
    PeekSkip,
    DrainLatched,
    DrainMemory,
    HostPending,
    Count,
};
constexpr std::array<const char*, static_cast<size_t>(Met::Count)> met_names{
    "queued", "memory", "latch", "peek latch", "peek", "drain latch", "drain memory", "host"};

/// Why an acquire drained (the puller's CheckWithoutDrain, or a suspended acquire's poll).
enum class DrainWhy : u8 {
    Operation, ///< Not greater-or-equal (or equal without NoDrainEqual)
    Settings,  ///< Split, front macros or releases-at-once off
    Unknown,   ///< A release the front can't see is queued
    Word,      ///< The buffer cache may hold the word newer than memory
    Compute,   ///< A QMD may release the word after the queued release
    Queued,    ///< The queued release does not meet it (or its value is unknown)
    NotMet,    ///< Memory does not meet it, nothing queued (valve, or first check)
    Poll,      ///< A suspended acquire's poll
    Count,
};
constexpr std::array<const char*, static_cast<size_t>(DrainWhy::Count)> drain_why_names{
    "operation", "settings", "unknown release", "GPU-written word", "compute", "queued",
    "not met", "suspended poll"};

/// Written by the back stages, read by the front's report.
struct BackCounters {
    std::atomic<u64> releases{};
    std::atomic<u64> release_ns{};
    std::atomic<u64> release_max_ns{};
    std::atomic<u64> release_words{};
    std::atomic<u64> qmd_releases{};
    std::atomic<u64> qmd_landed{};
    std::atomic<u64> qmd_land_ns{};
    std::atomic<u64> qmd_land_max_ns{};
};
inline BackCounters g_back;

inline void Max(std::atomic<u64>& max, u64 value) {
    u64 seen = max.load(std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

/// Back: a release sent at `sent_ns` with `words` queued ahead of it ran now.
inline void NoteReleaseRan(s64 sent_ns, u64 words) {
    const u64 ns = static_cast<u64>((std::max<s64>)(0, NowNs() - sent_ns));
    g_back.releases.fetch_add(1, std::memory_order_relaxed);
    g_back.release_ns.fetch_add(ns, std::memory_order_relaxed);
    g_back.release_words.fetch_add(words, std::memory_order_relaxed);
    Max(g_back.release_max_ns, ns);
}

/// Back: a QMD release went behind a fence; it landed `queued_ns` (NowNs then) later.
inline void NoteQmdRelease() {
    g_back.qmd_releases.fetch_add(1, std::memory_order_relaxed);
}
inline void NoteQmdLanded(s64 queued_ns) {
    const u64 ns = static_cast<u64>((std::max<s64>)(0, NowNs() - queued_ns));
    g_back.qmd_landed.fetch_add(1, std::memory_order_relaxed);
    g_back.qmd_land_ns.fetch_add(ns, std::memory_order_relaxed);
    Max(g_back.qmd_land_max_ns, ns);
}

/// The front thread's counts.
class Front {
public:
    /// An acquire was checked; `passed`: it went on without a drain or a wait.
    void NoteAcquire(bool passed) {
        ++acquires;
        passed_now += passed ? 1 : 0;
    }

    /// A channel suspended on an acquire, with `words` queued for the back.
    void NoteWaitStart(const void* owner, u64 words) {
        for (Waiting& waiting : waits) {
            if (waiting.owner == owner) {
                waiting.words = words;
                return;
            }
        }
        waits.push_back({owner, words});
    }

    /// The suspended acquire on `device` was met after `us`, by `met`; `words` queued now.
    void NoteWaitEnd(const void* owner, u64 device, Met met, u64 us, u64 words) {
        u64 words_at_start = 0;
        for (size_t i = 0; i < waits.size(); ++i) {
            if (waits[i].owner == owner) {
                words_at_start = waits[i].words;
                waits[i] = waits.back();
                waits.pop_back();
                break;
            }
        }
        auto& by = met_stats[static_cast<size_t>(met)];
        ++by.count;
        by.us += us;
        by.max_us = (std::max)(by.max_us, us);
        by.words_start += words_at_start;
        by.words_end += words;
        Address& address = addresses[device];
        ++address.waits;
        address.wait_us += us;
        address.max_us = (std::max)(address.max_us, us);
        ++address.met[static_cast<size_t>(met)];
    }

    /// An acquire on `device` drained the back for `ns`, `total` words queued, `why`.
    /// `met_before`: memory met it before the drain. `release`: words queued up to the release
    /// the front sent to the word (0 for none). `met_after`: memory or a latch met it after.
    void NoteDrain(u64 device, DrainWhy why, bool met_before, u64 total, u64 release, u64 ns,
                   bool met_after) {
        auto& drain = drains[static_cast<size_t>(why)];
        ++drain.count;
        drain.ns += ns;
        drain.max_ns = (std::max)(drain.max_ns, ns);
        drain.words += total;
        drain.met_before += met_before ? 1 : 0;
        drain.met_after += met_after ? 1 : 0;
        if (release != 0) {
            ++drain.with_release;
            drain.release_words += release;
            drain.after_release_words += total > release ? total - release : 0;
        }
        Address& address = addresses[device];
        ++address.drains;
        address.drain_ns += ns;
    }

    /// The front blocked in the OS for a suspended channel (`ns`; `notified`: woken early).
    void NoteSleep(u64 ns, bool notified) {
        ++sleeps;
        sleep_ns += ns;
        sleep_timeouts += notified ? 0 : 1;
        after_timeout = !notified;
    }
    /// A pass over the suspended channels; `resumed`: one went on.
    void NoteResumePass(bool resumed) {
        if (resumed && after_timeout) {
            ++resumed_after_timeout;
        }
        after_timeout = false;
    }

    /// Logs every 10 s (front loop).
    void Report() {
        const s64 now = NowNs();
        if (window_start == 0) {
            window_start = now;
            return;
        }
        if (now - window_start < 10'000'000'000LL) {
            return;
        }
        Log(static_cast<double>(now - window_start) / 1e9);
        window_start = now;
    }

private:
    struct Waiting {
        const void* owner;
        u64 words;
    };
    struct MetStats {
        u64 count{};
        u64 us{};
        u64 max_us{};
        u64 words_start{};
        u64 words_end{};
    };
    struct DrainStats {
        u64 count{};
        u64 ns{};
        u64 max_ns{};
        u64 words{};
        u64 met_before{};
        u64 met_after{};
        u64 with_release{};
        u64 release_words{};
        u64 after_release_words{};
    };
    struct Address {
        u64 waits{};
        u64 wait_us{};
        u64 max_us{};
        u64 drains{};
        u64 drain_ns{};
        std::array<u32, static_cast<size_t>(Met::Count)> met{};
    };

    void Log(double seconds) {
        const auto avg = [](u64 total, u64 count) {
            return count == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(count);
        };
        std::string met_line;
        for (size_t i = 0; i < met_stats.size(); ++i) {
            const MetStats& m = met_stats[i];
            if (m.count == 0) {
                continue;
            }
            met_line += fmt::format(
                "{}{} {} ({:.1f} ms, avg {:.0f} max {:.0f} us; queued words start/end {:.0f}/{:.0f})",
                met_line.empty() ? "" : ", ", met_names[i], m.count,
                static_cast<double>(m.us) / 1000.0, avg(m.us, m.count),
                static_cast<double>(m.max_us), avg(m.words_start, m.count),
                avg(m.words_end, m.count));
        }
        std::string drain_line;
        for (size_t i = 0; i < drains.size(); ++i) {
            const DrainStats& d = drains[i];
            if (d.count == 0) {
                continue;
            }
            drain_line += fmt::format(
                "{}{} {} ({:.1f} ms, max {:.2f}; words {:.0f}; met before {} after {}; with a "
                "queued release {}: words to it {:.0f}, after it {:.0f})",
                drain_line.empty() ? "" : ", ", drain_why_names[i], d.count,
                static_cast<double>(d.ns) / 1e6, static_cast<double>(d.max_ns) / 1e6,
                avg(d.words, d.count), d.met_before, d.met_after, d.with_release,
                avg(d.release_words, d.with_release), avg(d.after_release_words, d.with_release));
        }
        std::vector<std::pair<u64, const Address*>> top;
        for (const auto& [device, address] : addresses) {
            top.emplace_back(device, &address);
        }
        std::ranges::sort(top, [](const auto& a, const auto& b) {
            return a.second->wait_us * 1000 + a.second->drain_ns >
                   b.second->wait_us * 1000 + b.second->drain_ns;
        });
        std::string address_line;
        for (size_t i = 0; i < top.size() && i < 6; ++i) {
            const Address& a = *top[i].second;
            std::string met;
            for (size_t k = 0; k < a.met.size(); ++k) {
                if (a.met[k] != 0) {
                    met += fmt::format(" {} {}", met_names[k], a.met[k]);
                }
            }
            address_line += fmt::format(
                "{}{:#x}: waits {} {:.1f} ms (max {:.1f}){}, drains {} {:.1f} ms",
                i == 0 ? "" : "; ", top[i].first, a.waits, static_cast<double>(a.wait_us) / 1000.0,
                static_cast<double>(a.max_us) / 1000.0, met, a.drains,
                static_cast<double>(a.drain_ns) / 1e6);
        }
        const auto take = [](std::atomic<u64>& value) {
            return value.exchange(0, std::memory_order_relaxed);
        };
        const u64 releases = take(g_back.releases);
        const u64 release_ns = take(g_back.release_ns);
        const u64 release_max_ns = take(g_back.release_max_ns);
        const u64 release_words = take(g_back.release_words);
        const u64 qmd = take(g_back.qmd_releases);
        const u64 qmd_landed = take(g_back.qmd_landed);
        const u64 qmd_ns = take(g_back.qmd_land_ns);
        const u64 qmd_max_ns = take(g_back.qmd_land_max_ns);
        LOG_INFO(HW_GPU,
                 "GPU syncs in {:.1f} s (EDEN_NX2_SYNC_DIAG): acquires {} (went on at once {}) | "
                 "waits by what met them: {} | acquire drains by why: {} | front OS sleeps {} "
                 "({:.1f} ms, {} ran out, a channel went on after {}) | releases sent to the "
                 "back {}: ran after avg {:.2f} max {:.2f} ms, {:.0f} words ahead | QMD fenced "
                 "releases {}, landed {} after avg {:.2f} max {:.2f} ms | top semaphores: {}",
                 seconds, acquires, passed_now, met_line.empty() ? "none" : met_line,
                 drain_line.empty() ? "none" : drain_line, sleeps,
                 static_cast<double>(sleep_ns) / 1e6, sleep_timeouts, resumed_after_timeout,
                 releases, avg(release_ns, releases) / 1e6,
                 static_cast<double>(release_max_ns) / 1e6, avg(release_words, releases), qmd,
                 qmd_landed, avg(qmd_ns, qmd_landed) / 1e6, static_cast<double>(qmd_max_ns) / 1e6,
                 address_line.empty() ? "none" : address_line);
        acquires = passed_now = 0;
        met_stats = {};
        drains = {};
        addresses.clear();
        sleeps = sleep_ns = sleep_timeouts = resumed_after_timeout = 0;
    }

    u64 acquires{};
    u64 passed_now{};
    std::vector<Waiting> waits;
    std::array<MetStats, static_cast<size_t>(Met::Count)> met_stats{};
    std::array<DrainStats, static_cast<size_t>(DrainWhy::Count)> drains{};
    std::unordered_map<u64, Address> addresses;
    u64 sleeps{};
    u64 sleep_ns{};
    u64 sleep_timeouts{};
    u64 resumed_after_timeout{};
    bool after_timeout{};
    s64 window_start{};
};

inline Front& GetFront() {
    static Front front;
    return front;
}

} // namespace VideoCommon::Nx2SyncDiag
