// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_nx2perf_per_game.h"

#include <array>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "core/nx2_perf_settings.h"
#include "qt_common/config/uisettings.h"

namespace Perf = Core::Nx2PerfSettings;

namespace {

enum Choice : int {
    Default = 0,
    On = 1,
    Off = 2,
};

struct Entry {
    bool Perf::Switches::*field;
    Perf::Game game; ///< Other: every game
    const char* label;
};

// The order of Perf::Keys, then Perf::GameKeys.
const Entry* EntryOf(std::size_t key) {
    using S = Perf::Switches;
    static const Entry entries[Perf::KeyCount] = {
        {&S::read_traps, Perf::Game::Other, QT_TR_NOOP("Read traps")},
        {&S::split_gpu_thread, Perf::Game::Other, QT_TR_NOOP("Split GPU thread")},
        {&S::acquire_peek, Perf::Game::Other, QT_TR_NOOP("Acquire peek")},
        {&S::jit_same, Perf::Game::Other, QT_TR_NOOP("Same-value filter in the macro JIT")},
        {&S::front_macro_lean, Perf::Game::Other, QT_TR_NOOP("Lean front macro path")},
        {&S::fg_timed, Perf::Game::Other, QT_TR_NOOP("Timed frame generation")},
        {&S::osd, Perf::Game::Other, QT_TR_NOOP("FPS overlay in RivaTuner")},
        {&S::dk_dyn, Perf::Game::Bananza, QT_TR_NOOP("Dynamic frame rate")},
        {&S::mkw_dyn, Perf::Game::MarioKartWorld, QT_TR_NOOP("Dynamic frame rate")},
    };
    return &entries[key];
}

// The presets a game can choose (Custom is what changing a box makes).
constexpr std::array<Perf::Preset, 4> Presets{
    Perf::Preset::Performance,
    Perf::Preset::Balanced,
    Perf::Preset::Reactive,
    Perf::Preset::Safe,
};

QString PresetName(Perf::Preset preset) {
    switch (preset) {
    case Perf::Preset::Performance:
        return QObject::tr("Performance");
    case Perf::Preset::Balanced:
        return QObject::tr("Balanced");
    case Perf::Preset::Reactive:
        return QObject::tr("Experimental");
    case Perf::Preset::Safe:
        return QObject::tr("Safe");
    case Perf::Preset::Custom:
        break;
    }
    return QObject::tr("Custom");
}

} // Anonymous namespace

ConfigureNx2PerfPerGame::ConfigureNx2PerfPerGame(u64 title_id_, QWidget* parent)
    : QWidget(parent), title_id{title_id_} {
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(
        tr("Settings here replace the Tuning tab's for this game only; Default follows it. Donkey "
           "Kong Bananza starts from the Performance preset instead of the Tuning tab's. They "
           "take effect when the game starts. If a game has already run since "
           "Dozer was opened, Dozer will ask to restart before a change applies."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    layout->addLayout(form);
    layout->addStretch();

    const Perf::Game game = Perf::GameOf(title_id);
    const auto games = UISettings::values.nx2perf_games.find(title_id);
    const bool stored = games != UISettings::values.nx2perf_games.end();

    preset = new QComboBox(this);
    preset->addItem(QString{});
    for (const Perf::Preset choice : Presets) {
        preset->addItem(PresetName(choice));
    }
    if (stored && games->second.preset) {
        for (std::size_t i = 0; i < Presets.size(); ++i) {
            if (Presets[i] == *games->second.preset) {
                preset->setCurrentIndex(static_cast<int>(i) + 1);
            }
        }
    }
    form->addRow(tr("Preset"), preset);

    for (std::size_t key = 0; key < Perf::KeyCount; ++key) {
        const Entry* entry = EntryOf(key);
        if (entry->game != Perf::Game::Other && entry->game != game) {
            continue;
        }
        auto* combo = new QComboBox(this);
        combo->addItem(QString{});
        combo->addItem(tr("On"));
        combo->addItem(tr("Off"));
        if (stored && games->second.boxes[key]) {
            combo->setCurrentIndex(*games->second.boxes[key] ? On : Off);
        }
        form->addRow(tr(entry->label), combo);
        rows.push_back({combo, key});
    }

    ShowDefaults();
    connect(preset, &QComboBox::currentIndexChanged, this, [this] { ShowDefaults(); });
}

ConfigureNx2PerfPerGame::~ConfigureNx2PerfPerGame() = default;

Perf::Switches ConfigureNx2PerfPerGame::Base() const {
    Perf::Switches base =
        Perf::GameBase(UISettings::values.nx2perf, Perf::GameOf(title_id));
    const int choice = preset->currentIndex();
    if (choice > 0) {
        base = Perf::FromPreset(Presets[static_cast<std::size_t>(choice) - 1], base);
    }
    return base;
}

void ConfigureNx2PerfPerGame::ShowDefaults() {
    const Perf::Switches base = Base();
    preset->setItemText(0, tr("Default (%1)").arg(PresetName(
                               Perf::GameBase(UISettings::values.nx2perf, Perf::GameOf(title_id))
                                   .preset)));
    for (const Row& row : rows) {
        const bool on = base.*EntryOf(row.key)->field;
        row.combo->setItemText(Default, tr("Default (%1)").arg(on ? tr("On") : tr("Off")));
    }
}

void ConfigureNx2PerfPerGame::ApplyConfiguration() {
    Perf::Overrides overrides;
    const int choice = preset->currentIndex();
    if (choice > 0) {
        overrides.preset = Presets[static_cast<std::size_t>(choice) - 1];
    }
    for (const Row& row : rows) {
        const int box = row.combo->currentIndex();
        if (box != Default) {
            overrides.boxes[row.key] = box == On;
        }
    }
    auto& games = UISettings::values.nx2perf_games;
    if (overrides.Empty()) {
        games.erase(title_id);
    } else {
        games[title_id] = overrides;
    }
}
