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
// (dist/dozer/start-*.bat), as a preset and on / off settings kept in the [Anka] group of
// qt-config.ini, the Balanced preset until changed. The program sets the variables in its own
// environment before a game boots (yuzu/nx2perf_session.h); anka-cli and the launchers don't read
// these settings.
//
// Every switch is read once per process, most of them into a function's static the first time
// a game reaches it (EDEN_NX2_BACK_STAGES, ACQUIRE_PEEK, PRESENT_INTERVAL_WORD, MODE_PACE,
// PACE_SPIN_US, DK_DYN, OSD, READ_TRAPS); JIT_SAME, FRONT_MACRO_LEAN and FG_TIMED are read again
// at each boot.
// So the variables are set once, at the first boot, and a later boot that would need others (a
// changed setting, or Bananza after another game and back) restarts the program for that game.
namespace Core::Nx2PerfSettings {

/// The launchers' setups, and Custom: a box changed by hand. The stored numbers stay as they
/// were (Performance is 4) so an existing qt-config.ini keeps its preset. The tab calls Reactive
/// "Experimental".
enum class Preset : u8 {
    Balanced = 0,    ///< start-balanced.bat: all on, read traps on, Reactive Flushing off
    Reactive = 1,    ///< start-reactive-experimental.bat: read traps off, Reactive Flushing on, RX_RT_FIT=3
    Safe = 2,        ///< start-safe.bat: all off, and the shortcuts Dozer keeps on set to 0
    Custom = 3,      ///< the boxes as set, nothing else
    Performance = 4, ///< start-performance.bat: Balanced with read traps off
    Quality = 5,     ///< Balanced with Bananza's foliage, shadows and reflections as the game's
};

/// Combo order in the Tuning tab. Not the stored numbers. Quality is only for Bananza's own
/// (GamePresetOrder): it changes nothing but Bananza's settings.
inline constexpr std::array<Preset, 5> PresetOrder{
    Preset::Performance, Preset::Balanced, Preset::Reactive, Preset::Safe, Preset::Custom,
};
inline constexpr std::array<Preset, 6> BananzaPresetOrder{
    Preset::Performance, Preset::Balanced, Preset::Quality,
    Preset::Reactive,    Preset::Safe,     Preset::Custom,
};

/// The combo index of a preset in `order` (Custom, the last, when it is not one of these).
template <std::size_t N = PresetOrder.size()>
constexpr int IndexOf(Preset preset, const std::array<Preset, N>& order = PresetOrder) {
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        if (order[static_cast<std::size_t>(i)] == preset) {
            return i;
        }
    }
    return static_cast<int>(order.size()) - 1;
}

/// Bananza's shadow quality steps: the sun shadow's most cascades.
inline constexpr int ShadowLow = 1;
inline constexpr int ShadowMedium = 2;
inline constexpr int ShadowVeryHigh = 4;

struct Switches {
    bool read_traps = false;       ///< on leaves EDEN_NX2_READ_TRAPS unset; off sets it to 0
    bool split_gpu_thread = true;  ///< EDEN_NX2_BACK_STAGES=1 + EDEN_NX2_BACK_CUTS=a
    bool acquire_peek = true;      ///< EDEN_NX2_ACQUIRE_PEEK=1
    bool jit_same = true;          ///< EDEN_NX2_JIT_SAME=1
    bool front_macro_lean = true;  ///< EDEN_NX2_FRONT_MACRO_LEAN=1
    bool fg_timed = true;          ///< EDEN_NX2_FG_TIMED=1
    bool osd = true;               ///< EDEN_NX2_OSD=1
    // Game-specific: no preset sets or clears these (the Safe preset leaves them as they are).
    bool dk_dyn = true;            ///< EDEN_NX2_DK_DYN=1, Bananza only
    bool mkw_dyn = true;           ///< EDEN_NX2_MKW_DYN=1, Mario Kart World only
    // Game-specific, but set by the presets (FromPreset): on in Performance, Balanced and
    // Experimental, off in Safe and Quality.
    bool dk_foliage_off = true;    ///< EDEN_NX2_DK_FOLIAGE_OFF=1, Bananza only
    /// Bananza's reflections off (EDEN_NX2_DK_REFLECTIONS_OFF=1); unticked, the game's own.
    bool dk_reflections_off = true;
    /// Bananza's shadow quality, kept per game only: the sun shadow's most cascades, Low 1 to
    /// Very high 4 (the game's own); below it EDEN_NX2_DK_SHADOW_CASCADES=<n>. Medium in
    /// Performance, Balanced and Experimental, Very high in Safe and Quality.
    int dk_shadow = ShadowMedium;
    Preset preset = Preset::Performance;

    bool operator==(const Switches&) const = default;
};

/// What the performance launcher sets. A file that stores no choice (`preset\default`) still means
/// this, as it did before Balanced became the first start's preset (FirstStart).
constexpr Switches Performance() {
    return Switches{};
}

/// What the balanced launcher sets: Performance with read traps on.
constexpr Switches Balanced() {
    Switches switches{};
    switches.read_traps = true;
    switches.preset = Preset::Balanced;
    return switches;
}

/// Balanced with Bananza as the game makes it: foliage, very high shadows and reflections.
constexpr Switches Quality() {
    Switches switches = Balanced();
    switches.dk_foliage_off = false;
    switches.dk_reflections_off = false;
    switches.dk_shadow = ShadowVeryHigh;
    switches.preset = Preset::Quality;
    return switches;
}

/// The settings of a first start (no [Anka] group yet): the balanced launcher's.
constexpr Switches FirstStart() {
    return Balanced();
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
        .dk_foliage_off = false,
        .dk_reflections_off = false,
        .dk_shadow = ShadowVeryHigh,
        .preset = Preset::Safe,
    };
}

/// The keys of the [Anka] group, one per setting (the preset has its own, "preset").
struct Key {
    std::string_view name;
    bool Switches::*field;
};
inline constexpr std::array<Key, 7> Keys{{
    {"read_traps", &Switches::read_traps},
    {"split_gpu_thread", &Switches::split_gpu_thread},
    {"acquire_peek", &Switches::acquire_peek},
    {"jit_same", &Switches::jit_same},
    {"front_macro_lean", &Switches::front_macro_lean},
    {"fg_timed", &Switches::fg_timed},
    {"osd", &Switches::osd},
}};

/// The game-specific keys: stored like the others, but outside the presets (but for
/// bananza_foliage_off, which they set).
inline constexpr std::array<Key, 4> GameKeys{{
    {"bananza_dynamic_fps", &Switches::dk_dyn},
    {"mariokart_dynamic_fps", &Switches::mkw_dyn},
    {"bananza_foliage_off", &Switches::dk_foliage_off},
    {"bananza_reflections_off", &Switches::dk_reflections_off},
}};

inline constexpr std::size_t KeyCount = Keys.size() + GameKeys.size();

/// The key at `index`: Keys, then GameKeys.
constexpr const Key& KeyAt(std::size_t index) {
    return index < Keys.size() ? Keys[index] : GameKeys[index - Keys.size()];
}

/// One game's own choices over what it would run with: a preset, and a box per KeyAt() slot, each
/// or none (it follows the global one). A game with a preset of its own (GamePreset) follows the
/// global preset instead when `global_preset` is set.
struct Overrides {
    std::optional<Preset> preset;
    bool global_preset = false;
    std::array<std::optional<bool>, KeyCount> boxes{};
    std::optional<int> dk_shadow; ///< Bananza's shadow quality, ShadowLow to ShadowVeryHigh

    constexpr bool Empty() const {
        if (preset || global_preset || dk_shadow) {
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

/// `switches` with the overrides' boxes over them (the preset stays).
constexpr Switches WithOverrides(Switches switches, const Overrides& overrides) {
    for (std::size_t i = 0; i < KeyCount; ++i) {
        if (overrides.boxes[i]) {
            switches.*KeyAt(i).field = *overrides.boxes[i];
        }
    }
    if (overrides.dk_shadow) {
        switches.dk_shadow = *overrides.dk_shadow;
    }
    return switches;
}

/// What a game's "preset" is stored as when it follows the global preset.
inline constexpr int FollowGlobalPreset = -2;

/// Reads a game's overrides through `read(key)`: -1 for no override, 0 off, 1 on; "preset" is
/// a preset number, FollowGlobalPreset, or -1 (Custom, or anything else, is none);
/// "bananza_shadow_quality" a shadow quality step or -1.
template <typename Read>
Overrides LoadOverrides(Read&& read) {
    Overrides overrides;
    const int preset = read("preset");
    overrides.global_preset = preset == FollowGlobalPreset;
    if (preset != static_cast<int>(Preset::Custom) && preset >= 0 &&
        preset <= static_cast<int>(Preset::Quality)) {
        overrides.preset = static_cast<Preset>(preset);
    }
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
    return overrides;
}

/// Writes a game's overrides through `write(key, value)`, with the values `LoadOverrides` reads.
template <typename Write>
void SaveOverrides(const Overrides& overrides, Write&& write) {
    int preset = overrides.preset ? static_cast<int>(*overrides.preset) : -1;
    if (overrides.global_preset) {
        preset = FollowGlobalPreset;
    }
    write("preset", preset);
    for (std::size_t i = 0; i < KeyCount; ++i) {
        const auto& box = overrides.boxes[i];
        write(KeyAt(i).name, box ? static_cast<int>(*box) : -1);
    }
    write("bananza_shadow_quality", overrides.dk_shadow.value_or(-1));
}

/// A stored preset number. Anything else is Custom (a file edited by hand).
constexpr Preset PresetFromInt(int value) {
    switch (value) {
    case static_cast<int>(Preset::Balanced):
    case static_cast<int>(Preset::Reactive):
    case static_cast<int>(Preset::Safe):
    case static_cast<int>(Preset::Custom):
    case static_cast<int>(Preset::Performance):
    case static_cast<int>(Preset::Quality):
        return static_cast<Preset>(value);
    default:
        return Preset::Custom;
    }
}

/// The boxes of a preset (Custom: `current` as it is), with the preset set. The dynamic frame
/// rate boxes are always `current`'s; Bananza's foliage, shadows and reflections are the preset's.
/// Performance and Experimental (Reactive) are Balanced's boxes with read traps off; Quality is
/// Balanced's with Bananza as the game makes it.
constexpr Switches FromPreset(Preset preset, const Switches& current) {
    if (preset == Preset::Custom) {
        Switches switches = current;
        switches.preset = Preset::Custom;
        return switches;
    }
    Switches switches = preset == Preset::Safe      ? AllOff()
                        : preset == Preset::Quality ? Quality()
                                                    : Balanced();
    if (preset == Preset::Performance || preset == Preset::Reactive) {
        switches.read_traps = false;
    }
    switches.preset = preset;
    switches.dk_dyn = current.dk_dyn;
    switches.mkw_dyn = current.mkw_dyn;
    return switches;
}

/// Whether the boxes are the preset's (Custom always is).
constexpr bool MatchesPreset(const Switches& switches) {
    const Switches preset = FromPreset(switches.preset, switches);
    for (const Key& key : Keys) {
        if (switches.*key.field != preset.*key.field) {
            return false;
        }
    }
    return true;
}

/// After a box was changed by hand: Custom, unless the boxes are still the preset's.
constexpr Switches AfterBoxChange(Switches switches) {
    if (!MatchesPreset(switches)) {
        switches.preset = Preset::Custom;
    }
    return switches;
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

/// The frame generation a preset asks for: Safe turns it off, the others leave it.
constexpr std::optional<FrameGen> FrameGenFor(Preset preset) {
    if (preset == Preset::Safe) {
        return FrameGen::Off;
    }
    return std::nullopt;
}

/// The barrier feedback loops setting (Graphics > Advanced) a preset asks for: on for Reactive,
/// which needs it (start-reactive-experimental.bat sets both); none for the others.
constexpr std::optional<bool> BarrierFeedbackLoops(Preset preset) {
    if (preset == Preset::Reactive) {
        return true;
    }
    return std::nullopt;
}

/// The Reactive Flushing setting (Graphics > Advanced) a preset asks for; none for Safe and Custom
/// (the launchers' safe one leaves the .ini as it is).
constexpr std::optional<bool> ReactiveFlushing(Preset preset) {
    switch (preset) {
    case Preset::Balanced:
    case Preset::Performance:
    case Preset::Quality:
        return false;
    case Preset::Reactive:
        return true;
    default:
        return std::nullopt;
    }
}

/// Reads every setting through `read(key, default)` and the preset through
/// `read_preset(key, default)`; missing keys give the first start's (Performance). Boxes that are
/// not their preset's (the file edited by hand) make it Custom.
template <typename Read, typename ReadPreset>
Switches Load(Read&& read, ReadPreset&& read_preset) {
    const Preset preset =
        PresetFromInt(read_preset("preset", static_cast<int>(Preset::Performance)));
    // read_traps was added after the other boxes. A file without the key keeps the preset's
    // own value (Balanced on, Performance / Experimental / Safe off, Custom on) instead of being
    // read as Custom.
    const bool read_traps_fallback = FromPreset(preset, Balanced()).read_traps;
    Switches switches{};
    for (const Key& key : Keys) {
        const bool fallback =
            key.name == "read_traps" ? read_traps_fallback : Balanced().*key.field;
        switches.*key.field = read(key.name, fallback);
    }
    const Switches preset_switches = FromPreset(preset, Balanced());
    for (const Key& key : GameKeys) {
        // Bananza's foliage and reflections were added after the presets: a file without them
        // gets the preset's
        const bool by_preset = key.field == &Switches::dk_foliage_off ||
                               key.field == &Switches::dk_reflections_off;
        const bool fallback = by_preset ? preset_switches.*key.field : Balanced().*key.field;
        switches.*key.field = read(key.name, fallback);
    }
    // Bananza's shadow quality is not stored here: the preset's (games keep their own choice)
    switches.dk_shadow = preset_switches.dk_shadow;
    switches.preset = preset;
    return AfterBoxChange(switches);
}

/// Writes every setting through `write(key, value, default)` and the preset through
/// `write_preset(key, value, default)`.
template <typename Write, typename WritePreset>
void Save(const Switches& switches, Write&& write, WritePreset&& write_preset) {
    for (const Key& key : Keys) {
        write(key.name, switches.*key.field, Balanced().*key.field);
    }
    for (const Key& key : GameKeys) {
        write(key.name, switches.*key.field, Balanced().*key.field);
    }
    write_preset("preset", static_cast<int>(switches.preset),
                 static_cast<int>(Preset::Performance));
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

/// The preset a game starts from instead of the global one: Bananza Performance (read traps off).
/// Other games: none, they follow the global preset (Balanced from the first start).
constexpr std::optional<Preset> GamePreset(Game game) {
    return game == Game::Bananza ? std::optional{Preset::Performance} : std::nullopt;
}

/// The preset a game runs with: its own choice, else its starting one (unless it follows the
/// global preset), else the global one.
constexpr Preset PresetFor(Preset global, Game game, const Overrides& own) {
    if (own.preset) {
        return *own.preset;
    }
    const auto start = GamePreset(game);
    return start && !own.global_preset ? *start : global;
}

/// The settings a game runs with: its preset's, then its own boxes.
constexpr Switches ForGame(const Switches& global, Game game, const Overrides& own) {
    Switches switches = global;
    const Preset preset = PresetFor(global.preset, game, own);
    if (preset != global.preset) {
        switches = FromPreset(preset, global);
    }
    return WithOverrides(switches, own);
}

struct Var {
    std::string_view name;
    std::string_view value;

    bool operator==(const Var&) const = default;
};

/// The per-draw shortcuts this fork keeps on by default; the Safe preset turns them off (=0), as
/// start-safe.bat does.
inline constexpr std::array<std::string_view, 68> SafeOff{
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
    "EDEN_NX2_DMA_READ_PEEK",    "EDEN_NX2_ACQUIRE_NODRAIN_EQUAL",
};

/// The variables a setting or preset can set, besides SafeOff, in the order they are set.
inline constexpr std::array<std::string_view, 17> Names{
    "EDEN_NX2_BACK_STAGES",   "EDEN_NX2_BACK_CUTS",
    "EDEN_NX2_ACQUIRE_PEEK",  "EDEN_NX2_JIT_SAME",
    "EDEN_NX2_FRONT_MACRO_LEAN", "EDEN_NX2_FG_TIMED",
    "EDEN_NX2_OSD",           "EDEN_NX2_DK_DYN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE",
    "EDEN_NX2_PACE_SPIN_US",  "EDEN_NX2_MKW_DYN",
    "EDEN_NX2_READ_TRAPS",    "EDEN_NX2_RX_RT_FIT",
    "EDEN_NX2_DK_FOLIAGE_OFF", "EDEN_NX2_DK_SHADOW_CASCADES",
    "EDEN_NX2_DK_REFLECTIONS_OFF",
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

/// The variables the settings ask for in `game`, the same values as the launcher of the preset.
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
        if (switches.dk_foliage_off) {
            vars.push_back({"EDEN_NX2_DK_FOLIAGE_OFF", "1"});
        }
        if (switches.dk_shadow >= ShadowLow && switches.dk_shadow < ShadowVeryHigh) {
            static constexpr std::array<std::string_view, 3> Cascades{"1", "2", "3"};
            vars.push_back({"EDEN_NX2_DK_SHADOW_CASCADES",
                            Cascades[static_cast<std::size_t>(switches.dk_shadow - 1)]});
        }
        if (switches.dk_reflections_off) {
            vars.push_back({"EDEN_NX2_DK_REFLECTIONS_OFF", "1"});
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
    if (switches.preset == Preset::Reactive) {
        vars.push_back({"EDEN_NX2_RX_RT_FIT", "3"});
    }
    if (switches.preset == Preset::Safe) {
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
