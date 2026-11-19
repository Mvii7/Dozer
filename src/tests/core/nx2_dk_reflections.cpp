// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_reflections.h"

namespace {
using namespace Loader::Nx2DkReflections;

constexpr u32 TextSize = 0x1000, AlignedSize = 0x2000;
constexpr size_t Getter = 0x100, Flag = 0x200, Routine = TextSize / 4 + CaveOffset;

struct Text {
    std::vector<u32> words;

    // The text, then the free space up to the next page.
    Text() : words(AlignedSize / 4, 0) {
        std::fill(words.begin(), words.begin() + TextSize / 4, 0x12345678);
        words[Getter] = IntervalLoad;
        words[Getter + 1] = ValueLoad;
        words[Getter + 2] = Ret;
        words[Flag] = 0xF9401549;     // ldr x9, [x10, #0x28]
        words[Flag + 1] = 0x3941912B; // ldrb w11, [x9, #0x64]
    }

    std::span<u8> Bytes() {
        return {reinterpret_cast<u8*>(words.data()), words.size() * 4};
    }
};
} // Anonymous namespace

TEST_CASE("Nx2DkReflections: the variables", "[core]") {
    REQUIRE(IntervalFrom(nullptr) == 0);
    REQUIRE(IntervalFrom("24") == 24);
    REQUIRE(IntervalFrom("240") == 240);
    REQUIRE(IntervalFrom("241") == 0);
    REQUIRE(IntervalFrom("0") == 0);
    REQUIRE(IntervalFrom("2x") == 0);
    REQUIRE(OffFrom("1"));
    REQUIRE_FALSE(OffFrom("0"));
    REQUIRE_FALSE(OffFrom(nullptr));
}

TEST_CASE("Nx2DkReflections: the interval and off", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Result result = Apply(text.Bytes(), TextSize, AlignedSize, 24, false);
    REQUIRE(result.interval);
    REQUIRE_FALSE(result.off);
    expected[Getter] = 0x52800300; // movz w0, #24
    expected[Getter + 1] = Ret;
    REQUIRE(text.words == expected);

    Text off;
    expected = off.words;
    result = Apply(off.Bytes(), TextSize, AlignedSize, 0, true);
    REQUIRE(result.off);
    REQUIRE_FALSE(result.interval);
    // The parameter itself is turned off, not only the register.
    expected[Flag + 1] = 0x14000000 | (Routine - (Flag + 1)); // b routine
    expected[Routine] = 0x3901913F;                           // strb wzr, [x9, #0x64]
    expected[Routine + 1] = 0x5280000B;                       // movz w11, #0
    expected[Routine + 2] = 0x14000000 | ((Flag + 2 - (Routine + 2)) & 0x3FFFFFF); // b back
    REQUIRE(off.words == expected);
}

TEST_CASE("Nx2DkReflections: off left alone without free space", "[core]") {
    Text used;
    used.words[Routine + 1] = 0x12345678;
    const std::vector<u32> before = used.words;
    REQUIRE_FALSE(Apply(used.Bytes(), TextSize, AlignedSize, 0, true).off);
    REQUIRE(used.words == before);

    Text small;
    const std::vector<u32> unchanged = small.words;
    REQUIRE_FALSE(Apply(small.Bytes(), TextSize, TextSize, 0, true).off);
    REQUIRE(small.words == unchanged);
}

TEST_CASE("Nx2DkReflections: left alone when not found once, or the game's own", "[core]") {
    Text twice;
    twice.words[Getter + 0x20] = IntervalLoad;
    twice.words[Getter + 0x21] = ValueLoad;
    twice.words[Getter + 0x22] = Ret;
    twice.words[Flag + 0x20] = 0xF9401549;
    twice.words[Flag + 0x21] = 0x3941912B;
    const std::vector<u32> before = twice.words;
    const Result result = Apply(twice.Bytes(), TextSize, AlignedSize, 24, true);
    REQUIRE_FALSE(result.interval);
    REQUIRE_FALSE(result.off);
    REQUIRE(twice.words == before);

    Text game;
    const std::vector<u32> unchanged = game.words;
    REQUIRE_FALSE(Apply(game.Bytes(), TextSize, AlignedSize, GameInterval, false).interval);
    REQUIRE(game.words == unchanged);
}
