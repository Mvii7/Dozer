// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include "common/common_types.h"
#include "core/loader/nx2_dk_dynamic_fps.h"

// Donkey Kong Bananza's reflections: the cube map the game captures of the scene around the camera
// (drawing the scene again into its faces) and its shiny and wet surfaces reflect.
//   EDEN_NX2_DK_CUBEMAP_INTERVAL=<1-240>: frames between two captures (the game's: 8), only by
//   hand; higher, fewer draws, reflections follow later.
//   EDEN_NX2_DK_REFLECTIONS_OFF=1: no cube map at all (IsUseCubeMap off): no captures, no
//   reflections. The Reflections off box in Bananza's Properties > Tuning.
// Both are put in the main module as it loads, only where the instructions are exactly as expected.
namespace Loader::Nx2DkReflections {

constexpr const char* IntervalSwitch = "EDEN_NX2_DK_CUBEMAP_INTERVAL";
constexpr const char* OffSwitch = "EDEN_NX2_DK_REFLECTIONS_OFF";
constexpr int GameInterval = 8;
constexpr int MostInterval = 240;

/// The interval a variable asks for: 1 to 240, or 0 (unset or anything else).
constexpr int IntervalFrom(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    int interval = 0;
    for (size_t i = 0; value[i] != '\0'; ++i) {
        if (value[i] < '0' || value[i] > '9' || i >= 3) {
            return 0;
        }
        interval = interval * 10 + (value[i] - '0');
    }
    return interval >= 1 && interval <= MostInterval ? interval : 0;
}

constexpr bool OffFrom(const char* value) {
    return value != nullptr && std::string_view{value} == "1";
}

// A graphics parameter is an object with its value at +0x64, held by GraphicsParam at a fixed
// offset. CubeMapShotInterval (+0x220) is read by one getter:
//   ldr x8, [x0, #0x220]; ldr w0, [x8, #0x64]; ret        -> movz w0, #interval; ret
// IsUseCubeMap (+0x28) is read in place once, where the game builds its directors:
//   ldr xM, [xK, #0x28]; ldrb wN, [xM, #0x64]             -> the load becomes movz wN, #0
constexpr u32 IntervalLoad = 0xF9411008U; // ldr x8, [x0, #0x220]
constexpr u32 ValueLoad = 0xB9406500U;    // ldr w0, [x8, #0x64]
constexpr u32 Ret = 0xD65F03C0U;
constexpr u32 CubeMapParam = 0x28;
constexpr u32 ValueOffset = 0x64;

struct Result {
    bool interval = false; ///< the interval was put in
    bool off = false;      ///< the cube map was turned off
};

// `text` starts at the text segment; `text_size` is its size in the NSO header.
inline Result Apply(std::span<u8> text, u32 text_size, int interval, bool off) {
    Result result;
    if (text.size() < text_size) {
        return result;
    }
    const size_t count = text_size / sizeof(u32);
    const auto word = [&](size_t index) {
        u32 value;
        std::memcpy(&value, text.data() + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto put = [&](size_t index, u32 value) {
        std::memcpy(text.data() + index * sizeof(u32), &value, sizeof(value));
    };
    if (interval >= 1 && interval <= MostInterval && interval != GameInterval) {
        size_t found = 0, at = 0;
        for (size_t i = 0; i + 2 < count; ++i) {
            if (word(i) == IntervalLoad && word(i + 1) == ValueLoad && word(i + 2) == Ret) {
                at = i;
                ++found;
            }
        }
        if (found == 1) {
            put(at, 0x52800000U | (static_cast<u32>(interval) << 5)); // movz w0, #interval
            put(at + 1, Ret);
            result.interval = true;
        }
    }
    if (off) {
        size_t found = 0, at = 0;
        for (size_t i = 0; i + 1 < count; ++i) {
            const u32 first = word(i), second = word(i + 1);
            if ((first & 0xFFC00000U) == 0xF9400000U && ((first >> 10) & 0xFFFU) * 8 == CubeMapParam &&
                (second & 0xFFFFFC00U) == (0x39400000U | (ValueOffset << 10)) &&
                ((second >> 5) & 31U) == (first & 31U)) {
                at = i + 1;
                ++found;
            }
        }
        if (found == 1) {
            put(at, 0x52800000U | (word(at) & 31U)); // movz wN, #0
            result.off = true;
        }
    }
    return result;
}

/// Whether a module is the one the settings are for: Bananza's main.
constexpr bool IsTarget(u64 program_id, std::string_view module_name) {
    return module_name == "main" && program_id == Nx2DkDynamicFps::ProgramId;
}

} // namespace Loader::Nx2DkReflections
