// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include <QWidget>

#include "common/common_types.h"

class QComboBox;

// The per-game Tuning tab: each box of the Tuning tab (core/nx2_perf_settings.h) can follow the
// global setting (for Bananza and Mario Kart World, their built-in default) or be set on / off for
// this game only. Only the choices made here are kept (UISettings::values.nx2perf_games).
class ConfigureNx2PerfPerGame : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNx2PerfPerGame(u64 title_id, QWidget* parent = nullptr);
    ~ConfigureNx2PerfPerGame() override;

    void ApplyConfiguration();

private:
    struct Row {
        QComboBox* combo;
        std::size_t key; ///< the index in Core::Nx2PerfSettings::KeyAt
    };

    u64 title_id;
    std::vector<Row> rows;
};
