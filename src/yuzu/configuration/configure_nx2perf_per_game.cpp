// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_nx2perf_per_game.h"

#include <optional>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStyle>
#include <QVBoxLayout>

#include "configuration/shared_widget.h"
#include "core/nx2_perf_settings.h"
#include "qt_common/config/uisettings.h"

namespace Perf = Core::Nx2PerfSettings;

namespace {

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

// The cross next to a changed setting: the other tabs' own restore button (hidden until shown).
QPushButton* CreateResetButton(QWidget* parent) {
    QPushButton* button = ConfigurationShared::Widget::CreateRestoreGlobalButton(true, parent);
    button->setToolTip(QObject::tr("Reset to the global setting"));
    return button;
}

void ShowReset(QPushButton* button, bool shown) {
    button->setEnabled(shown);
    button->setVisible(shown);
}

} // Anonymous namespace

ConfigureNx2PerfPerGame::ConfigureNx2PerfPerGame(u64 title_id_, QWidget* parent)
    : QWidget(parent), title_id{title_id_}, game{Perf::GameOf(title_id_)} {
    const auto stored = UISettings::values.nx2perf_games.find(title_id);
    if (stored != UISettings::values.nx2perf_games.end()) {
        own = stored->second;
    }

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(
        tr("These settings are the Tuning tab's, as this game runs them. A cross next to one resets "
           "it to the Tuning tab's. Donkey Kong Bananza starts with the Performance preset instead "
           "of the Tuning tab's. They take effect when the game starts. If a game has already run "
           "since Dozer was opened, Dozer will ask to restart before a change applies."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    layout->addLayout(form);
    layout->addStretch();

    // Preferred, like the other tabs' boxes: the box grows to the cross's height.
    preset = new QComboBox(this);
    preset->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    for (const Perf::Preset choice : Perf::PresetOrder) {
        preset->addItem(PresetName(choice));
    }
    // Custom is what changing a box of the Tuning tab makes; it is only shown.
    if (auto* model = qobject_cast<QStandardItemModel*>(preset->model())) {
        model->item(Perf::IndexOf(Perf::Preset::Custom))->setEnabled(false);
    }
    preset_reset = CreateResetButton(this);
    auto* preset_row = new QHBoxLayout;
    preset_row->addWidget(preset, 1);
    preset_row->addWidget(preset_reset);
    form->addRow(tr("Preset"), preset_row);
    connect(preset, &QComboBox::activated, this, [this](int index) {
        if (index >= 0 && static_cast<std::size_t>(index) < Perf::PresetOrder.size()) {
            ChoosePreset(Perf::PresetOrder[static_cast<std::size_t>(index)]);
        }
    });
    connect(preset_reset, &QPushButton::clicked, this, [this] {
        own.preset.reset();
        own.global_preset = Perf::GamePreset(game).has_value();
        Refresh();
    });

    for (std::size_t key = 0; key < Perf::KeyCount; ++key) {
        const Entry* entry = EntryOf(key);
        if (entry->game != Perf::Game::Other && entry->game != game) {
            continue;
        }
        auto* box = new QCheckBox(this);
        auto* reset = CreateResetButton(this);
        auto* row_layout = new QHBoxLayout;
        row_layout->addWidget(box, 1);
        row_layout->addWidget(reset);
        form->addRow(tr(entry->label), row_layout);
        rows.push_back({box, reset, key});
        const std::size_t index = rows.size() - 1;
        connect(box, &QCheckBox::toggled, this,
                [this, index](bool checked) { ChooseBox(rows[index], checked); });
        connect(reset, &QPushButton::clicked, this, [this, key] {
            own.boxes[key].reset();
            Refresh();
        });
    }

    Refresh();
}

ConfigureNx2PerfPerGame::~ConfigureNx2PerfPerGame() = default;

void ConfigureNx2PerfPerGame::Refresh() {
    const Perf::Switches& global = UISettings::values.nx2perf;
    const Perf::Switches now = Perf::ForGame(global, game, own);
    {
        const QSignalBlocker blocker{preset};
        preset->setCurrentIndex(Perf::IndexOf(now.preset));
    }
    ShowReset(preset_reset, now.preset != global.preset);
    for (const Row& row : rows) {
        {
            const QSignalBlocker blocker{row.box};
            row.box->setChecked(now.*EntryOf(row.key)->field);
        }
        // The game's own boxes have no global setting to go back to.
        ShowReset(row.reset, row.key < Perf::Keys.size() && own.boxes[row.key].has_value());
    }
}

void ConfigureNx2PerfPerGame::ChoosePreset(Perf::Preset chosen) {
    const Perf::Preset global = UISettings::values.nx2perf.preset;
    own.preset.reset();
    own.global_preset = false;
    // Only what differs from where the game starts is kept.
    if (chosen != Perf::PresetFor(global, game, own)) {
        if (chosen == global) {
            own.global_preset = true;
        } else {
            own.preset = chosen;
        }
    }
    Refresh();
}

void ConfigureNx2PerfPerGame::ChooseBox(const Row& row, bool checked) {
    Perf::Overrides without = own;
    without.boxes[row.key].reset();
    const bool base =
        Perf::ForGame(UISettings::values.nx2perf, game, without).*EntryOf(row.key)->field;
    own.boxes[row.key] = checked == base ? std::nullopt : std::optional<bool>{checked};
    Refresh();
}

void ConfigureNx2PerfPerGame::ApplyConfiguration() {
    auto& games = UISettings::values.nx2perf_games;
    if (own.Empty()) {
        games.erase(title_id);
    } else {
        games[title_id] = own;
    }
}
