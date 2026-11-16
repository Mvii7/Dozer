// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_bananza.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>

#include "qt_common/config/uisettings.h"
#include "yuzu/nx2perf_session.h"

namespace Quality = Loader::Nx2DkQuality;

ConfigureBananza::ConfigureBananza(QWidget* parent) : QWidget(parent) {
    // The rows are many: they scroll
    auto* const content = new QWidget(this);
    auto* const layout = new QGridLayout(content);
    auto* const scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    auto* const outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    intro = new QLabel(this);
    intro->setWordWrap(true);
    layout->addWidget(intro, 0, 0, 1, 3);

    preset = new QComboBox(this);
    for (int i = 0; i < 4; ++i) {
        preset->addItem(QString{});
    }
    layout->addWidget(preset, 1, 0);
    preset_text = new QLabel(this);
    preset_text->setWordWrap(true);
    layout->addWidget(preset_text, 1, 1, 1, 2);
    connect(preset, &QComboBox::activated, this, [this](int index) {
        const auto chosen = static_cast<Quality::Preset>(index);
        if (chosen != Quality::Preset::Custom) {
            Show(Quality::PresetValues(chosen));
        }
        ShowPreset();
    });

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Quality::Knob& knob = Quality::Knobs[i];
        Row& row = rows[i];
        row.box = new QCheckBox(this);
        row.slider = new QSlider(Qt::Horizontal, this);
        row.slider->setRange(knob.min, knob.max);
        row.slider->setValue(knob.game);
        row.slider->setVisible(!Quality::IsToggle(knob));
        row.value = new QLabel(this);
        row.value->setMinimumWidth(
            row.value->fontMetrics().horizontalAdvance(QStringLiteral("000 frames (game)")));
        const int line = static_cast<int>(i) + 2;
        layout->addWidget(row.box, line, 0);
        layout->addWidget(row.slider, line, 1);
        layout->addWidget(row.value, line, 2);
        connect(row.box, &QCheckBox::toggled, this, [this, i] {
            ShowValue(i);
            ShowPreset();
        });
        connect(row.slider, &QSlider::valueChanged, this, [this, i] {
            ShowValue(i);
            ShowPreset();
        });
    }

    const int after = static_cast<int>(rows.size()) + 2;
    auto* const reset = new QPushButton(this);
    reset->setObjectName(QStringLiteral("reset"));
    layout->addWidget(reset, after, 0);
    connect(reset, &QPushButton::clicked, this, [this] { Show(Quality::Values{}); });
    note = new QLabel(this);
    note->setWordWrap(true);
    layout->addWidget(note, after + 1, 0, 1, 3);
    layout->setRowStretch(after + 2, 1);
    layout->setColumnStretch(1, 1);

    RetranslateUI();
    Show(UISettings::values.nx2dkq);
}

ConfigureBananza::~ConfigureBananza() = default;

void ConfigureBananza::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureBananza::RetranslateUI() {
    setAccessibleName(tr("Bananza"));
    intro->setText(tr("Donkey Kong Bananza only, experimental. A ticked row replaces a value of "
                      "the game by the slider's; an unticked row leaves the game's own. Read when "
                      "the game starts: a change needs a restart of the program."));
    note->setText(tr("Lower values are faster, except the cube map interval, where higher is "
                     "faster. The terrain's own resolution is fixed by the game's data and has "
                     "no setting. Rows marked \"for testing\" have an effect nobody has "
                     "measured yet."));
    findChild<QPushButton*>(QStringLiteral("reset"))->setText(tr("Reset to the Game's"));
    preset->setItemText(static_cast<int>(Quality::Preset::Default), tr("Default"));
    preset->setItemText(static_cast<int>(Quality::Preset::Balanced), tr("Balanced"));
    preset->setItemText(static_cast<int>(Quality::Preset::Performance), tr("Performance"));
    preset->setItemText(static_cast<int>(Quality::Preset::Custom), tr("Custom"));
    ShowPreset();

    // Indexed by the setting, so a row cannot get another's text.
    std::array<std::pair<QString, QString>, Quality::Count> texts;
    texts[Quality::ResBest] = {
        tr("Best render resolution"),
        tr("The highest resolution the game renders at before its upscaler, per axis, in "
           "percent of the output (2880x1620 docked). The game's own is 73% (2095x1179).")};
    texts[Quality::ResWorst] = {
        tr("Worst render resolution"),
        tr("The lowest resolution the game's resolution control may drop to; the game's own is "
           "50% (1440x810). Set equal to the best one for a fixed resolution.")};
    texts[Quality::GpuTarget] = {
        tr("Resolution control: GPU load target"),
        tr("The game lowers its resolution when its measured GPU load goes above this (its own "
           "is 90). Lower: it drops sooner, within the range above. The load it measures under "
           "emulation may not be meaningful: for testing.")};
    texts[Quality::FrameMinScale] = {
        tr("Frame buffer minimum scale"),
        tr("The lowest scale of the game's frame buffer scaling. The game's own is 100%, which "
           "may mean it does not use it: lower values are untested.")};
    texts[Quality::BloomScale] = {tr("Bloom buffer scale"),
                                  tr("The bloom buffers' size relative to the frame.")};
    texts[Quality::CubeMapInterval] = {
        tr("Cube map update interval"),
        tr("Frames between two captures of the reflection cube maps. Higher is faster (the "
           "only row where it is); reflections follow changes later.")};
    texts[Quality::RadialBlurScale] = {tr("Radial blur render scale"),
                                       tr("The radial blur's buffer size relative to the frame.")};
    texts[Quality::RadialBlurSamples] = {tr("Radial blur samples"),
                                         tr("Samples the radial blur takes per pixel.")};
    texts[Quality::VoxelLodDist] = {
        tr("Terrain LOD distance"),
        tr("Scales the two distances at which the destructible terrain switches to its "
           "coarser meshes. Lower: coarser terrain closer to the camera, fewer triangles.")};
    texts[Quality::VoxelLodValue] = {
        tr("Terrain LOD value"),
        tr("Scales the two values the terrain's LOD steps carry beside their distance (how "
           "coarse a step is). What higher or lower does is not known yet: for testing.")};
    texts[Quality::CameraFar] = {
        tr("Camera far distance"),
        tr("Scales the distance beyond which the game draws nothing. A big cut (about 10%) "
           "doubled the frame rate in a busy area in testing, but scenery stops being drawn "
           "fairly close. From the Selevet fork's far clip scale.")};
    texts[Quality::ColorDecalMax] = {
        tr("Colour decal limit"),
        tr("The most colour decals (paint marks) the game keeps; its own is 660.")};
    texts[Quality::MeshSimplify] = {
        tr("Terrain mesh simplification"),
        tr("How the game simplifies the destructible terrain's meshes: 1 Normal (the game's "
           "usual), 2 High, 3 SuperHigh, 4 NoSimplify, 5 Low. Which of them simplifies the "
           "most is not known: the names could mean either way, so try them. Applies to every "
           "terrain field, including ones the game keeps unsimplified.")};
    texts[Quality::GgxFilterOff] = {
        tr("Reflection filtering off"),
        tr("Skips the GGX filtering of the reflection cube maps. Reflections get sharper or "
           "wrong; may cost less. For testing.")};
    texts[Quality::CubeMapOcclusionOff] = {
        tr("Cube map occlusion check off"),
        tr("Skips the occlusion check the reflection cube maps make. For testing.")};
    texts[Quality::OcclusionDepth] = {
        tr("Occlusion query depth"),
        tr("Scales the depth threshold of the game's occlusion queries (its own is 50). What "
           "higher or lower does is not known yet: for testing.")};
    texts[Quality::OcclusionRadius] = {
        tr("Occlusion query radius"),
        tr("Scales the radius threshold of the game's occlusion queries (its own is 50). For "
           "testing.")};
    texts[Quality::OcclusionPolygon] = {
        tr("Occlusion query polygons"),
        tr("Scales the polygon count threshold of the game's occlusion queries (its own is "
           "1000). For testing.")};
    texts[Quality::ShadowMaskOff] = {
        tr("Shadow masks off"),
        tr("Turns off the game's shadow masks (the volume shadows). Shadows or darkening may go "
           "missing. For testing.")};
    texts[Quality::DecalOff] = {
        tr("Decals off"),
        tr("Skips creating the game's decal director (decals are not drawn). May break something that expects it. For testing.")};
    texts[Quality::DepthBloomOff] = {
        tr("Depth bloom off"),
        tr("Skips the depth bloom. For testing.")};
    texts[Quality::BloomExtensionOff] = {
        tr("Bloom extension off"),
        tr("Skips the bloom extension. For testing.")};
    texts[Quality::RadialBlurOff] = {
        tr("Radial blur off"),
        tr("Skips the radial blur drawer. For testing.")};
    texts[Quality::ThunderOff] = {
        tr("Thunder off"),
        tr("Skips the thunder renderer. For testing.")};
    texts[Quality::WetModelOff] = {
        tr("Wet models off"),
        tr("Skips the wet model control (no wet look on models). For testing.")};
    texts[Quality::CloudShapeOff] = {
        tr("Cloud shapes off"),
        tr("Skips the cloud shapes. For testing.")};
    texts[Quality::DofIndirectOff] = {
        tr("Depth of field off"),
        tr("Skips the indirect depth of field control. For testing.")};
    texts[Quality::AglSkyOff] = {
        tr("AGL sky off"),
        tr("Skips the AGL sky. The sky may go missing. Something that expects it may break. For testing.")};
    texts[Quality::CubeMapOff] = {
        tr("Cube maps off"),
        tr("Skips the reflection cube maps altogether. Something that expects it may break. For testing.")};
    texts[Quality::FogOff] = {
        tr("Fog off"),
        tr("Skips the fog. Something that expects it may break. For testing.")};
    texts[Quality::LightMapOff] = {
        tr("Light maps off"),
        tr("Skips the light map director. Something that expects it may break. For testing.")};
    texts[Quality::ModelForegroundOff] = {
        tr("Foreground models off"),
        tr("Skips the foreground model drawer. Something that expects it may break. For testing.")};
    texts[Quality::PrePassLightOff] = {
        tr("Pre-pass light off"),
        tr("Skips the pre-pass light. Something that expects it may break. For testing.")};
    texts[Quality::SkyModelOff] = {
        tr("Sky model off"),
        tr("Skips the sky model. Something that expects it may break. For testing.")};
    texts[Quality::FoliageCap] = {
        tr("Foliage models at once"),
        tr("Scales how many foliage models (grass, weeds, plants) the game draws and keeps alive "
           "at once (its own caps are 25000 and 50000). Done in the game's settings files as "
           "they are read, so it needs a restart.")};
    texts[Quality::FoliageFrustumOff] = {
        tr("Foliage view margin off"),
        tr("Sets the extra margin the game adds around the view for foliage (50%) to 0: less "
           "foliage outside the picture, but it may pop in at the edges when turning.")};
    texts[Quality::FoliageDensity] = {
        tr("Foliage placement density"),
        tr("Scales the chance and the rate of the foliage the game places while you play. "
           "Foliage already placed in the stage's data is not changed (use the cap for that).")};
    texts[Quality::FoliageFarClip] = {
        tr("Foliage draw distance"),
        tr("Scales how far each foliage model (grass, flowers, plants, rocks) is drawn, from its "
           "own distance in the game's data (100 to 300 units). Lower: foliage ends closer to "
           "you but stays near you. Needs a restart.")};
    texts[Quality::FoliageLod] = {
        tr("Foliage LOD bias"),
        tr("Scales when each foliage model switches to a simpler mesh. Higher: simpler meshes "
           "sooner, so foliage looks coarser at a distance and costs less.")};
    texts[Quality::EffectEmitCap] = {
        tr("Effects started per frame"),
        tr("Scales how many effects (particles, dust, debris) the game may start in one frame "
           "(its own cap is 156). Changed in the game's settings files as they are read.")};
    texts[Quality::EffectParticleCap] = {
        tr("Particles per emitter"),
        tr("Scales the most particles one emitter may have (its own cap is 512).")};
    texts[Quality::EffectAliveCap] = {
        tr("Effects alive per rule"),
        tr("Scales how many effects the game's emission rules keep alive at once (foliage effects "
           "and debris parts: 64 each).")};
    texts[Quality::EffectMaterialCap] = {
        tr("Terrain material effects"),
        tr("Scales how many terrain material effects (dust, smoke, snow) are active at once, in "
           "the effect settings and in each material's own limit.")};
    texts[Quality::VoxelCellSize] = {
        tr("Terrain cell size"),
        tr("Scales the terrain's cell size (a bigger cell: fewer cells). Not in the presets: the "
           "saved terrain may be laid out for the game's own size, so this may corrupt the "
           "terrain. For testing.")};
    texts[Quality::VoxelBorderMeshOff] = {
        tr("Terrain border mesh off"),
        tr("Builds no border mesh between neighbouring terrain chunks. Cracks may show. For "
           "testing.")};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].box->setText(texts[i].first);
        QString tip = texts[i].second;
        const auto env = Quality::Knobs[i].env;
        const bool outside = Nx2PerfSession::SetOutside(env);
        if (outside) {
            tip += QLatin1Char(' ') +
                   tr("Set outside the program (%1), so that value is used whatever this row says.")
                       .arg(QString::fromUtf8(env.data(), static_cast<qsizetype>(env.size())));
        }
        rows[i].box->setToolTip(tip);
        rows[i].slider->setToolTip(tip);
        rows[i].box->setEnabled(!outside);
        ShowValue(i);
    }
}

void ConfigureBananza::Show(const Quality::Values& values) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const QSignalBlocker box_blocker{rows[i].box};
        const QSignalBlocker slider_blocker{rows[i].slider};
        rows[i].box->setChecked(values[i] != 0);
        rows[i].slider->setValue(values[i] != 0 ? values[i] : Quality::Knobs[i].game);
        ShowValue(i);
    }
    ShowPreset();
}

void ConfigureBananza::ShowValue(std::size_t index) {
    const Row& row = rows[index];
    const bool on = row.box->isChecked();
    row.slider->setEnabled(on && row.box->isEnabled());
    if (Quality::IsToggle(Quality::Knobs[index])) {
        row.value->setText(on ? tr("off") : tr("on (game)"));
        return;
    }
    const int value = on ? row.slider->value() : Quality::Knobs[index].game;
    QString text;
    switch (static_cast<Quality::Id>(index)) {
    case Quality::CubeMapInterval:
        text = tr("%n frame(s)", nullptr, value);
        break;
    case Quality::MeshSimplify: {
        const QStringList names{tr("Normal"), tr("High"), tr("SuperHigh"), tr("NoSimplify"),
                                tr("Low")};
        text = names.value(value - 1);
        break;
    }
    case Quality::RadialBlurSamples:
    case Quality::ColorDecalMax:
        text = QString::number(value);
        break;
    default:
        text = tr("%1%").arg(value);
        break;
    }
    row.value->setText(on ? text : tr("%1 (game)").arg(text));
}

Quality::Values ConfigureBananza::Current() const {
    Quality::Values values{};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        values[i] = rows[i].box->isChecked() ? rows[i].slider->value() : 0;
    }
    return values;
}

void ConfigureBananza::ShowPreset() {
    const Quality::Preset now = Quality::PresetOf(Current());
    {
        const QSignalBlocker blocker{preset};
        preset->setCurrentIndex(static_cast<int>(now));
    }
    switch (now) {
    case Quality::Preset::Default:
        preset_text->setText(tr("The game's own values: nothing is changed."));
        break;
    case Quality::Preset::Balanced:
        preset_text->setText(tr("A moderate cut: render resolution 65% at best and 45% at "
                                "worst, smaller bloom and blur, camera far distance 80%."));
        break;
    case Quality::Preset::Performance:
        preset_text->setText(tr("A hard cut: render resolution 50% at best and 35% at worst, "
                                "camera far distance 50%. The picture gets visibly softer and "
                                "distant scenery disappears."));
        break;
    case Quality::Preset::Custom:
        preset_text->setText(tr("Your own choice of the rows below."));
        break;
    }
}

void ConfigureBananza::ApplyConfiguration() {
    UISettings::values.nx2dkq = Current();
}
