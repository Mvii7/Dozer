// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Service {

// Compressed frames waiting for the Switch 2 video decoder thread. The client can hand over a
// movie faster than it decodes (Mario Kart World's opening VP9 goes in as a burst). Queuing that
// burst makes the first pictures late: the thread is still working through packets while the
// audio clock has already started, so the opening stutters and the rest, submitted as frames are
// shown, plays evenly. Past RefuseAt the session answers busy (nn::codec 3603) and the client
// waits for the submission-ready event.
class Nx2VideoInputQueue {
public:
    struct Input {
        std::vector<u8> data;
        u64 pts;
        bool end;
    };

    /// The submission-ready event stops below this many packets, as before the cap.
    static constexpr size_t RoomBelow = 8;
    /// Submit answers busy from this many packets on. End of stream is still taken.
    static constexpr size_t RefuseAt = 24;

    /// Queues a packet. False: the session should answer busy. The refused packet is kept, not
    /// dropped. One client sends it again after the ready event (the copy is skipped); another
    /// ignores the result and sends the next packet, and a lost VP9 packet decodes against a
    /// missing reference until the next keyframe. The kept packet goes in ahead of that next one.
    bool Submit(Input input) {
        if (held) {
            if (held->pts != input.pts || held->data != input.data) {
                inputs.push_back(std::move(*held));
            }
            held.reset();
        }
        if (!input.end && Full()) {
            held = std::move(input);
            return false;
        }
        inputs.push_back(std::move(input));
        return true;
    }

    Input Pop() {
        auto input = std::move(inputs.front());
        inputs.pop_front();
        return input;
    }

    void Clear() {
        inputs.clear();
        held.reset();
    }

    bool Empty() const {
        return inputs.empty();
    }

    size_t Size() const {
        return inputs.size();
    }

    bool HasRoom() const {
        return inputs.size() < RoomBelow;
    }

    bool Full() const {
        return inputs.size() >= RefuseAt;
    }

private:
    std::deque<Input> inputs;
    std::optional<Input> held;
};

} // namespace Service
