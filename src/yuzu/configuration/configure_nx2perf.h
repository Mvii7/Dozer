// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <QWidget>

#include "core/nx2_perf_settings.h"

class QCheckBox;

namespace Ui {
class ConfigureNx2Perf;
}

// The Tuning tab: Dozer's settings (core/nx2_perf_settings.h), one box each, which every game
// follows unless its own tab says otherwise. A setup (Default, Experimental, Safe, the launchers')
// only fills the boxes in once. A box whose variables were all set outside the program
// (yuzu/nx2perf_session.h) is greyed out: theirs is used.
class ConfigureNx2Perf : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNx2Perf(QWidget* parent = nullptr);
    ~ConfigureNx2Perf() override;

    void ApplyConfiguration();

    /// A setup also sets Reactive Flushing in Graphics, Advanced, through this (false: it could
    /// not, a game is running).
    void SetReactiveFlushing(std::function<bool(bool, bool)> func);

    /// A game runs: Reactive Flushing cannot change now; the tab keeps it
    /// for the restart or the game's end (Nx2PerfSession::AddPendingGraphics).
    void SetGameRunning(bool running);

private:
    struct Row {
        QCheckBox* box;
        bool Core::Nx2PerfSettings::Switches::*field;
        std::vector<std::string_view> names;
    };

    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void SetConfiguration();
    Core::Nx2PerfSettings::Switches Current() const;
    void Show(const Core::Nx2PerfSettings::Switches& switches);
    void ShowSetups();
    void ApplySetup(Core::Nx2PerfSettings::Setup chosen);
    void MarkSetOutside();
    void ShowPending();

    std::unique_ptr<Ui::ConfigureNx2Perf> ui;
    std::vector<Row> rows;
    std::function<bool(bool, bool)> reactive_flushing;
    bool game_running = false;
    std::optional<bool> pending_reactive;
    std::optional<bool> pending_barrier;
};
