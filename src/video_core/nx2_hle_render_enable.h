// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>
#include <span>

#include "common/cityhash.h"
#include "common/common_types.h"

// EDEN_NX2_HLE_RENDER_ENABLE (Maxwell3D's split front, FrontRunMacro): NVN's render enable
// macro, {parameter, address high, address low}, reads a ReportSemaphore::Compare at the address
// (MME_DMA_READ_FIFOED: the front drains the back first) and sets render_enable.mode to the
// parameter when its two counters are equal, to the parameter ^ 1 when they differ. =1 sends the
// macro's other writes and leaves the read and the mode to the back, in the stream's order (with
// the stages parked, as ProcessQueryCondition reads it), so the front does not wait; =2 runs the
// macro as before and has the back compare its own result with the front's. 0 or unset: off.
namespace VideoCommon::Nx2HleRenderEnable {

inline u32 Mode() {
    static const u32 mode = [] {
        const char* value = std::getenv("EDEN_NX2_HLE_RENDER_ENABLE");
        if (value == nullptr || (value[0] != '1' && value[0] != '2')) {
            return 0U;
        }
        return static_cast<u32>(value[0] - '0');
    }();
    return mode;
}

/// Whether the macro at `start` is NVN's render enable macro (its 9 instructions' hash).
inline bool Identify(std::span<const u32> code, u32 start) {
    constexpr u64 Hash = 0xb55c2fa5e78f65a9ULL;
    constexpr size_t Words = 9 * 3;
    const size_t begin = size_t{start} * 3;
    return begin + Words <= code.size() &&
           Common::CityHash64(reinterpret_cast<const char*>(code.data() + begin),
                              Words * sizeof(u32)) == Hash;
}

/// The words the macro reads: a ReportSemaphore::Compare.
inline constexpr u32 ReadWords = 6;

/// The mode the macro sets for `parameter` and the words it read: words 0-1 against 4-5.
inline constexpr u32 ModeFor(u32 parameter, const std::array<u32, ReadWords>& words) {
    const bool differ = ((words[0] ^ words[4]) | (words[1] ^ words[5])) != 0;
    return differ ? parameter ^ 1 : parameter;
}

} // namespace VideoCommon::Nx2HleRenderEnable
