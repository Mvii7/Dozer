// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include <QWidget>

#include "common/common_types.h"
#include "core/nx2_perf_settings.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

// The per-game Tuning tab: each box of the Tuning tab (core/nx2_perf_settings.h) as this game runs
// them, and Bananza's graphics levels with a preset that sets them. They follow the Tuning tab; a
// cross appears next to one this game changed, and resets it to the Tuning tab's. Only the
// changes are kept (UISettings::values.nx2perf_games).
class ConfigureNx2PerfPerGame : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNx2PerfPerGame(u64 title_id, QWidget* parent = nullptr);
    ~ConfigureNx2PerfPerGame() override;

    void ApplyConfiguration();

private:
    struct Row {
        QCheckBox* box;
        QLabel* source; ///< "Tuning tab" or "Changed"
        QPushButton* reset;
        std::size_t key; ///< the index in Core::Nx2PerfSettings::KeyAt
    };

    static constexpr int PresetPerformance = 0;
    static constexpr int PresetQuality = 1;
    static constexpr int PresetCustom = 2;

    void Refresh();
    void ChooseLevels(const Core::Nx2PerfSettings::BananzaLevels& levels);
    void ChooseBox(const Row& row, bool checked);

    u64 title_id;
    Core::Nx2PerfSettings::Game game;
    Core::Nx2PerfSettings::Overrides own;
    QComboBox* preset = nullptr; ///< Bananza's graphics preset
    QPushButton* preset_reset = nullptr;
    QComboBox* foliage = nullptr; ///< Bananza's foliage
    QComboBox* shadow = nullptr; ///< Bananza's shadow quality
    QComboBox* reflections = nullptr; ///< Bananza's reflection updates
    std::vector<Row> rows;
};
