// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/loader/nx2_dk_dynamic_fps.h"
#include "core/loader/nx2_mkw_dynamic_fps.h"

// Anka's settings in the Qt program (anka.exe): the EDEN_NX2_* switches of the launchers
// (dist/dozer/start-*.bat) as on / off settings kept in the [Anka] group of qt-config.ini. The
// program sets the variables in its own environment before a game boots (yuzu/nx2perf_session.h);
// anka-cli and the launchers don't read these settings.
//
// A game runs with the global settings, then its own overrides over them (ForGame). Nothing else
// stands between the two: there is no stored preset. Safe and Experimental are only a way to fill
// in the global boxes at once (Apply).
//
// Every switch is read once per process, most of them into a function's static the first time
// a game reaches it (EDEN_NX2_BACK_STAGES, ACQUIRE_PEEK, PRESENT_INTERVAL_WORD, MODE_PACE,
// PACE_SPIN_US, DK_DYN, OSD, READ_TRAPS); JIT_SAME, FRONT_MACRO_LEAN and FG_TIMED are read again
// at each boot.
// So the variables are set once, at the first boot, and a later boot that would need others (a
// changed setting, or Bananza after another game and back) restarts the program for that game.
namespace Core::Nx2PerfSettings {

/// What the Tuning tab's "Apply" fills the global settings in with. Nothing of it is stored.
enum class Setup : u8 {
    Defaults,     ///< start-performance.bat: all on, read traps on, Reactive Flushing off
    Experimental, ///< start-reactive-experimental.bat: read traps off, Reactive Flushing on, RX_RT_FIT=3
    Safe,         ///< start-safe.bat: all off, and the shortcuts Dozer keeps on set to 0
};

/// The numbers the preset of an older qt-config.ini was stored as, read once to carry it over.
namespace LegacyPreset {
inline constexpr int Quality = 0;
inline constexpr int Reactive = 1;
inline constexpr int Safe = 2;
inline constexpr int Custom = 3;
inline constexpr int Performance = 4;
} // namespace LegacyPreset

/// Bananza's shadow quality steps: the sun shadow's most cascades.
inline constexpr int ShadowLow = 1;
inline constexpr int ShadowMedium = 2;
inline constexpr int ShadowVeryHigh = 4;

/// Bananza's foliage steps: Off (EDEN_NX2_DK_FOLIAGE_OFF=1), Low and Medium (the foliage kept,
/// EDEN_NX2_DK_FOLIAGE_DENSITY, the thorns kept, EDEN_NX2_DK_FOLIAGE_THORN_DENSITY, and how far it
/// is drawn, EDEN_NX2_DK_FOLIAGE_DISTANCE), High the game's own.
inline constexpr int FoliageOff = 1;
inline constexpr int FoliageLow = 2;    ///< 10% kept (thorns 50%), drawn up to 200 units
inline constexpr int FoliageMedium = 3; ///< 50% kept (thorns 75%), drawn up to 400 units
inline constexpr int FoliageHigh = 4;

/// Bananza's reflection update steps: how often the reflection cube map is captured again.
inline constexpr int ReflectionsVeryLow = 1; ///< every 240 frames, the most the patch takes
inline constexpr int ReflectionsLow = 2;    ///< every 32 frames
inline constexpr int ReflectionsMedium = 3; ///< every 16 frames
inline constexpr int ReflectionsHigh = 4;   ///< every 8 frames, the game's own

struct Switches {
    bool read_traps = true;        ///< on leaves EDEN_NX2_READ_TRAPS unset; off sets it to 0
    bool split_gpu_thread = true;  ///< EDEN_NX2_BACK_STAGES=1 + EDEN_NX2_BACK_CUTS=a
    bool acquire_peek = true;      ///< EDEN_NX2_ACQUIRE_PEEK=1
    bool jit_same = true;          ///< EDEN_NX2_JIT_SAME=1
    bool front_macro_lean = true;  ///< EDEN_NX2_FRONT_MACRO_LEAN=1
    bool fg_timed = true;          ///< EDEN_NX2_FG_TIMED=1
    bool osd = false;              ///< EDEN_NX2_OSD=1, off until ticked
    bool safe_off = false;         ///< the per-draw shortcuts Dozer keeps on set to 0 (SafeOff)
    bool rt_fit = false;           ///< EDEN_NX2_RX_RT_FIT=3, for Reactive Flushing
    // Game-specific: Apply leaves these as they are.
    bool dk_dyn = true;            ///< EDEN_NX2_DK_DYN=1, Bananza only
    bool mkw_dyn = true;           ///< EDEN_NX2_MKW_DYN=1, Mario Kart World only
    /// Bananza's foliage, reflection updates and shadow quality. Not stored: ForGame sets them
    /// from BananzaLevels, and a game's own choices go over them.
    int dk_foliage = FoliageLow;
    int dk_reflections = ReflectionsVeryLow;
    int dk_shadow = ShadowMedium;

    bool operator==(const Switches&) const = default;
};

/// What the performance launcher sets: everything on, read traps too, but not the RTSS overlay.
constexpr Switches Performance() {
    return Switches{};
}

/// The settings of a first start (no [Anka] group yet): the performance launcher's.
constexpr Switches FirstStart() {
    return Performance();
}

/// start-safe.bat's setup: none of the switches, and the per-draw shortcuts off.
constexpr Switches AllOff() {
    return Switches{
        .read_traps = false,
        .split_gpu_thread = false,
        .acquire_peek = false,
        .jit_same = false,
        .front_macro_lean = false,
        .fg_timed = false,
        .osd = false,
        .safe_off = true,
        .dk_foliage = FoliageHigh,
        .dk_reflections = ReflectionsHigh,
        .dk_shadow = ShadowVeryHigh,
    };
}

/// The boxes `setup` fills in, over `current`: the dynamic frame rate boxes are always `current`'s.
/// Experimental is Defaults with read traps off and RX_RT_FIT=3.
constexpr Switches Apply(Setup setup, const Switches& current) {
    Switches switches = setup == Setup::Safe ? AllOff() : Performance();
    if (setup == Setup::Experimental) {
        switches.read_traps = false;
        switches.rt_fit = true;
    }
    switches.dk_dyn = current.dk_dyn;
    switches.mkw_dyn = current.mkw_dyn;
    return switches;
}

/// Bananza's three levels as a game makes them from the global settings: low foliage, very low
/// reflection updates and medium shadows, or all the game's own when the shortcuts are off (Safe).
struct BananzaLevels {
    int foliage;
    int reflections;
    int shadow;

    bool operator==(const BananzaLevels&) const = default;
};

inline constexpr BananzaLevels BananzaPerformance{FoliageLow, ReflectionsVeryLow, ShadowMedium};
inline constexpr BananzaLevels BananzaQuality{FoliageHigh, ReflectionsHigh, ShadowVeryHigh};

constexpr BananzaLevels BananzaDefault(const Switches& global) {
    return global.safe_off ? BananzaQuality : BananzaPerformance;
}

/// The keys of the [Anka] group, one per setting.
struct Key {
    std::string_view name;
    bool Switches::*field;
};
inline constexpr std::array<Key, 9> Keys{{
    {"read_traps", &Switches::read_traps},
    {"split_gpu_thread", &Switches::split_gpu_thread},
    {"acquire_peek", &Switches::acquire_peek},
    {"jit_same", &Switches::jit_same},
    {"front_macro_lean", &Switches::front_macro_lean},
    {"fg_timed", &Switches::fg_timed},
    {"osd", &Switches::osd},
    {"safe_off", &Switches::safe_off},
    {"rt_fit", &Switches::rt_fit},
}};

/// The game-specific keys: stored like the others, but no setup sets or clears them.
inline constexpr std::array<Key, 2> GameKeys{{
    {"bananza_dynamic_fps", &Switches::dk_dyn},
    {"mariokart_dynamic_fps", &Switches::mkw_dyn},
}};

inline constexpr std::size_t KeyCount = Keys.size() + GameKeys.size();

/// The key at `index`: Keys, then GameKeys.
constexpr const Key& KeyAt(std::size_t index) {
    return index < Keys.size() ? Keys[index] : GameKeys[index - Keys.size()];
}

/// One game's own choices over the global settings: a box per KeyAt() slot, each or none (it
/// follows the global one), and Bananza's three levels.
struct Overrides {
    std::array<std::optional<bool>, KeyCount> boxes{};
    std::optional<int> dk_shadow; ///< Bananza's shadow quality, ShadowLow to ShadowVeryHigh
    std::optional<int> dk_reflections; ///< ReflectionsVeryLow to ReflectionsHigh
    std::optional<int> dk_foliage;     ///< FoliageOff to FoliageHigh

    constexpr bool Empty() const {
        if (dk_shadow || dk_reflections || dk_foliage) {
            return false;
        }
        for (const auto& box : boxes) {
            if (box) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const Overrides&) const = default;
};

/// `switches` with the overrides over them.
constexpr Switches WithOverrides(Switches switches, const Overrides& overrides) {
    for (std::size_t i = 0; i < KeyCount; ++i) {
        if (overrides.boxes[i]) {
            switches.*KeyAt(i).field = *overrides.boxes[i];
        }
    }
    if (overrides.dk_shadow) {
        switches.dk_shadow = *overrides.dk_shadow;
    }
    if (overrides.dk_reflections) {
        switches.dk_reflections = *overrides.dk_reflections;
    }
    if (overrides.dk_foliage) {
        switches.dk_foliage = *overrides.dk_foliage;
    }
    return switches;
}

/// The settings a game runs with: the global ones with Bananza's levels as a game makes them, then
/// the game's own overrides.
constexpr Switches ForGame(Switches global, const Overrides& own) {
    const BananzaLevels levels = BananzaDefault(global);
    global.dk_foliage = levels.foliage;
    global.dk_reflections = levels.reflections;
    global.dk_shadow = levels.shadow;
    return WithOverrides(global, own);
}

/// Reads a game's overrides through `read(key)`: -1 for no override, 0 off, 1 on;
/// "bananza_shadow_quality" a shadow quality step or -1, "bananza_reflection_updates" a
/// reflection update step or -1 (else the box it replaces, "bananza_reflections_off": on is
/// Very low, off High), "bananza_foliage" a foliage step or -1 (else the box it replaces,
/// "bananza_foliage_off": on is Off, off High).
template <typename Read>
Overrides LoadOverrides(Read&& read) {
    Overrides overrides;
    for (std::size_t i = 0; i < KeyCount; ++i) {
        const int value = read(KeyAt(i).name);
        if (value == 0 || value == 1) {
            overrides.boxes[i] = value == 1;
        }
    }
    if (const int shadow = read("bananza_shadow_quality");
        shadow >= ShadowLow && shadow <= ShadowVeryHigh) {
        overrides.dk_shadow = shadow;
    }
    if (const int updates = read("bananza_reflection_updates");
        updates >= ReflectionsVeryLow && updates <= ReflectionsHigh) {
        overrides.dk_reflections = updates;
    } else if (const int off = read("bananza_reflections_off"); off == 0 || off == 1) {
        overrides.dk_reflections = off == 1 ? ReflectionsVeryLow : ReflectionsHigh;
    }
    if (const int foliage = read("bananza_foliage");
        foliage >= FoliageOff && foliage <= FoliageHigh) {
        overrides.dk_foliage = foliage;
    } else if (const int off = read("bananza_foliage_off"); off == 0 || off == 1) {
        overrides.dk_foliage = off == 1 ? FoliageOff : FoliageHigh;
    }
    return overrides;
}

/// Writes a game's overrides through `write(key, value)`, with the values `LoadOverrides` reads.
template <typename Write>
void SaveOverrides(const Overrides& overrides, Write&& write) {
    for (std::size_t i = 0; i < KeyCount; ++i) {
        const auto& box = overrides.boxes[i];
        write(KeyAt(i).name, box ? static_cast<int>(*box) : -1);
    }
    write("bananza_shadow_quality", overrides.dk_shadow.value_or(-1));
    write("bananza_reflection_updates", overrides.dk_reflections.value_or(-1));
    write("bananza_reflections_off", -1);
    write("bananza_foliage", overrides.dk_foliage.value_or(-1));
    write("bananza_foliage_off", -1);
}
/// The Anka tab's frame generation choice. It writes the Graphics tab's settings (frame_gen,
/// frame_gen_type, frame_gen_multiplier, frame_gen_target_rate); they are not EDEN_NX2_* switches.
enum class FrameGen : u8 {
    Off = 0,     ///< frame generation off
    Fixed2x = 1, ///< FSR3, one generated frame per real frame
    Dynamic = 2, ///< FSR3 aiming at DYNAMIC_TARGET_RATE (the pacer caps it at the refresh rate)
    Other = 3,   ///< the Graphics tab holds another setup: shown, never written
};

/// Dynamic's target frame rate (frame_gen_target_rate).
constexpr u32 DYNAMIC_TARGET_RATE = 120;

/// The Graphics tab's frame generation settings, as far as the choice goes.
struct FrameGenSettings {
    bool on = false;
    bool fsr3 = true; ///< frame_gen_type is FSR3
    u32 multiplier = 2;
    u32 target_rate = 0;

    constexpr bool operator==(const FrameGenSettings&) const = default;
};

/// The choice the settings amount to.
constexpr FrameGen FrameGenFrom(const FrameGenSettings& settings) {
    if (!settings.on) {
        return FrameGen::Off;
    }
    if (settings.fsr3 && settings.target_rate == 0 && settings.multiplier == 2) {
        return FrameGen::Fixed2x;
    }
    if (settings.fsr3 && settings.target_rate == DYNAMIC_TARGET_RATE) {
        return FrameGen::Dynamic;
    }
    return FrameGen::Other;
}

/// The settings a choice writes; what it leaves alone comes from `current` (Off only turns frame
/// generation off, Dynamic keeps the multiplier: the target rate decides, Other changes nothing).
constexpr FrameGenSettings FrameGenApply(FrameGen choice, FrameGenSettings current) {
    switch (choice) {
    case FrameGen::Off:
        current.on = false;
        break;
    case FrameGen::Fixed2x:
        current = {.on = true, .fsr3 = true, .multiplier = 2, .target_rate = 0};
        break;
    case FrameGen::Dynamic:
        current.on = true;
        current.fsr3 = true;
        current.target_rate = DYNAMIC_TARGET_RATE;
        break;
    case FrameGen::Other:
        break;
    }
    return current;
}

/// The frame generation a setup asks for: Safe turns it off, the others leave it.
constexpr std::optional<FrameGen> FrameGenFor(Setup setup) {
    if (setup == Setup::Safe) {
        return FrameGen::Off;
    }
    return std::nullopt;
}

/// The barrier feedback loops setting (Graphics > Advanced) a setup asks for: on for Experimental,
/// which needs it (start-reactive-experimental.bat sets both); none for the others.
constexpr std::optional<bool> BarrierFeedbackLoops(Setup setup) {
    if (setup == Setup::Experimental) {
        return true;
    }
    return std::nullopt;
}

/// The Reactive Flushing setting (Graphics > Advanced) a setup asks for; none for Safe (the
/// launchers' safe one leaves the .ini as it is).
constexpr std::optional<bool> ReactiveFlushing(Setup setup) {
    switch (setup) {
    case Setup::Defaults:
        return false;
    case Setup::Experimental:
        return true;
    case Setup::Safe:
        break;
    }
    return std::nullopt;
}

/// What qt-config.ini's [Anka] group is written as: 0 (none) is a file from before the preset went
/// away, whose "preset" is read once (Load) and carried over as boxes.
inline constexpr int FormatVersion = 1;

/// Reads every setting through `read(key, default)` and the version and an older preset through
/// `read_int(key, default)`; missing keys give the first start's (Performance). A file from before
/// the preset went away gets the boxes its preset set: Safe the shortcuts off, Experimental
/// RX_RT_FIT=3.
template <typename Read, typename ReadInt>
Switches Load(Read&& read, ReadInt&& read_int) {
    const bool legacy = read_int("version", 0) < FormatVersion;
    const int preset = legacy ? read_int("preset", LegacyPreset::Performance) : -1;
    // read_traps was added after the other boxes: a file without the key keeps the preset's own
    // value (read traps: Performance and Quality on, Experimental and Safe off) instead of being
    // read as another setup. Performance had read traps off before: a stored Performance takes
    // the preset's.
    const bool preset_traps = preset != LegacyPreset::Reactive && preset != LegacyPreset::Safe;
    Switches switches{};
    for (const Key& key : Keys) {
        const bool by_preset = key.name == "read_traps";
        const bool fallback = by_preset && legacy ? preset_traps : Performance().*key.field;
        switches.*key.field = read(key.name, fallback);
    }
    for (const Key& key : GameKeys) {
        switches.*key.field = read(key.name, Performance().*key.field);
    }
    if (legacy) {
        if (preset == LegacyPreset::Performance) {
            switches.read_traps = true;
        }
        // The boxes were stored with the preset; what it set besides them was not
        switches.safe_off = switches.safe_off || preset == LegacyPreset::Safe;
        switches.rt_fit = switches.rt_fit || preset == LegacyPreset::Reactive;
    }
    return switches;
}

/// Writes every setting through `write(key, value, default)` and the version through
/// `write_int(key, value, default)`.
template <typename Write, typename WriteInt>
void Save(const Switches& switches, Write&& write, WriteInt&& write_int) {
    for (const Key& key : Keys) {
        write(key.name, switches.*key.field, Performance().*key.field);
    }
    for (const Key& key : GameKeys) {
        write(key.name, switches.*key.field, Performance().*key.field);
    }
    write_int("version", FormatVersion, 0);
}

/// The games with settings of their own. Each takes its title only: the program id its patch
/// checks (Loader::Nx2DkDynamicFps::ApplyIfEnabled, Loader::Nx2MkwDynamicFps::ApplyIfEnabled).
enum class Game : u8 {
    Other,
    Bananza,
    MarioKartWorld,
};

constexpr Game GameOf(u64 program_id) {
    if (program_id == Loader::Nx2DkDynamicFps::ProgramId) {
        return Game::Bananza;
    }
    if (program_id == Loader::Nx2MkwDynamicFps::ProgramId) {
        return Game::MarioKartWorld;
    }
    return Game::Other;
}

/// What a game's older "preset" was stored as: a LegacyPreset number, FollowGlobal (it follows the
/// global one, Bananza's starting preset aside), or -1.
inline constexpr int LegacyFollowGlobal = -2;

/// Carries a game's older preset over as overrides, over the global settings `global` (read with
/// an older file, see Load). A preset the game chose gives the boxes and Bananza's levels it set
/// that differ from global's. Bananza started with Performance whatever the global one, and keeps
/// its levels (not its boxes: those follow global now) unless it followed the global preset.
constexpr void CarryOverPreset(Overrides& own, Game game, int legacy_preset, const Switches& global) {
    const bool chosen = legacy_preset == LegacyPreset::Quality ||
                        legacy_preset == LegacyPreset::Reactive ||
                        legacy_preset == LegacyPreset::Safe ||
                        legacy_preset == LegacyPreset::Performance;
    const bool bananza_start = game == Game::Bananza && !chosen &&
                               legacy_preset != LegacyFollowGlobal;
    if (!chosen && !bananza_start) {
        return;
    }
    if (chosen) {
        Switches target = Apply(legacy_preset == LegacyPreset::Safe           ? Setup::Safe
                                : legacy_preset == LegacyPreset::Reactive     ? Setup::Experimental
                                                                              : Setup::Defaults,
                                global);
        for (std::size_t i = 0; i < Keys.size(); ++i) {
            if (!own.boxes[i] && target.*Keys[i].field != global.*Keys[i].field) {
                own.boxes[i] = target.*Keys[i].field;
            }
        }
    }
    if (game != Game::Bananza) {
        return;
    }
    const bool game_makes_it = legacy_preset == LegacyPreset::Quality ||
                               legacy_preset == LegacyPreset::Safe;
    const BananzaLevels levels = game_makes_it ? BananzaQuality : BananzaPerformance;
    const BananzaLevels base = BananzaDefault(global);
    if (!own.dk_foliage && levels.foliage != base.foliage) {
        own.dk_foliage = levels.foliage;
    }
    if (!own.dk_reflections && levels.reflections != base.reflections) {
        own.dk_reflections = levels.reflections;
    }
    if (!own.dk_shadow && levels.shadow != base.shadow) {
        own.dk_shadow = levels.shadow;
    }
}

struct Var {
    std::string_view name;
    std::string_view value;

    bool operator==(const Var&) const = default;
};

/// The per-draw shortcuts this fork keeps on by default; Safe turns them off (=0), as
/// start-safe.bat does.
inline constexpr std::array<std::string_view, 73> SafeOff{
    "EDEN_NX2_LIGHT_DRAW",       "EDEN_NX2_LIGHT_UNIFORM",     "EDEN_NX2_LIGHT_UNIFORM_VALUES",
    "EDEN_NX2_LIGHT_STORAGE",    "EDEN_NX2_UNIFORM_SYNC_SKIP", "EDEN_NX2_STORAGE_KEEP",
    "EDEN_NX2_UBO_REUSE",        "EDEN_NX2_UBO_DIRECT",        "EDEN_NX2_UBO_STAGE",
    "EDEN_NX2_UBO_BIND_SKIP",    "EDEN_NX2_SSBO_FAST",         "EDEN_NX2_SSBO_MEMO",
    "EDEN_NX2_SYNC_SETS",        "EDEN_NX2_SYNC_SKIP",         "EDEN_NX2_SYNC_EXACT",
    "EDEN_NX2_SYNC_MEMO",        "EDEN_NX2_VERTEX_SKIP",       "EDEN_NX2_VERTEX_FAST",
    "EDEN_NX2_VERTEX_CLEAN",     "EDEN_NX2_LIGHT_SYNC_SKIP",   "EDEN_NX2_INDEX_SKIP",
    "EDEN_NX2_INLINE_FIX",       "EDEN_NX2_TOUCH_SKIP",        "EDEN_NX2_TEX_TOUCH_SKIP",
    "EDEN_NX2_ALIAS_LAZY",       "EDEN_NX2_USAGE_SKIP",        "EDEN_NX2_USAGE_RESET_SKIP",
    "EDEN_NX2_GPU_WRITTEN_FILTER", "EDEN_NX2_MARK_WORDS",      "EDEN_NX2_INVAL_HULL",
    "EDEN_NX2_INVAL_SKIP",       "EDEN_NX2_SMALL_WRITE",       "EDEN_NX2_RANGE_SKIP",
    "EDEN_NX2_SLOT_SKIP",        "EDEN_NX2_QUERY_BOUNDS",      "EDEN_NX2_SHADER_BOUNDS",
    "EDEN_NX2_DESC_REUSE",       "EDEN_NX2_DESC_HULL",         "EDEN_NX2_DESC_SECONDARY",
    "EDEN_NX2_DESC_XLAT",        "EDEN_NX2_KEY_CMP",           "EDEN_NX2_NOTIFY_LAZY",
    "EDEN_NX2_RESET_BATCH",      "EDEN_NX2_RESET_LAST",        "EDEN_NX2_RESET_RUNS",
    "EDEN_NX2_RESET_SORT",       "EDEN_NX2_WRITE_BATCH",       "EDEN_NX2_BATCH_DIRECT",
    "EDEN_NX2_CB_RUNS",          "EDEN_NX2_TICK_SKIP",         "EDEN_NX2_HLE_INDIRECT",
    "EDEN_NX2_INDIRECT_BATCH",   "EDEN_NX2_LAYOUT_MEMO",       "EDEN_NX2_SAME_SKIP",
    "EDEN_NX2_HSETP2_REG",       "EDEN_NX2_DIRTY_THUNK_SKIP",  "EDEN_NX2_FAST_PRIORITY_SCAN",
    "EDEN_NX2_PBFLUSH_WINDOW_KIB", "EDEN_NX2_UPSCALE_SCAN",    "EDEN_NX2_UPSCALE_LEARN",
    "EDEN_NX2_UPSCALE_HEADER",   "EDEN_NX2_FRONT_RUNS",        "EDEN_NX2_UBO_KEEP",
    "EDEN_NX2_RX_ONE_SUBMIT",    "EDEN_NX2_ADD_MEMO",          "EDEN_NX2_PAGE_RANGES",
    "EDEN_NX2_DMA_READ_PEEK",    "EDEN_NX2_ACQUIRE_NODRAIN_EQUAL", "EDEN_NX2_RX_COALESCE",
    "EDEN_NX2_WRITTEN_AGE",      "EDEN_NX2_ASYNC_READBACK",    "EDEN_NX2_ACQUIRE_SUSPEND",
    "EDEN_NX2_MEMORY_QUERY_MS",
};

/// The variables a setting can set, besides SafeOff, in the order they are set.
inline constexpr std::array<std::string_view, 20> Names{
    "EDEN_NX2_BACK_STAGES",   "EDEN_NX2_BACK_CUTS",
    "EDEN_NX2_ACQUIRE_PEEK",  "EDEN_NX2_JIT_SAME",
    "EDEN_NX2_FRONT_MACRO_LEAN", "EDEN_NX2_FG_TIMED",
    "EDEN_NX2_OSD",           "EDEN_NX2_DK_DYN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE",
    "EDEN_NX2_PACE_SPIN_US",  "EDEN_NX2_MKW_DYN",
    "EDEN_NX2_READ_TRAPS",    "EDEN_NX2_RX_RT_FIT",
    "EDEN_NX2_DK_FOLIAGE_OFF", "EDEN_NX2_DK_SHADOW_CASCADES",
    "EDEN_NX2_DK_CUBEMAP_INTERVAL",
    "EDEN_NX2_DK_FOLIAGE_DENSITY", "EDEN_NX2_DK_FOLIAGE_DISTANCE",
    "EDEN_NX2_DK_FOLIAGE_THORN_DENSITY",
};

/// Calls `f(name)` for every variable the settings can set (Names, then SafeOff).
template <typename F>
void ForEachName(F&& f) {
    for (const std::string_view name : Names) {
        f(name);
    }
    for (const std::string_view name : SafeOff) {
        f(name);
    }
}

/// The variables the settings ask for in `game`, the same values as the launcher of the setup.
/// A game's own never reach another game. Bananza's frame pacing is always set: the patch sets it
/// aside while it applies, and it is what steers the game when the patch is off or does not match.
inline std::vector<Var> Wanted(const Switches& switches, Game game) {
    std::vector<Var> vars;
    if (switches.split_gpu_thread) {
        vars.push_back({"EDEN_NX2_BACK_STAGES", "1"});
        vars.push_back({"EDEN_NX2_BACK_CUTS", "a"});
    }
    if (switches.acquire_peek) {
        vars.push_back({"EDEN_NX2_ACQUIRE_PEEK", "1"});
    }
    if (switches.jit_same) {
        vars.push_back({"EDEN_NX2_JIT_SAME", "1"});
    }
    if (switches.front_macro_lean) {
        vars.push_back({"EDEN_NX2_FRONT_MACRO_LEAN", "1"});
    }
    if (switches.fg_timed) {
        vars.push_back({"EDEN_NX2_FG_TIMED", "1"});
    }
    if (switches.osd) {
        vars.push_back({"EDEN_NX2_OSD", "1"});
    }
    if (game == Game::Bananza) {
        if (switches.dk_dyn) {
            vars.push_back({"EDEN_NX2_DK_DYN", "1"});
        }
        if (switches.dk_foliage == FoliageOff) {
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_OFF", "1"});
        } else if (switches.dk_foliage == FoliageLow) {
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_DENSITY", "10"});
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_DISTANCE", "200"});
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_THORN_DENSITY", "50"});
        } else if (switches.dk_foliage == FoliageMedium) {
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_DENSITY", "50"});
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_DISTANCE", "400"});
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_THORN_DENSITY", "75"});
        }
        if (switches.dk_shadow >= ShadowLow && switches.dk_shadow < ShadowVeryHigh) {
            static constexpr std::array<std::string_view, 3> Cascades{"1", "2", "3"};
            vars.push_back({"EDEN_NX2_DK_SHADOW_CASCADES",
                            Cascades[static_cast<std::size_t>(switches.dk_shadow - 1)]});
        }
        if (switches.dk_reflections == ReflectionsVeryLow) {
            vars.push_back({"EDEN_NX2_DK_CUBEMAP_INTERVAL", "240"});
        } else if (switches.dk_reflections == ReflectionsLow) {
            vars.push_back({"EDEN_NX2_DK_CUBEMAP_INTERVAL", "32"});
        } else if (switches.dk_reflections == ReflectionsMedium) {
            vars.push_back({"EDEN_NX2_DK_CUBEMAP_INTERVAL", "16"});
        }
        vars.push_back({"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"});
        vars.push_back({"EDEN_NX2_MODE_PACE", "2"});
        vars.push_back({"EDEN_NX2_PACE_SPIN_US", "1500"});
    }
    if (game == Game::MarioKartWorld && switches.mkw_dyn) {
        vars.push_back({"EDEN_NX2_MKW_DYN", "1"});
    }
    // On by default (unset). Off is the only value the program sets.
    if (!switches.read_traps) {
        vars.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    }
    if (switches.rt_fit) {
        vars.push_back({"EDEN_NX2_RX_RT_FIT", "3"});
    }
    if (switches.safe_off) {
        for (const std::string_view name : SafeOff) {
            vars.push_back({name, "0"});
        }
    }
    return vars;
}

/// What the program sets: the wanted variables but those already in its environment when it
/// started (a launcher, the user, or nx2_switches.txt set them: theirs wins, set or not by us).
/// `set_outside(name)` tells whether a name was there.
template <typename SetOutside>
std::vector<Var> Plan(const Switches& switches, Game game, SetOutside&& set_outside) {
    std::vector<Var> plan;
    for (const Var& var : Wanted(switches, game)) {
        if (!set_outside(var.name)) {
            plan.push_back(var);
        }
    }
    return plan;
}

/// Whether settings just saved need a restart: only when this process already set its variables
/// for a game (`applied`, the plan it set) and a game of the same kind would now get others.
/// Before the first game nothing was set: the settings simply apply to it.
inline bool NeedsRestart(const std::optional<std::vector<Var>>& applied,
                         const std::vector<Var>& plan_now) {
    return applied.has_value() && *applied != plan_now;
}

} // namespace Core::Nx2PerfSettings
