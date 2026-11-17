// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

/// Frames an image must go unused before the collector deletes it while the device has room.
/// Mario Kart World's races keep far more images in use than the collector's floor: deleting the
/// ones unused for 50 frames made dozens again every frame, uploads included.
constexpr u64 MinEvictionAge = 150;

/// Frames since its last use before the collector may delete an image. `mode_age` is the age the
/// collector's mode asks for; it applies as is only when memory is short: no limit is known (0)
/// or the usage has reached `limit`. `override_age` (EDEN_NX2_TC_MIN_AGE) raises the age only
/// while there is room.
[[nodiscard]] constexpr u64 EvictionAge(u64 mode_age, u64 override_age, u64 usage, u64 limit) {
    if (limit == 0 || usage >= limit) {
        return mode_age;
    }
    return (std::max)({mode_age, MinEvictionAge, override_age});
}

} // namespace VideoCommon
