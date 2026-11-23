// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging.h"
#include "common/net/net.h"
#include "common/scm_rev.h"
#include "update_checker.h"

std::optional<Common::Net::Release> UpdateChecker::GetUpdate() {
    auto latest = Common::Net::GetLatestRelease();
    if (!latest)
        return std::nullopt;

    LOG_INFO(Frontend, "Received update {}", latest->title);

    // Builds carry their commit rather than a version number, so ask the host whether the latest
    // release is ahead of this commit.
    const auto status = Common::Net::GetCompareStatus(latest->tag, Common::g_scm_rev);
    LOG_INFO(Frontend, "Compare of {} against this build: {}", latest->tag,
             status.value_or("unknown"));
    if (status == "behind")
        return latest;

    // A build from a release branch is not an ancestor of the next release, so the host calls it
    // diverged. Offer the update if the release came out after this build was made.
    if (status == "diverged") {
        const u64 built = Common::Net::ParseIsoTimestamp(Common::g_build_date);
        if (built != 0 && built < latest->published)
            return latest;
    }

    return std::nullopt;
}
