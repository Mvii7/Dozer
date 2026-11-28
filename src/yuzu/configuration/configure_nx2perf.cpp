// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_nx2perf.h"

#include <cstdlib>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QStringList>

#include "qt_common/config/uisettings.h"
#include "ui_configure_nx2perf.h"
#include "yuzu/nx2perf_session.h"

namespace Perf = Core::Nx2PerfSettings;

ConfigureNx2Perf::ConfigureNx2Perf(QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureNx2Perf>()} {
    ui->setupUi(this);

    using S = Perf::Switches;
    rows = {
        {ui->read_traps, &S::read_traps, {"EDEN_NX2_READ_TRAPS"}},
        {ui->split_gpu_thread, &S::split_gpu_thread, {"EDEN_NX2_BACK_STAGES", "EDEN_NX2_BACK_CUTS"}},
        {ui->acquire_peek, &S::acquire_peek, {"EDEN_NX2_ACQUIRE_PEEK"}},
        {ui->jit_same, &S::jit_same, {"EDEN_NX2_JIT_SAME"}},
        {ui->front_macro_lean, &S::front_macro_lean, {"EDEN_NX2_FRONT_MACRO_LEAN"}},
        {ui->fg_timed, &S::fg_timed, {"EDEN_NX2_FG_TIMED"}},
        {ui->osd, &S::osd, {"EDEN_NX2_OSD"}},
        // The shortcuts are many variables; they are not listed as set outside.
        {ui->safe_off, &S::safe_off, {}},
        {ui->rt_fit, &S::rt_fit, {"EDEN_NX2_RX_RT_FIT"}},
    };

    SetConfiguration();
    MarkSetOutside();

    // The combo's items are a hint, then Perf::Setup in order; it goes back to the hint.
    connect(ui->setup, &QComboBox::activated, this, [this](int index) {
        if (index > 0 && index <= static_cast<int>(Perf::Setup::Safe) + 1) {
            ApplySetup(static_cast<Perf::Setup>(index - 1));
        }
        const QSignalBlocker blocker{ui->setup};
        ui->setup->setCurrentIndex(0);
    });
    ShowSetups();
}

ConfigureNx2Perf::~ConfigureNx2Perf() = default;

void ConfigureNx2Perf::SetReactiveFlushing(std::function<bool(bool, bool)> func) {
    reactive_flushing = std::move(func);
}

void ConfigureNx2Perf::SetGameRunning(bool running) {
    game_running = running;
}

void ConfigureNx2Perf::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNx2Perf::RetranslateUI() {
    ui->retranslateUi(this);
    ShowSetups();
    MarkSetOutside();
}

void ConfigureNx2Perf::SetConfiguration() {
    Show(UISettings::values.nx2perf);
    if (Nx2PerfSession::HasPendingGraphics()) {
        ShowPending();
    }
}

Perf::Switches ConfigureNx2Perf::Current() const {
    Perf::Switches switches{};
    for (const Row& row : rows) {
        switches.*row.field = row.box->isChecked();
    }
    return switches;
}

void ConfigureNx2Perf::Show(const Perf::Switches& switches) {
    for (const Row& row : rows) {
        const QSignalBlocker blocker{row.box};
        row.box->setChecked(switches.*row.field);
    }
}

void ConfigureNx2Perf::ShowSetups() {
    ui->setup_text->setText(
        tr("Default: the fastest settings, like start-default.bat. Experimental: Reactive "
           "Flushing instead of read traps, like start-reactive-experimental.bat. Safe: "
           "everything off, like start-safe.bat. A preset only ticks the boxes below; change "
           "any of them afterwards."));
}

void ConfigureNx2Perf::ApplySetup(Perf::Setup chosen) {
    ui->status->clear();
    Show(Perf::Apply(chosen, Current()));
    const auto reactive = Perf::ReactiveFlushing(chosen);
    if (!reactive) {
        return;
    }
    const bool barrier_on = Perf::BarrierFeedbackLoops(chosen).value_or(false);
    if (game_running) {
        pending_reactive = *reactive;
        pending_barrier = Perf::BarrierFeedbackLoops(chosen);
        ShowPending();
        return;
    }
    if (!reactive_flushing || !reactive_flushing(*reactive, barrier_on)) {
        ui->status->setText(tr("Reactive Flushing (Graphics, Advanced) can only be changed with no "
                               "game running; set it there afterwards."));
    }
}

void ConfigureNx2Perf::ShowPending() {
    ui->status->setText(tr("A game is running: Reactive Flushing cannot change in it. It is applied "
                           "when the program restarts (Restart Now) or when the game stops."));
}

void ConfigureNx2Perf::ApplyConfiguration() {
    UISettings::values.nx2perf = Current();
    if (game_running) {
        Nx2PerfSession::AddPendingGraphics({.reactive_flushing = pending_reactive,
                                            .barrier_feedback_loops = pending_barrier});
    }
}

void ConfigureNx2Perf::MarkSetOutside() {
    for (const Row& row : rows) {
        if (row.names.empty()) {
            continue;
        }
        QStringList outside;
        for (const std::string_view name : row.names) {
            if (Nx2PerfSession::SetOutside(name)) {
                // The value now: the program never sets one that was there at its start.
                const std::string key{name};
                const char* value = std::getenv(key.c_str());
                outside << QStringLiteral("%1=%2").arg(QString::fromStdString(key),
                                                       QString::fromUtf8(value ? value : ""));
            }
        }
        // All set outside: the box changes nothing, its value is theirs.
        const bool all_outside = outside.size() == static_cast<qsizetype>(row.names.size());
        row.box->setEnabled(!all_outside);
        if (outside.isEmpty()) {
            row.box->setToolTip(QString{});
        } else if (all_outside) {
            row.box->setToolTip(tr("Set outside the program (your environment, a .bat of your own "
                                   "or nx2_switches.txt), so this is used whatever the box says: "
                                   "%1")
                                    .arg(outside.join(QStringLiteral(", "))));
        } else {
            row.box->setToolTip(tr("Partly set outside the program, and that part is used: %1. "
                                   "The box sets the rest.")
                                    .arg(outside.join(QStringLiteral(", "))));
        }
    }
}
