// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/range_sets.h"
#include "common/range_sets.inc"

namespace {

std::vector<std::pair<u64, u64>> Pieces(const Common::RangeSet<u64>& set, u64 addr, u64 size) {
    std::vector<std::pair<u64, u64>> pieces;
    set.ForEachInRange(addr, size,
                       [&](u64 lower, u64 upper) { pieces.emplace_back(lower, upper); });
    return pieces;
}

} // Anonymous namespace

TEST_CASE("RangeSet: an empty range finds nothing", "[common]") {
    Common::RangeSet<u64> set;
    set.Add(0x861b8000, 0x1000);
    set.Add(0x90000000, 0x2000);
    // Bananza's null written bindings armed 0+0: every range came back as [lower, 0).
    REQUIRE(Pieces(set, 0, 0).empty());
    REQUIRE(Pieces(set, 0x90000000, 0).empty());
    REQUIRE(Pieces(set, 0x90001000, 0).empty());
    set.Subtract(0x90000000, 0);
    REQUIRE(Pieces(set, 0x90000000, 0x2000) ==
            std::vector<std::pair<u64, u64>>{{0x90000000, 0x90002000}});
    REQUIRE(Pieces(set, 0x80000000, 0x20000000) ==
            std::vector<std::pair<u64, u64>>{{0x861b8000, 0x861b9000}, {0x90000000, 0x90002000}});
}

TEST_CASE("OverlapRangeSet: an empty range finds and removes nothing", "[common]") {
    Common::OverlapRangeSet<u64> set;
    set.Add(0x861b8000, 0x1000);
    u64 found = 0;
    set.ForEachInRange(0, 0, [&](u64, u64, s32) { ++found; });
    REQUIRE(found == 0);
    set.Subtract(0x861b8000, 0);
    set.ForEachInRange(0x861b8000, 0x1000, [&](u64, u64, s32) { ++found; });
    REQUIRE(found == 1);
}
