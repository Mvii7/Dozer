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
    texts[Quality::CameraFar] = {
        tr("Camera far distance"),
        tr("Scales the distance beyond which the game draws nothing. A big cut (about 10%) "
           "doubled the frame rate in a busy area in testing, but scenery stops being drawn "
           "fairly close. From the Selevet fork's far clip scale.")};
    texts[Quality::ColorDecalMax] = {
        tr("Colour decal limit"),
        tr("The most colour decals (paint marks) the game keeps; its own is 660.")};
    texts[Quality::ActorDrawDistance] = {
        tr("Actor draw distance"),
        tr("Scales how far away actors (objects, enemies, scenery pieces: over 2000 of them use "
           "it) stop being drawn, from the game's own distances (50 to 1200 by actor, 200 by "
           "default). Lower: they are clipped closer to you. Actors marked never to clip stay.")};
    texts[Quality::NonClipDistance] = {
        tr("Never-clipped object distance"),
        tr("Gives the objects the game never clips a draw distance, in units (the actors' own "
           "go from 50 to 1200): the ones of no clip level (about 220 kinds: big level models, "
           "landmarks, backdrop effects) and the ones whose clipping is switched off (about 670 "
           "kinds: distant views, level models, cutscene props). The actor draw distance leaves "
           "both alone. Lower: big scenery disappears at a distance, cutscenes seen from afar "
           "may miss props. For testing.")};
    texts[Quality::ShadowCascades] = {
        tr("Sun shadow cascades"),
        tr("The most cascades the sun's shadow is drawn in (the game's: up to 4). Each cascade "
           "draws every shadow caster again, so fewer cascades means fewer draws; the remaining "
           "ones are stretched over the same distance, so shadows get blurrier.")};
    texts[Quality::ShadowDistance] = {
        tr("Sun shadow distance"),
        tr("Scales how far the sun's shadow reaches (the end of its last cascade). Lower: "
           "objects further away cast no shadow and are not drawn into it, so fewer draws.")};
    texts[Quality::GgxFilterOff] = {
        tr("Reflection filtering off"),
        tr("Skips the GGX filtering of the reflection cube maps. Reflections get sharper or "
           "wrong; may cost less. For testing.")};
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
    texts[Quality::CloudShapeOff] = {
        tr("Cloud shapes off"),
        tr("Skips the cloud shapes. For testing.")};
    texts[Quality::DofIndirectOff] = {
        tr("Depth of field off"),
        tr("Skips the indirect depth of field control. For testing.")};
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
    texts[Quality::FoliageCap] = {
        tr("Foliage models at once"),
        tr("Scales how many foliage models (grass, weeds, plants) the game draws and keeps alive "
           "at once: its global caps (25000 and 50000) and each stage's own draw buffer (1200 "
           "to 6000, the limit a stage actually draws with). Done in the game's settings files "
           "as they are read, so it needs a restart.")};
    texts[Quality::FoliageFrustumOff] = {
        tr("Foliage view margin off"),
        tr("Sets the extra margin the game adds around the view for foliage (50%) to 0: less "
           "foliage outside the picture, but it may pop in at the edges when turning.")};
    texts[Quality::FoliageDensity] = {
        tr("Foliage placement density"),
        tr("Thins the grass and plants each stage places ahead of time, in patches: the rest "
           "keeps its look, with bare ground between clumps instead of an even carpet. Also "
           "scales the chance and the rate of the foliage the game places while you play. "
           "Needs a restart.")};
    texts[Quality::FoliageFarClip] = {
        tr("Foliage draw distance"),
        tr("Scales how far each foliage model (grass, flowers, plants, rocks) is drawn, from its "
           "own distance in the game's data (80 to 700 units), or the game's default of 100 for "
           "the models without one (about a third of the foliage). Lower: foliage ends closer to "
           "you but stays near you; 1% is about no foliage. "
           "Needs a restart.")};
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
    texts[Quality::EffectEmitOff] = {
        tr("Effect starts off"),
        tr("Lets the game start one effect (particles, dust, debris) per frame at most. Needs a "
           "restart. Something that expects an effect may break.")};
    texts[Quality::EffectParticleOff] = {
        tr("Emitter particles off"),
        tr("Lets one emitter have one particle at most. Needs a restart.")};
    texts[Quality::EffectAliveOff] = {
        tr("Alive effects off"),
        tr("Lets each emission rule keep one effect alive at most. Needs a restart.")};
    texts[Quality::EffectMaterialOff] = {
        tr("Terrain material effects off"),
        tr("Lets one terrain material effect (dust, smoke, snow) be active at most. Needs a "
           "restart.")};
    texts[Quality::SceneClipOn] = {
        tr("Object clipping in every level"),
        tr("Turns the objects' far clipping on in the levels that have it off (135 of them, "
           "mostly special and challenge stages, where nothing is ever clipped) and removes "
           "the extra +2000 two levels add to every distance, so the actor draw distance rows "
           "work there too. On its own the game's distances apply. Needs a restart.")};
    texts[Quality::DebrisOff] = {
        tr("Debris off"),
        tr("Gives every terrain material the setting some of them (trees, iron mesh) already "
           "have: no fragments when it breaks. Changed in the game's code as it loads. It may "
           "also change how terrain breaks apart or which chunks can be picked up. For testing.")};
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
    case Quality::NonClipDistance:
        text = on ? tr("%1 units").arg(value * 10) : tr("never");
        break;
    case Quality::RadialBlurSamples:
    case Quality::ShadowCascades:
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
