// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/read_traps.h"

using Core::Memory::ReadTraps;

namespace {
std::vector<u64> Disarmed(ReadTraps& traps, u64 begin, u64 end, u64 flushed_generation) {
    std::vector<u64> pages;
    traps.Disarm(begin, end, flushed_generation, [&](u64 page) { pages.push_back(page); });
    return pages;
}

std::vector<u64> Armed(const ReadTraps& traps) {
    std::vector<u64> pages;
    traps.ForEach(0, ~0ULL, [&](u64 page) { pages.push_back(page); });
    return pages;
}
} // namespace

TEST_CASE("ReadTraps: a flushed read disarms what was armed before it", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(10, traps.NewGeneration());
    const u64 snapshot = traps.Generation();
    REQUIRE(Disarmed(traps, 10, 11, snapshot) == std::vector<u64>{10});
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: a re-arm during the flush survives the disarm", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(10, traps.NewGeneration());
    const u64 snapshot = traps.Generation();
    traps.Arm(10, traps.NewGeneration());
    REQUIRE(Disarmed(traps, 10, 11, snapshot).empty());
    REQUIRE(Armed(traps) == std::vector<u64>{10});
    REQUIRE(Disarmed(traps, 10, 11, traps.Generation()) == std::vector<u64>{10});
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: a read that started before the arm keeps the trap", "[core][read_traps]") {
    ReadTraps traps;
    const u64 snapshot = traps.Generation();
    traps.Arm(5, traps.NewGeneration());
    REQUIRE(Disarmed(traps, 5, 6, snapshot).empty());
    REQUIRE_FALSE(traps.Empty());
}

TEST_CASE("ReadTraps: disarm and drop stay inside their page range", "[core][read_traps]") {
    ReadTraps traps;
    const u64 generation = traps.NewGeneration();
    for (const u64 page : {1, 2, 3, 10}) {
        traps.Arm(page, generation);
    }
    REQUIRE(Disarmed(traps, 2, 4, generation) == std::vector<u64>{2, 3});
    REQUIRE(Armed(traps) == std::vector<u64>{1, 10});
    traps.Drop(0, 2);
    REQUIRE(Armed(traps) == std::vector<u64>{10});
    traps.Drop(10, 11);
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: re-arming a page counts it once", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(7, traps.NewGeneration());
    traps.Arm(7, traps.NewGeneration());
    traps.Drop(7, 8);
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: clear keeps generations increasing", "[core][read_traps]") {
    ReadTraps traps;
    traps.Arm(3, traps.NewGeneration());
    const u64 before_clear = traps.Generation();
    traps.Clear();
    REQUIRE(traps.Empty());
    REQUIRE(Armed(traps).empty());
    REQUIRE(traps.NewGeneration() > before_clear);
}

TEST_CASE("ReadTraps: mapping changes invalidate cached read areas without armed pages",
          "[core][read_traps]") {
    ReadTraps traps;
    const u64 before_drop = traps.Generation();
    traps.Drop(10, 11);
    REQUIRE(traps.Generation() > before_drop);
    const u64 before_clear = traps.Generation();
    traps.Clear();
    REQUIRE(traps.Generation() > before_clear);
}

TEST_CASE("ReadTraps: out-of-order read completions preserve later GPU writes",
          "[core][read_traps][mkw_sync]") {
    ReadTraps traps;
    traps.Arm(10, traps.NewGeneration());
    const u64 first_read = traps.Generation();
    traps.Arm(10, traps.NewGeneration());
    const u64 second_read = traps.Generation();
    traps.Arm(11, traps.NewGeneration());

    REQUIRE(Disarmed(traps, 10, 12, second_read) == std::vector<u64>{10});
    REQUIRE(Armed(traps) == std::vector<u64>{11});
    traps.Arm(10, traps.NewGeneration());
    REQUIRE(Disarmed(traps, 10, 12, first_read).empty());
    REQUIRE(Armed(traps) == std::vector<u64>{10, 11});
    REQUIRE(Disarmed(traps, 10, 12, traps.Generation()) == std::vector<u64>{10, 11});
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: old completion cannot disarm a reused mapping",
          "[core][read_traps][mkw_sync]") {
    ReadTraps traps;
    traps.Arm(10, traps.NewGeneration());
    const u64 old_read = traps.Generation();
    SECTION("one mapping is retired") {
        traps.Drop(10, 11);
    }
    SECTION("the session is reset") {
        traps.Clear();
    }
    REQUIRE(traps.Empty());
    traps.Arm(10, traps.NewGeneration());
    REQUIRE(Disarmed(traps, 10, 11, old_read).empty());
    REQUIRE(Armed(traps) == std::vector<u64>{10});
    REQUIRE(Disarmed(traps, 10, 11, traps.Generation()) == std::vector<u64>{10});
    REQUIRE(traps.Empty());
}

TEST_CASE("ReadTraps: completing part of a batch does not consume other pages",
          "[core][read_traps][mkw_sync]") {
    ReadTraps traps;
    const u64 first_write = traps.NewGeneration();
    for (const u64 page : {9, 10, 11, 12}) {
        traps.Arm(page, first_write);
    }
    const u64 read = traps.Generation();
    traps.Arm(11, traps.NewGeneration());
    REQUIRE(Disarmed(traps, 10, 12, read) == std::vector<u64>{10});
    REQUIRE(Armed(traps) == std::vector<u64>{9, 11, 12});
    REQUIRE(Disarmed(traps, 10, 12, traps.Generation()) == std::vector<u64>{11});
    REQUIRE(Armed(traps) == std::vector<u64>{9, 12});
    REQUIRE_FALSE(traps.Empty());
}

TEST_CASE("ReadTraps: the lock-free filter never misses an armed page", "[core][read_traps]") {
    ReadTraps traps;
    // Pages 7 and 7 + 2^15 share a filter slot (page & FilterMask).
    constexpr u64 a = 7;
    constexpr u64 b = 7 + (u64{1} << 15);
    REQUIRE_FALSE(traps.MayAny(a, a + 1));
    const u64 generation = traps.NewGeneration();
    traps.Arm(a, generation);
    traps.Arm(b, generation);
    // Arming an armed page again moves its generation, not its count.
    traps.Arm(a, traps.NewGeneration());
    REQUIRE(traps.MayAny(a, a + 1));
    REQUIRE(traps.MayAny(b, b + 1));
    REQUIRE(traps.Any(a, a + 1));
    // a disarmed: the shared slot still holds b.
    REQUIRE(Disarmed(traps, a, a + 1, traps.Generation()) == std::vector<u64>{a});
    REQUIRE_FALSE(traps.Any(a, a + 1));
    REQUIRE(traps.MayAny(a, a + 1));
    REQUIRE(traps.MayAny(b, b + 1));
    // Disarming an unarmed page changes nothing.
    REQUIRE(Disarmed(traps, a, a + 1, traps.Generation()).empty());
    REQUIRE(traps.MayAny(b, b + 1));
    // b dropped: the slot is empty again.
    traps.Drop(b, b + 1);
    REQUIRE_FALSE(traps.MayAny(a, a + 1));
    REQUIRE_FALSE(traps.MayAny(b, b + 1));
    // A range wider than the filter is always "may".
    REQUIRE(traps.MayAny(0, (u64{1} << 15) + 1));
    // Clear empties every slot.
    traps.Arm(a, traps.NewGeneration());
    traps.Arm(100, traps.Generation());
    traps.Clear();
    REQUIRE_FALSE(traps.MayAny(a, a + 1));
    REQUIRE_FALSE(traps.MayAny(100, 101));
    REQUIRE(traps.Empty());
}
