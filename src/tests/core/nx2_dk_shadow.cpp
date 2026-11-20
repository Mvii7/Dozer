// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_shadow.h"

namespace {
using namespace Loader::Nx2DkShadow;

// A text segment of 0x1e00 bytes in 0x2000: the free space starts at word 0x780.
constexpr u32 TextSize = 0x1e00;
constexpr u32 AlignedSize = 0x2000;
constexpr size_t Routine = 0x780 + CaveOffset;
constexpr size_t Site = 0x300;

struct Text {
    std::vector<u32> words;

    Text() : words(AlignedSize / 4, 0) {
        for (size_t i = 0; i < TextSize / 4; ++i) {
            words[i] = 0x12345678;
        }
        words[Site] = LoadCount;
        words[Site + 1] = CompareMost;
        words[Site + 2] = PickLower;
    }

    std::span<u8> Bytes() {
        return {reinterpret_cast<u8*>(words.data()), words.size() * 4};
    }
};
} // Anonymous namespace

TEST_CASE("Nx2DkShadow: the variable", "[core]") {
    REQUIRE(CascadesFrom(nullptr) == 0);
    REQUIRE(CascadesFrom("") == 0);
    REQUIRE(CascadesFrom("1") == 1);
    REQUIRE(CascadesFrom("3") == 3);
    REQUIRE(CascadesFrom("4") == 0); // the game's own: nothing to do
    REQUIRE(CascadesFrom("0") == 0);
    REQUIRE(CascadesFrom("12") == 0);
}

TEST_CASE("Nx2DkShadow: the cascades capped", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    REQUIRE(Apply(text.Bytes(), TextSize, AlignedSize, 2));
    expected[Site] = 0x14000000U | static_cast<u32>(Routine - Site);
    expected[Routine] = LoadCount;
    expected[Routine + 1] = 0x7100091F; // cmp w8, #2
    expected[Routine + 2] = 0x5400004D; // b.le +8
    expected[Routine + 3] = 0x52800048; // movz w8, #2
    expected[Routine + 4] = 0x14000000U | (static_cast<u32>((Site + 1) - (Routine + 4)) & 0x3FFFFFFU);
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkShadow: left alone when not found once or the space is taken", "[core]") {
    Text missing;
    missing.words[Site + 2] = 0;
    std::vector<u32> before = missing.words;
    REQUIRE_FALSE(Apply(missing.Bytes(), TextSize, AlignedSize, 1));
    REQUIRE(missing.words == before);

    Text twice;
    twice.words[Site + 0x40] = LoadCount;
    twice.words[Site + 0x41] = CompareMost;
    twice.words[Site + 0x42] = PickLower;
    before = twice.words;
    REQUIRE_FALSE(Apply(twice.Bytes(), TextSize, AlignedSize, 1));
    REQUIRE(twice.words == before);

    Text taken;
    taken.words[Routine + 3] = 1;
    before = taken.words;
    REQUIRE_FALSE(Apply(taken.Bytes(), TextSize, AlignedSize, 1));
    REQUIRE(taken.words == before);

    Text game;
    before = game.words;
    REQUIRE_FALSE(Apply(game.Bytes(), TextSize, AlignedSize, MostCascades));
    REQUIRE(game.words == before);
}
