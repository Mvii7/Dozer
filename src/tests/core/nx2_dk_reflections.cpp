// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_reflections.h"

namespace {
using namespace Loader::Nx2DkReflections;

constexpr u32 TextSize = 0x1000;
constexpr size_t Getter = 0x100;

struct Text {
    std::vector<u32> words;

    Text() : words(TextSize / 4, 0x12345678) {
        words[Getter] = IntervalLoad;
        words[Getter + 1] = ValueLoad;
        words[Getter + 2] = Ret;
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
    Result result = Apply(text.Bytes(), TextSize, 24, false);
    REQUIRE(result.interval);
    REQUIRE_FALSE(result.off);
    expected[Getter] = 0x52800300; // movz w0, #24
    expected[Getter + 1] = Ret;
    REQUIRE(text.words == expected);

    // Off keeps the cube map and only stops its captures, whatever the interval asked.
    Text off;
    expected = off.words;
    result = Apply(off.Bytes(), TextSize, 24, true);
    REQUIRE(result.off);
    REQUIRE_FALSE(result.interval);
    expected[Getter] = 0x529FFFE0; // movz w0, #0xffff
    expected[Getter + 1] = Ret;
    REQUIRE(off.words == expected);
}

TEST_CASE("Nx2DkReflections: left alone when not found once, or the game's own", "[core]") {
    Text twice;
    twice.words[Getter + 0x20] = IntervalLoad;
    twice.words[Getter + 0x21] = ValueLoad;
    twice.words[Getter + 0x22] = Ret;
    const std::vector<u32> before = twice.words;
    const Result result = Apply(twice.Bytes(), TextSize, 24, true);
    REQUIRE_FALSE(result.interval);
    REQUIRE_FALSE(result.off);
    REQUIRE(twice.words == before);

    Text game;
    const std::vector<u32> unchanged = game.words;
    REQUIRE_FALSE(Apply(game.Bytes(), TextSize, GameInterval, false).interval);
    REQUIRE(game.words == unchanged);
}
