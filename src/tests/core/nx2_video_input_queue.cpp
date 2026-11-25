// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_video_input_queue.h"

namespace {

using Input = Service::Nx2VideoInputQueue::Input;

Service::Nx2VideoInputQueue FullQueue() {
    Service::Nx2VideoInputQueue queue;
    for (u64 index = 0; index < queue.RefuseAt; ++index) {
        REQUIRE(queue.Submit(Input{{static_cast<u8>(index)}, index * 16667, false}));
    }
    return queue;
}

std::vector<u64> DrainPts(Service::Nx2VideoInputQueue& queue) {
    std::vector<u64> pts;
    while (!queue.Empty()) {
        pts.push_back(queue.Pop().pts);
    }
    return pts;
}

} // namespace

TEST_CASE("Nx2 video input queue takes packets up to the busy watermark", "[core]") {
    Service::Nx2VideoInputQueue queue;
    for (u64 index = 0; index < queue.RefuseAt; ++index) {
        REQUIRE(queue.Submit(Input{{static_cast<u8>(index)}, index * 16667, false}));
        REQUIRE(queue.HasRoom() == (index + 1 < queue.RefuseAt));
    }
    REQUIRE(queue.Full());
    REQUIRE_FALSE(queue.Submit(Input{{0xAB}, queue.RefuseAt * 16667, false}));
    REQUIRE(queue.Size() == queue.RefuseAt);

    REQUIRE(queue.Pop().pts == 0);
    REQUIRE(queue.HasRoom());
    REQUIRE(queue.Submit(Input{{0xAB}, queue.RefuseAt * 16667, false}));
    REQUIRE(queue.Size() == queue.RefuseAt);
}

TEST_CASE("Nx2 video input queue keeps one copy of a resent packet", "[core]") {
    auto queue = FullQueue();
    const Input packet{{0xAB, 0xCD}, queue.RefuseAt * 16667, false};
    REQUIRE_FALSE(queue.Submit(packet));
    REQUIRE_FALSE(queue.Submit(packet));
    REQUIRE(queue.Size() == queue.RefuseAt);

    queue.Pop();
    REQUIRE(queue.Submit(packet));
    auto pts = DrainPts(queue);
    REQUIRE(pts.front() == 16667);
    REQUIRE(pts.back() == packet.pts);
    REQUIRE(pts.size() == queue.RefuseAt);
}

TEST_CASE("Nx2 video input queue keeps a packet the client does not resend", "[core]") {
    auto queue = FullQueue();
    const u64 first = queue.RefuseAt;
    REQUIRE_FALSE(queue.Submit(Input{{0x01}, first * 16667, false}));
    REQUIRE_FALSE(queue.Submit(Input{{0x02}, (first + 1) * 16667, false}));
    REQUIRE(queue.Size() == first + 1);

    while (queue.Size() > 2) {
        queue.Pop();
    }
    REQUIRE(queue.Submit(Input{{0x03}, (first + 2) * 16667, false}));
    REQUIRE(DrainPts(queue) == std::vector<u64>{(first - 1) * 16667, first * 16667,
                                                 (first + 1) * 16667, (first + 2) * 16667});
}

TEST_CASE("Nx2 video input queue accepts end of stream while full", "[core]") {
    auto queue = FullQueue();
    const u64 refused = queue.RefuseAt;
    REQUIRE_FALSE(queue.Submit(Input{{0x01}, refused * 16667, false}));
    REQUIRE(queue.Submit(Input{{}, (refused + 1) * 16667, true}));

    auto pts = DrainPts(queue);
    REQUIRE(pts.size() == refused + 2);
    REQUIRE(pts[refused] == refused * 16667);
    REQUIRE(pts.back() == (refused + 1) * 16667);
}

TEST_CASE("Nx2 video input queue clear drops a held packet", "[core]") {
    auto queue = FullQueue();
    REQUIRE_FALSE(queue.Submit(Input{{0x01}, queue.RefuseAt * 16667, false}));
    queue.Clear();
    REQUIRE(queue.Empty());
    REQUIRE(queue.HasRoom());
    REQUIRE(queue.Submit(Input{{0x02}, 0, false}));
    REQUIRE(DrainPts(queue) == std::vector<u64>{0});
}
