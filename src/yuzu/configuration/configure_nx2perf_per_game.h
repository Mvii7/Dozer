// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <QWidget>

#include "common/common_types.h"
#include "core/nx2_perf_settings.h"

class QCheckBox;
class QComboBox;
class QPushButton;

// The per-game Tuning tab: the preset and each box of the Tuning tab (core/nx2_perf_settings.h) as
// this game runs them. They mirror the global ones; a cross appears next to one this game changed
// (Bananza's preset starts changed), and resets it to the global setting. Only the changes are
// kept (UISettings::values.nx2perf_games).
class ConfigureNx2PerfPerGame : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNx2PerfPerGame(u64 title_id, QWidget* parent = nullptr);
    ~ConfigureNx2PerfPerGame() override;

    void ApplyConfiguration();

private:
    struct Row {
        QCheckBox* box;
        QPushButton* reset;
        std::size_t key; ///< the index in Core::Nx2PerfSettings::KeyAt
    };

    /// The presets this game's combo lists, in order (Custom last).
    std::span<const Core::Nx2PerfSettings::Preset> Order() const;
    void Refresh();
    void ChoosePreset(Core::Nx2PerfSettings::Preset chosen);
    void ChooseBox(const Row& row, bool checked);

    u64 title_id;
    Core::Nx2PerfSettings::Game game;
    Core::Nx2PerfSettings::Overrides own;
    QComboBox* preset = nullptr;
    QPushButton* preset_reset = nullptr;
    QComboBox* shadow = nullptr; ///< Bananza's shadow quality
    std::vector<Row> rows;
};
