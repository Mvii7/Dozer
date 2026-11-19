// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/range_sets.inc"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/control/channel_state_cache.inc"

namespace VideoCommon {

template class VideoCommon::ChannelSetupCaches<VideoCommon::BufferCacheChannelInfo>;

} // namespace VideoCommon

// The buffer cache header calls these from inline members: one copy of each for every user.
namespace Common {

template class RangeSet<DAddr>;
template class OverlapRangeSet<DAddr>;

} // namespace Common

