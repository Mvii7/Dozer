// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <span>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_COND_DIAG=1 (logging only): NVN's render enable macro on the split front. Each call
// reads two 64-bit counters from GPU memory (MME_DMA_READ_FIFOED, so the front drains the back
// first) and sets render_enable.mode to True or False by whether they differ. Logs every 10 s:
// its calls by parameter, the modes they set (and how often the mode changed), how often the
// read waited for the back or downloaded from the GPU and the time it took, and the draws and
// clears the back skipped for a False mode (DrawManager's ShouldExecute).
namespace VideoCommon::Nx2CondDiag {

inline bool On() {
    static const bool on = [] {
        const char* value = std::getenv("EDEN_NX2_COND_DIAG");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

/// Draws and clears the back skipped for render enable (any thread).
inline std::atomic<u64> g_skipped_draws{};
inline std::atomic<u64> g_skipped_clears{};

inline void NoteSkippedDraw() {
    if (On()) [[unlikely]] {
        g_skipped_draws.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void NoteSkippedClear() {
    if (On()) [[unlikely]] {
        g_skipped_clears.fetch_add(1, std::memory_order_relaxed);
    }
}

/// The front thread's counts.
class Counter {
public:
    /// Whether the macro at `start` is the render enable macro (its code, once per generation).
    bool IsMacro(std::span<const u32> code, u64 generation, u32 start) {
        if (generation != code_generation) {
            code_generation = generation;
            kinds.fill(0);
        }
        if (start >= kinds.size()) {
            return false;
        }
        u8& kind = kinds[start];
        if (kind == 0) {
            constexpr u64 Hash = 0xb55c2fa5e78f65a9ULL;
            constexpr size_t Words = 9 * 3;
            const size_t begin = size_t{start} * 3;
            const bool match =
                begin + Words <= code.size() &&
                Common::CityHash64(reinterpret_cast<const char*>(code.data() + begin),
                                   Words * sizeof(u32)) == Hash;
            kind = match ? 2 : 1;
            if (match) {
                LOG_INFO(HW_GPU, "Macro at {:#x} is NVN's render enable macro (EDEN_NX2_COND_DIAG)",
                         start);
            }
        }
        return kind == 2;
    }

    /// A call's read (FrontDmaRead): whether it waited for the back, downloaded, and its time.
    void NoteRead(bool waited, bool downloaded, u64 ns) {
        reads += 1;
        reads_waited += waited ? 1 : 0;
        reads_downloaded += downloaded ? 1 : 0;
        read_ns += ns;
    }

    /// A call ran with parameter `parameter`, leaving render_enable.mode at `mode`.
    void NoteCall(u32 parameter, u32 mode) {
        ++calls;
        ++by_parameter[parameter < 2 ? parameter : 2];
        ++by_mode[mode < 2 ? mode : 2];
        changed += mode != last_mode ? 1 : 0;
        last_mode = mode;
        const auto now = std::chrono::steady_clock::now();
        if (window_start == std::chrono::steady_clock::time_point{}) {
            window_start = now;
        } else if (now - window_start >= std::chrono::seconds{10}) {
            Log();
            window_start = now;
        }
    }

private:
    void Log() {
        const u64 draws = g_skipped_draws.exchange(0, std::memory_order_relaxed);
        const u64 clears = g_skipped_clears.exchange(0, std::memory_order_relaxed);
        LOG_INFO(HW_GPU,
                 "Render enable macro in 10 s (EDEN_NX2_COND_DIAG): {} calls (parameter 0/1/other "
                 "{}/{}/{}), mode set True {} False {} other {}, mode changed {} | reads {}: "
                 "waited for the back {}, downloaded {}, {:.1f} ms | skipped for a False mode: "
                 "{} draws, {} clears",
                 calls, by_parameter[0], by_parameter[1], by_parameter[2], by_mode[1], by_mode[0],
                 by_mode[2], changed, reads, reads_waited, reads_downloaded,
                 static_cast<double>(read_ns) / 1e6, draws, clears);
        calls = changed = reads = reads_waited = reads_downloaded = read_ns = 0;
        by_parameter = {};
        by_mode = {};
    }

    std::array<u8, 0x1000> kinds{};
    u64 code_generation{~0ULL};
    u64 calls{};
    std::array<u64, 3> by_parameter{};
    std::array<u64, 3> by_mode{};
    u64 changed{};
    u32 last_mode{~0U};
    u64 reads{};
    u64 reads_waited{};
    u64 reads_downloaded{};
    u64 read_ns{};
    std::chrono::steady_clock::time_point window_start{};
};

inline Counter& Get() {
    static Counter counter;
    return counter;
}

} // namespace VideoCommon::Nx2CondDiag
