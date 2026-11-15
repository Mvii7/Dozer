// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_frame_stats.h"

TEST_CASE("Nx2FrameStats: p95 by nearest rank", "[core]") {
    REQUIRE(Service::Nx2FrameStats::P95({}) == 0);
    std::array<u32, 1> one{33'000};
    REQUIRE(Service::Nx2FrameStats::P95(one) == 33'000);
    // 600 frames 1..600 in reverse: the 570th smallest.
    std::vector<u32> frames;
    for (u32 i = 600; i >= 1; --i) {
        frames.push_back(i);
    }
    REQUIRE(Service::Nx2FrameStats::P95(frames) == 570);
    // 599 of them (the first block): ceil(0.95 * 599) = 570 too.
    frames.clear();
    for (u32 i = 1; i <= 599; ++i) {
        frames.push_back(i * 100);
    }
    REQUIRE(Service::Nx2FrameStats::P95(frames) == 57'000);
    // 20 frames: the 19th.
    std::vector<u32> twenty;
    for (u32 i = 1; i <= 20; ++i) {
        twenty.push_back(i);
    }
    REQUIRE(Service::Nx2FrameStats::P95(twenty) == 19);
    // A few long frames above the 95th percentile do not move it.
    std::vector<u32> hitches(600, 33'000);
    for (size_t i = 0; i < 25; ++i) {
        hitches[i * 20] = 150'000;
    }
    REQUIRE(Service::Nx2FrameStats::P95(hitches) == 33'000);
    std::vector<u32> more(600, 33'000);
    for (size_t i = 0; i < 31; ++i) {
        more[i * 19] = 150'000;
    }
    REQUIRE(Service::Nx2FrameStats::P95(more) == 150'000);
}

TEST_CASE("Nx2FrameStats: p99 by nearest rank", "[core]") {
    // 600 frames 1..600: the 594th smallest.
    std::vector<u32> frames;
    for (u32 i = 600; i >= 1; --i) {
        frames.push_back(i);
    }
    REQUIRE(Service::Nx2FrameStats::Percentile(frames, 990) == 594);
    // Seven long frames in 600 set the p99, six do not.
    std::vector<u32> seven(600, 16'700);
    for (size_t i = 0; i < 7; ++i) {
        seven[i * 80] = 120'000;
    }
    REQUIRE(Service::Nx2FrameStats::Percentile(seven, 990) == 120'000);
    std::vector<u32> six(600, 16'700);
    for (size_t i = 0; i < 6; ++i) {
        six[i * 100] = 120'000;
    }
    REQUIRE(Service::Nx2FrameStats::Percentile(six, 990) == 16'700);
}

TEST_CASE("Nx2FrameStats: lows from the run histogram", "[core]") {
    Service::Nx2FrameStats::Histogram run;
    REQUIRE(run.Percentile(990) == 0);
    // 10,000 frames at 16.7 ms with 11 hitches of 250 ms (10 would sit above the 99.9th
    // percentile): the 0.1% low is the hitch, the 1% low the bucket edge above 16.7 ms.
    for (u32 i = 0; i < 9'989; ++i) {
        run.Add(16'700);
    }
    for (u32 i = 0; i < 11; ++i) {
        run.Add(250'000);
    }
    REQUIRE(run.Count() == 10'000);
    REQUIRE(run.Percentile(990) == 16'800);
    REQUIRE(run.Percentile(999) == 250'000);
    // One frame past the last bucket reports the longest frame.
    Service::Nx2FrameStats::Histogram one;
    one.Add(3'000'000);
    REQUIRE(one.Percentile(999) == 3'000'000);
    // A frame on a bucket edge never reports more than the longest frame.
    Service::Nx2FrameStats::Histogram edge;
    edge.Add(16'000);
    REQUIRE(edge.Percentile(990) == 16'000);
}
