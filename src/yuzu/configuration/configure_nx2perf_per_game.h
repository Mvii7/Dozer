// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include <QWidget>

#include "common/common_types.h"
#include "core/nx2_perf_settings.h"

class QComboBox;

// The per-game Tuning tab: the preset and each box of the Tuning tab (core/nx2_perf_settings.h) can
// follow the game's default (the global setting; Bananza and Mario Kart World have a preset of
// their own) or be set for this game only. Only the choices made here are kept
// (UISettings::values.nx2perf_games).
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

    /// The settings the boxes' Default is: the game's base with the chosen preset.
    Core::Nx2PerfSettings::Switches Base() const;
    void ShowDefaults();

    u64 title_id;
    QComboBox* preset = nullptr;
    std::vector<Row> rows;
};
