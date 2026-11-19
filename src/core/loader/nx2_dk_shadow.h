// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include "common/common_types.h"
#include "core/loader/nx2_dk_dynamic_fps.h"

// EDEN_NX2_DK_SHADOW_CASCADES=<1-3> (unset: the game's own, up to 4): Donkey Kong Bananza's
// Shadow quality in the Tuning tab (Low 1, Medium 2, High 3, Very high the game's). It caps the
// cascades the sun's shadow is drawn in; each cascade draws every shadow caster again, so fewer
// cascades means fewer draws, the rest stretched over the same distance (blurrier shadows). Put
// in the main module as it loads, only when its instructions are found exactly where expected.
namespace Loader::Nx2DkShadow {

constexpr const char* Switch = "EDEN_NX2_DK_SHADOW_CASCADES";
constexpr int MostCascades = 4;

/// The variable's cascades: 1 to 3, or 0 for none (unset, the game's own, or anything else).
constexpr int CascadesFrom(const char* value) {
    if (value == nullptr || value[0] < '1' || value[0] > '3' || value[1] != '\0') {
        return 0;
    }
    return value[0] - '0';
}

/// Words of the free space past the end of the text segment the routine takes: after the
/// dynamic FPS patch (the first 19) and the Quality tab's routines.
constexpr size_t CaveOffset = 64;
constexpr size_t CaveWords = 5;

// The gsys depth shadow settings hold depth_shadow_cascade_num_0 at +0x1a48; each shadow is set
// up with it clamped to its own most cascades:
//   ldr w8, [x27, #0x1a48]; cmp w8, w9; csel w8, w8, w9, lt
// The load becomes a jump to a routine that caps it first (the flags are set again next):
//   ldr w8, [x27, #0x1a48]; cmp w8, #n; b.le +8; movz w8, #n; b back
constexpr u32 LoadCount = 0xB95A4B68U;  // ldr w8, [x27, #0x1a48]
constexpr u32 CompareMost = 0x6B09011FU; // cmp w8, w9
constexpr u32 PickLower = 0x1A89B108U;   // csel w8, w8, w9, lt

// `text` starts at the text segment and holds at least `aligned_size` bytes, the segment's size
// rounded up to whole pages; `text_size` is its size in the NSO header. Whether it was put in.
inline bool Apply(std::span<u8> text, u32 text_size, u32 aligned_size, int cascades) {
    if (cascades < 1 || cascades >= MostCascades || text.size() < aligned_size ||
        aligned_size < text_size) {
        return false;
    }
    const auto word = [&](size_t index) {
        u32 value;
        std::memcpy(&value, text.data() + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto put = [&](size_t index, u32 value) {
        std::memcpy(text.data() + index * sizeof(u32), &value, sizeof(value));
    };
    const size_t count = text_size / sizeof(u32);
    size_t found = 0, at = 0;
    for (size_t i = 0; i + 2 < count; ++i) {
        if (word(i) == LoadCount && word(i + 1) == CompareMost && word(i + 2) == PickLower) {
            at = i;
            ++found;
        }
    }
    const size_t routine = (text_size + 15) / 16 * 4 + CaveOffset;
    if (found != 1 || (routine + CaveWords) * sizeof(u32) > aligned_size) {
        return false;
    }
    for (size_t i = 0; i < CaveWords; ++i) {
        if (word(routine + i) != 0) {
            return false;
        }
    }
    const auto branch = [](size_t from, size_t to) {
        return 0x14000000U | (static_cast<u32>(to - from) & 0x3FFFFFFU);
    };
    const u32 n = static_cast<u32>(cascades);
    put(routine, LoadCount);
    put(routine + 1, 0x7100011FU | (n << 10)); // cmp w8, #n
    put(routine + 2, 0x5400004DU);             // b.le +8: past the movz, to the jump back
    put(routine + 3, 0x52800008U | (n << 5));  // movz w8, #n
    put(routine + 4, branch(routine + 4, at + 1));
    put(at, branch(at, routine));
    return true;
}

/// Whether a module is the one the setting is for: Bananza's main.
constexpr bool IsTarget(u64 program_id, std::string_view module_name) {
    return module_name == "main" && program_id == Nx2DkDynamicFps::ProgramId;
}

} // namespace Loader::Nx2DkShadow
