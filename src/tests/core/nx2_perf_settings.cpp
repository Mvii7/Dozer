// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/nx2_perf_settings.h"

namespace {

using namespace Core::Nx2PerfSettings;

constexpr u64 Bananza = 0x040074A01BF12000ULL;
constexpr u64 OtherGame = 0x0100000000010000ULL;
constexpr u64 MarioKartWorld = 0x0400C3F00006E000ULL;

// start-performance.bat's lines (dist/dozer), DK_DYN and OSD at their defaults (1).
const std::vector<Var> BalancedBananza{
    {"EDEN_NX2_BACK_STAGES", "1"},
    {"EDEN_NX2_BACK_CUTS", "a"},
    {"EDEN_NX2_ACQUIRE_PEEK", "1"},
    {"EDEN_NX2_JIT_SAME", "1"},
    {"EDEN_NX2_FRONT_MACRO_LEAN", "1"},
    {"EDEN_NX2_FG_TIMED", "1"},
    {"EDEN_NX2_OSD", "1"},
    {"EDEN_NX2_DK_DYN", "1"},
    {"EDEN_NX2_DK_FOLIAGE_DENSITY", "10"},
    {"EDEN_NX2_DK_FOLIAGE_DISTANCE", "200"},
    {"EDEN_NX2_DK_FOLIAGE_THORN_DENSITY", "50"},
    {"EDEN_NX2_DK_SHADOW_CASCADES", "2"},
    {"EDEN_NX2_DK_CUBEMAP_INTERVAL", "240"},
    {"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"},
    {"EDEN_NX2_MODE_PACE", "2"},
    {"EDEN_NX2_PACE_SPIN_US", "1500"},
};
// Of those, the ones Bananza gets that another game does not.
constexpr std::size_t BalancedBananzaOwn = 9;

// The game-specific variables Bananza gets whatever the preset (Safe's: foliage, very high
// shadows and reflections as the game's).
const std::vector<Var> BananzaVars{
    {"EDEN_NX2_DK_DYN", "1"},
    {"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"},
    {"EDEN_NX2_MODE_PACE", "2"},
    {"EDEN_NX2_PACE_SPIN_US", "1500"},
};

const std::vector<Var> MkwVars{
    {"EDEN_NX2_MKW_DYN", "1"},
};

// The game-specific variables set for a game (before the preset's own).
std::vector<Var> GameVars(Game game) {
    switch (game) {
    case Game::Bananza:
        return BananzaVars;
    case Game::MarioKartWorld:
        return MkwVars;
    case Game::Other:
        break;
    }
    return {};
}

const std::set<std::string_view> BananzaOnly{
    "EDEN_NX2_DK_DYN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD",
    "EDEN_NX2_MODE_PACE",
    "EDEN_NX2_PACE_SPIN_US",
    "EDEN_NX2_DK_FOLIAGE_OFF",
    "EDEN_NX2_DK_FOLIAGE_DENSITY",
    "EDEN_NX2_DK_FOLIAGE_DISTANCE",
    "EDEN_NX2_DK_FOLIAGE_THORN_DENSITY",
    "EDEN_NX2_DK_SHADOW_CASCADES",
    "EDEN_NX2_DK_CUBEMAP_INTERVAL",
};

// An ini group as the config sees it: the on / off keys, and "version" and "preset" as numbers.
struct Ini {
    std::map<std::string, bool> values;
    std::map<std::string, int> ints;

    Switches Read() const {
        return Load(
            [this](std::string_view key, bool def) {
                const auto it = values.find(std::string{key});
                return it == values.end() ? def : it->second;
            },
            [this](std::string_view key, int def) {
                const auto it = ints.find(std::string{key});
                return it == ints.end() ? def : it->second;
            });
    }
    void Write(const Switches& switches) {
        Save(
            switches,
            [this](std::string_view key, bool value, bool) { values[std::string{key}] = value; },
            [this](std::string_view key, int value, int) { ints[std::string{key}] = value; });
    }
    // A file written before the presets went away: no version, the preset's number beside the boxes.
    void MakeLegacy(int preset) {
        ints.erase("version");
        ints["preset"] = preset;
    }
};

bool NoneOutside(std::string_view) {
    return false;
}

// What a stored setting reads back as: Bananza's levels are not stored.
Switches Stored(Switches switches) {
    switches.dk_foliage = FoliageLow;
    switches.dk_reflections = ReflectionsVeryLow;
    switches.dk_shadow = ShadowMedium;
    return switches;
}

Switches WithLevels(Switches switches, const BananzaLevels& levels) {
    switches.dk_foliage = levels.foliage;
    switches.dk_reflections = levels.reflections;
    switches.dk_shadow = levels.shadow;
    return switches;
}

constexpr std::array<Setup, 3> Setups{Setup::Defaults, Setup::Experimental, Setup::Safe};

bool HasVar(const std::vector<Var>& vars, std::string_view name) {
    for (const Var& var : vars) {
        if (var.name == name) {
            return true;
        }
    }
    return false;
}

} // Anonymous namespace

TEST_CASE("Nx2PerfSettings: the first start is the performance setup", "[core]") {
    const Ini empty;
    REQUIRE(empty.Read() == Performance());
    REQUIRE(empty.Read() == Apply(Setup::Defaults, Performance()));
    REQUIRE(FirstStart() == Performance());
    REQUIRE(Wanted(Performance(), Game::Bananza) == BalancedBananza);
}

TEST_CASE("Nx2PerfSettings: every setting reads back what was written", "[core]") {
    for (const Key& key : Keys) {
        Switches switches = Performance();
        switches.*key.field = !(switches.*key.field);
        Ini ini;
        ini.Write(switches);
        REQUIRE(ini.values.size() == Keys.size() + GameKeys.size());
        REQUIRE(ini.Read() == switches);
        REQUIRE(ini.values.at(std::string{key.name}) == switches.*key.field);
        // Missing from the file (before the setting existed): its default.
        ini.values.erase(std::string{key.name});
        REQUIRE(ini.Read() == Performance());
    }
    for (const Key& key : GameKeys) {
        for (const Setup setup : Setups) {
            Switches switches = Apply(setup, Performance());
            switches.*key.field = false;
            REQUIRE(Apply(setup, switches).*key.field == false);
            Ini ini;
            ini.Write(switches);
            REQUIRE(ini.values.at(std::string{key.name}) == false);
            REQUIRE(ini.Read() == Stored(switches));
        }
    }
    for (const Setup setup : Setups) {
        Ini ini;
        ini.Write(Apply(setup, Performance()));
        REQUIRE(ini.Read() == Stored(Apply(setup, Performance())));
    }
    // The version is written, no preset is.
    Ini ini;
    ini.Write(Performance());
    REQUIRE(ini.ints.at("version") == FormatVersion);
    REQUIRE(ini.ints.count("preset") == 0);
}

TEST_CASE("Nx2PerfSettings: a file from before the presets went away", "[core]") {
    // The preset's boxes were stored with it; what it set besides them comes from its number.
    Ini safe;
    safe.Write(AllOff());
    safe.MakeLegacy(LegacyPreset::Safe);
    safe.values.erase("safe_off");
    REQUIRE(safe.Read() == Stored(AllOff()));
    REQUIRE(safe.Read().safe_off);

    Ini experimental;
    experimental.Write(Apply(Setup::Experimental, Performance()));
    experimental.MakeLegacy(LegacyPreset::Reactive);
    experimental.values.erase("rt_fit");
    REQUIRE(experimental.Read() == Apply(Setup::Experimental, Performance()));

    // Performance had read traps off: such a file is Performance with them on.
    Ini old;
    old.Write(Performance());
    old.values["read_traps"] = false;
    old.MakeLegacy(LegacyPreset::Performance);
    REQUIRE(old.Read() == Performance());
    // Quality (stored as 0, once Balanced) reads as the boxes it had.
    Ini balanced;
    balanced.Write(Performance());
    balanced.MakeLegacy(LegacyPreset::Quality);
    REQUIRE(balanced.Read() == Performance());
    // A file from before read_traps: the key is missing and the preset's value is used.
    for (const int preset : {LegacyPreset::Reactive, LegacyPreset::Safe}) {
        Ini before;
        before.Write(Performance());
        before.values.erase("read_traps");
        before.MakeLegacy(preset);
        REQUIRE(!before.Read().read_traps);
    }
    Ini before;
    before.Write(Performance());
    before.values.erase("read_traps");
    before.MakeLegacy(LegacyPreset::Quality);
    REQUIRE(before.Read().read_traps);
    // No preset stored either: the first start's.
    Ini none;
    none.values["osd"] = false;
    REQUIRE(none.Read().osd == false);
    REQUIRE(!none.Read().safe_off);
    // Once the version is written the old preset is not read again.
    Ini current;
    current.Write(Performance());
    current.ints["preset"] = LegacyPreset::Safe;
    REQUIRE(current.Read() == Performance());
}

TEST_CASE("Nx2PerfSettings: setups fill in the launchers' boxes", "[core]") {
    for (const Setup setup : {Setup::Defaults, Setup::Experimental}) {
        const Switches switches = Apply(setup, AllOff());
        for (const Key& key : Keys) {
            const bool on = key.name == "read_traps" ? setup == Setup::Defaults
                            : key.name == "safe_off" ? false
                            : key.name == "rt_fit"   ? setup == Setup::Experimental
                                                     : true;
            REQUIRE(switches.*key.field == on);
        }
    }
    REQUIRE(Apply(Setup::Defaults, AllOff()) == Performance());
    REQUIRE(Apply(Setup::Safe, Performance()) == AllOff());
    for (const Key& key : Keys) {
        const bool safe_box = key.name == "safe_off";
        REQUIRE(AllOff().*key.field == safe_box);
    }
    // The game-specific boxes are the current ones, whatever the setup.
    Switches mine = Performance();
    mine.dk_dyn = false;
    mine.mkw_dyn = false;
    for (const Setup setup : Setups) {
        REQUIRE(!Apply(setup, mine).dk_dyn);
        REQUIRE(!Apply(setup, mine).mkw_dyn);
    }
    // Reactive Flushing: Defaults off, Experimental on, Safe leaves it.
    REQUIRE(ReactiveFlushing(Setup::Defaults) == false);
    REQUIRE(ReactiveFlushing(Setup::Experimental) == true);
    REQUIRE(!ReactiveFlushing(Setup::Safe).has_value());
    // Barrier feedback loops: on with Experimental (it needs them), the others leave it.
    REQUIRE(BarrierFeedbackLoops(Setup::Experimental) == true);
    REQUIRE(!BarrierFeedbackLoops(Setup::Defaults).has_value());
    REQUIRE(!BarrierFeedbackLoops(Setup::Safe).has_value());
}

TEST_CASE("Nx2PerfSettings: setups set the launchers' variables", "[core]") {
    // Defaults: start-performance.bat's, read traps on (unset).
    REQUIRE(Wanted(Apply(Setup::Defaults, Performance()), Game::Bananza) == BalancedBananza);
    // Experimental: Defaults', read traps off and RX_RT_FIT=3 (start-reactive-experimental.bat).
    auto reactive = BalancedBananza;
    reactive.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    reactive.push_back({"EDEN_NX2_RX_RT_FIT", "3"});
    REQUIRE(Wanted(Apply(Setup::Experimental, Performance()), Game::Bananza) == reactive);
    // Safe: none of the boxes, read traps off, every per-draw shortcut =0 (start-safe.bat).
    // The game-specific settings are not Safe's to change: a game's own variables stay.
    for (const Game game : {Game::Bananza, Game::MarioKartWorld, Game::Other}) {
        const auto safe = Wanted(AllOff(), game);
        const auto own = GameVars(game);
        REQUIRE(safe.size() == own.size() + SafeOff.size() + 1);
        REQUIRE(std::vector<Var>(safe.begin(), safe.begin() + own.size()) == own);
        REQUIRE(safe[own.size()] == Var{"EDEN_NX2_READ_TRAPS", "0"});
        for (std::size_t i = 0; i < SafeOff.size(); ++i) {
            REQUIRE(safe[own.size() + 1 + i] == Var{SafeOff[i], "0"});
        }
    }
    // The shortcuts are a box of their own: everything else off sets read traps off only.
    Switches boxes_off = AllOff();
    boxes_off.safe_off = false;
    auto only_traps = BananzaVars;
    only_traps.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    REQUIRE(Wanted(boxes_off, Game::Bananza) == only_traps);
    Switches shortcuts = Performance();
    shortcuts.safe_off = true;
    auto with_shortcuts = BalancedBananza;
    for (const std::string_view name : SafeOff) {
        with_shortcuts.push_back({name, "0"});
    }
    REQUIRE(Wanted(shortcuts, Game::Bananza) == with_shortcuts);
}

TEST_CASE("Nx2PerfSettings: keys are distinct and cover every field", "[core]") {
    std::set<std::string_view> names;
    for (const Key& key : Keys) {
        names.insert(key.name);
        Switches one = Performance();
        one.*key.field = !(one.*key.field);
        REQUIRE(one != Performance());
    }
    for (const Key& key : GameKeys) {
        names.insert(key.name);
        Switches one = Performance();
        one.*key.field = !(one.*key.field);
        REQUIRE(one != Performance());
    }
    REQUIRE(names.size() == Keys.size() + GameKeys.size());
    REQUIRE(names.count("preset") == 0);
    REQUIRE(names.count("version") == 0);
}

TEST_CASE("Nx2PerfSettings: a game by the patch's own program id", "[core]") {
    REQUIRE(GameOf(Loader::Nx2DkDynamicFps::ProgramId) == Game::Bananza);
    REQUIRE(GameOf(Bananza) == Game::Bananza);
    REQUIRE(GameOf(Loader::Nx2MkwDynamicFps::ProgramId) == Game::MarioKartWorld);
    REQUIRE(GameOf(MarioKartWorld) == Game::MarioKartWorld);
    REQUIRE(GameOf(OtherGame) == Game::Other);
    REQUIRE(GameOf(0) == Game::Other);
    // Their updates' ids: the patches check the base ids only.
    REQUIRE(GameOf(Bananza + 0x800) == Game::Other);
    REQUIRE(GameOf(MarioKartWorld + 0x800) == Game::Other);
}

TEST_CASE("Nx2PerfSettings: a game's settings never reach another game", "[core]") {
    for (const Setup setup : Setups) {
        const Switches switches = Apply(setup, Performance());
        for (const Var& var : Wanted(switches, Game::Other)) {
            REQUIRE(BananzaOnly.count(var.name) == 0);
            REQUIRE(var.name != "EDEN_NX2_MKW_DYN");
        }
        for (const Var& var : Wanted(switches, Game::MarioKartWorld)) {
            REQUIRE(BananzaOnly.count(var.name) == 0);
        }
        for (const Var& var : Wanted(switches, Game::Bananza)) {
            REQUIRE(var.name != "EDEN_NX2_MKW_DYN");
        }
    }
    const auto other = Wanted(Performance(), GameOf(OtherGame));
    REQUIRE(other.size() == BalancedBananza.size() - BalancedBananzaOwn);
    // Bananza's dynamic frame rate off: its frame pacing stays, always.
    Switches no_dk = Performance();
    no_dk.dk_dyn = false;
    const auto pacing = Wanted(no_dk, Game::Bananza);
    REQUIRE(pacing.size() == other.size() + BalancedBananzaOwn - 1);
    REQUIRE(pacing.back() == Var{"EDEN_NX2_PACE_SPIN_US", "1500"});
    // Mario Kart World's switch is its own: Bananza's box does not touch it.
    REQUIRE(Wanted(no_dk, Game::MarioKartWorld) == Wanted(Performance(), Game::MarioKartWorld));
    auto mkw = other;
    mkw.push_back({"EDEN_NX2_MKW_DYN", "1"});
    REQUIRE(Wanted(Performance(), Game::MarioKartWorld) == mkw);
    Switches no_mkw = Performance();
    no_mkw.mkw_dyn = false;
    REQUIRE(Wanted(no_mkw, Game::MarioKartWorld) == other);
    REQUIRE(Wanted(no_mkw, Game::Bananza) == BalancedBananza);
}

TEST_CASE("Nx2PerfSettings: a variable set outside the program wins", "[core]") {
    REQUIRE(Plan(Performance(), Game::Bananza, NoneOutside) == BalancedBananza);

    // A launcher (or the user) set two of them: those are left out, whatever their value.
    const std::set<std::string_view> outside{"EDEN_NX2_JIT_SAME", "EDEN_NX2_MODE_PACE"};
    const auto is_outside = [&](std::string_view name) { return outside.count(name) != 0; };
    const auto plan = Plan(Performance(), Game::Bananza, is_outside);
    REQUIRE(plan.size() == BalancedBananza.size() - 2);
    for (const Var& var : plan) {
        REQUIRE(outside.count(var.name) == 0);
    }

    // Set outside and turned off here: still theirs (the plan never clears a variable).
    Switches off = Performance();
    off.jit_same = false;
    REQUIRE(Plan(off, Game::Bananza, is_outside) == plan);

    // Safe's shortcuts set outside: theirs too (READ_TRAPS=0 besides).
    const auto light = [](std::string_view name) { return name == "EDEN_NX2_LIGHT_DRAW"; };
    REQUIRE(Plan(AllOff(), Game::Bananza, light).size() == SafeOff.size() + BananzaVars.size());

    // Everything set outside: nothing to set.
    REQUIRE(Plan(Performance(), Game::Bananza, [](std::string_view) { return true; }).empty());
    REQUIRE(Plan(AllOff(), Game::Bananza, [](std::string_view) { return true; }).empty());
}

TEST_CASE("Nx2PerfSettings: every name is one a setup or a game's setting can set", "[core]") {
    std::set<std::string_view> wanted;
    for (const Setup setup : Setups) {
        for (const Game game : {Game::Bananza, Game::MarioKartWorld}) {
            for (const Var& var : Wanted(Apply(setup, Performance()), game)) {
                wanted.insert(var.name);
            }
        }
    }
    // Bananza's Low shadows, Medium reflection updates and foliage off are no setup's
    Switches low_shadow = Performance();
    low_shadow.dk_shadow = ShadowLow;
    low_shadow.dk_reflections = ReflectionsMedium;
    low_shadow.dk_foliage = FoliageOff;
    for (const Var& var : Wanted(low_shadow, Game::Bananza)) {
        wanted.insert(var.name);
    }
    std::set<std::string_view> names;
    ForEachName([&](std::string_view name) { names.insert(name); });
    REQUIRE(names.size() == Names.size() + SafeOff.size()); // no name twice
    REQUIRE(wanted == names);
}

TEST_CASE("Nx2PerfSettings: a boot that needs other variables differs from the session's", "[core]") {
    // What the session compares (yuzu/nx2perf_session.cpp): the plan of the first boot.
    const auto first_bananza = Plan(Performance(), GameOf(Bananza), NoneOutside);
    REQUIRE(Plan(Performance(), GameOf(Bananza), NoneOutside) == first_bananza);
    REQUIRE(Plan(Performance(), GameOf(OtherGame), NoneOutside) != first_bananza);
    Switches changed = Performance();
    changed.osd = false;
    REQUIRE(Plan(changed, Game::Bananza, NoneOutside) != first_bananza);
    // Bananza's own switches set outside: another game needs nothing else, no restart.
    const auto outside = [](std::string_view name) { return BananzaOnly.count(name) != 0; };
    REQUIRE(Plan(Performance(), Game::Bananza, outside) == Plan(Performance(), Game::Other, outside));
    // Another game's own settings differ from Bananza's, so moving between them restarts.
    REQUIRE(Plan(Performance(), Game::MarioKartWorld, NoneOutside) != first_bananza);
    REQUIRE(Plan(Performance(), Game::MarioKartWorld, NoneOutside) !=
            Plan(Performance(), Game::Other, NoneOutside));
}

TEST_CASE("Nx2PerfSettings: saving settings needs a restart only after a game set them", "[core]") {
    const auto plan_now = [](const Switches& switches) {
        return Plan(switches, Game::Bananza, NoneOutside);
    };
    // No game started in this process yet: never (they apply to the first game).
    REQUIRE(!NeedsRestart(std::nullopt, plan_now(AllOff())));
    REQUIRE(!NeedsRestart(std::nullopt, plan_now(Performance())));
    // Set for a game: only when the variables would differ.
    const std::optional<std::vector<Var>> applied = plan_now(Performance());
    REQUIRE(!NeedsRestart(applied, plan_now(Performance())));
    REQUIRE(NeedsRestart(applied, plan_now(AllOff())));
    REQUIRE(NeedsRestart(applied, plan_now(Apply(Setup::Experimental, Performance()))));
    Switches no_osd = Performance();
    no_osd.osd = false;
    REQUIRE(NeedsRestart(applied, plan_now(no_osd)));
    // A change that only touches variables set outside: nothing of ours differs, no restart.
    const auto osd_outside = [](std::string_view name) { return name == "EDEN_NX2_OSD"; };
    const std::optional<std::vector<Var>> applied_outside =
        Plan(Performance(), Game::Bananza, osd_outside);
    REQUIRE(!NeedsRestart(applied_outside, Plan(no_osd, Game::Bananza, osd_outside)));
}

TEST_CASE("Nx2PerfSettings: the frame generation choice and the Graphics settings", "[core]") {
    const FrameGenSettings off{};
    // Each choice gives the settings that read back as that choice.
    for (const FrameGen choice : {FrameGen::Off, FrameGen::Fixed2x, FrameGen::Dynamic}) {
        REQUIRE(FrameGenFrom(FrameGenApply(choice, off)) == choice);
    }
    REQUIRE(FrameGenApply(FrameGen::Fixed2x, off) ==
            FrameGenSettings{.on = true, .fsr3 = true, .multiplier = 2, .target_rate = 0});
    REQUIRE(FrameGenApply(FrameGen::Dynamic, off).target_rate == DYNAMIC_TARGET_RATE);
    REQUIRE(DYNAMIC_TARGET_RATE == 120);
    // Off only turns it off; Other changes nothing.
    const FrameGenSettings lsfg3{.on = true, .fsr3 = false, .multiplier = 3, .target_rate = 0};
    REQUIRE(FrameGenFrom(lsfg3) == FrameGen::Other);
    REQUIRE(FrameGenApply(FrameGen::Off, lsfg3) ==
            FrameGenSettings{.on = false, .fsr3 = false, .multiplier = 3, .target_rate = 0});
    REQUIRE(FrameGenApply(FrameGen::Other, lsfg3) == lsfg3);
    // Dynamic switches the type to FSR3 and keeps the multiplier (the target rate decides).
    REQUIRE(FrameGenApply(FrameGen::Dynamic, lsfg3) ==
            FrameGenSettings{.on = true, .fsr3 = true, .multiplier = 3, .target_rate = 120});
    // FSR3 with another multiplier or target rate is the Graphics tab's own setup.
    REQUIRE(FrameGenFrom({.on = true, .fsr3 = true, .multiplier = 3, .target_rate = 0}) ==
            FrameGen::Other);
    REQUIRE(FrameGenFrom({.on = true, .fsr3 = true, .multiplier = 2, .target_rate = 60}) ==
            FrameGen::Other);
    REQUIRE(FrameGenFrom({.on = false, .fsr3 = true, .multiplier = 2, .target_rate = 120}) ==
            FrameGen::Off);
    // Safe turns it off, the others leave it.
    REQUIRE(FrameGenFor(Setup::Safe) == FrameGen::Off);
    REQUIRE(!FrameGenFor(Setup::Defaults).has_value());
    REQUIRE(!FrameGenFor(Setup::Experimental).has_value());
}

TEST_CASE("Nx2PerfSettings: a game's own boxes go over the global ones", "[core]") {
    Switches global = Performance();
    global.read_traps = false;

    // No overrides: the global settings as they are.
    REQUIRE(Overrides{}.Empty());
    REQUIRE(WithOverrides(global, Overrides{}) == global);

    // Mario Kart World with read traps on, the others as the global ones.
    Overrides mkw;
    mkw.boxes[0] = true;
    REQUIRE(Keys[0].name == "read_traps");
    REQUIRE(!mkw.Empty());
    const Switches with_traps = WithOverrides(global, mkw);
    REQUIRE(with_traps.read_traps);
    REQUIRE(with_traps.osd == global.osd);

    // Read traps on leave EDEN_NX2_READ_TRAPS unset; off sets it to 0.
    REQUIRE(HasVar(Wanted(global, Game::Bananza), "EDEN_NX2_READ_TRAPS"));
    REQUIRE(!HasVar(Wanted(with_traps, Game::MarioKartWorld), "EDEN_NX2_READ_TRAPS"));
    // The two games differ in the variables, so the session asks for a restart between them.
    const auto bananza = Plan(global, Game::Bananza, NoneOutside);
    const auto mario_kart = Plan(with_traps, Game::MarioKartWorld, NoneOutside);
    REQUIRE(NeedsRestart(bananza, mario_kart));

    // A game-specific box is an override like the others.
    Overrides dyn;
    dyn.boxes[Keys.size() + 1] = false;
    REQUIRE(GameKeys[1].name == "mariokart_dynamic_fps");
    REQUIRE(!WithOverrides(global, dyn).mkw_dyn);
    REQUIRE(WithOverrides(global, dyn).dk_dyn);
}

TEST_CASE("Nx2PerfSettings: a game's overrides read back what was written", "[core]") {
    Overrides overrides;
    overrides.boxes[0] = true;
    overrides.boxes[3] = false;
    REQUIRE(Keys[3].name == "jit_same");
    overrides.boxes[KeyCount - 1] = true;

    std::map<std::string, int> ini;
    SaveOverrides(overrides, [&](std::string_view key, int value) { ini[std::string{key}] = value; });
    // The boxes, the shadow quality, the reflection updates and the foliage, and the boxes these
    // two replace (cleared).
    REQUIRE(ini.size() == KeyCount + 5);
    REQUIRE(ini.count("preset") == 0);
    REQUIRE(ini.at("bananza_shadow_quality") == -1);
    REQUIRE(ini.at("bananza_reflection_updates") == -1);
    REQUIRE(ini.at("bananza_reflections_off") == -1);
    REQUIRE(ini.at("read_traps") == 1);
    REQUIRE(ini.at("jit_same") == 0);
    REQUIRE(ini.at("acquire_peek") == -1);

    const auto read = [&](std::string_view key) {
        const auto it = ini.find(std::string{key});
        return it == ini.end() ? -1 : it->second;
    };
    REQUIRE(LoadOverrides(read) == overrides);

    // Missing keys and values that mean nothing: no override. An older game's preset is not one.
    REQUIRE(LoadOverrides([](std::string_view) { return -1; }).Empty());
    REQUIRE(LoadOverrides([](std::string_view) { return 7; }).Empty());
    REQUIRE(LoadOverrides([](std::string_view key) { return key == "preset" ? 2 : -1; }).Empty());
}

TEST_CASE("Nx2PerfSettings: every game follows the global settings", "[core]") {
    // The overlay off in the global settings reaches every game (the Bananza one too).
    Switches global = Performance();
    global.osd = false;
    for (const Game game : {Game::Bananza, Game::MarioKartWorld, Game::Other}) {
        REQUIRE(!ForGame(global, {}).osd);
        REQUIRE(!HasVar(Wanted(ForGame(global, {}), game), "EDEN_NX2_OSD"));
    }
    // Whatever was filled in, a game that changed nothing runs the global boxes as they are.
    for (const Setup setup : Setups) {
        const Switches filled = Apply(setup, Performance());
        for (const Key& key : Keys) {
            REQUIRE(ForGame(filled, {}).*key.field == filled.*key.field);
        }
        REQUIRE(ForGame(filled, {}).dk_dyn == filled.dk_dyn);
        REQUIRE(ForGame(filled, {}).mkw_dyn == filled.mkw_dyn);
    }
    // Its own box goes over the global one, and only that one.
    Overrides own;
    own.boxes[6] = true;
    REQUIRE(Keys[6].name == "osd");
    REQUIRE(ForGame(global, own).osd);
    REQUIRE(ForGame(global, own).read_traps == global.read_traps);
    // Changing the global box again changes what the other boxes follow, not the override.
    global.read_traps = false;
    REQUIRE(!ForGame(global, own).read_traps);
    REQUIRE(ForGame(global, own).osd);
    // What the first start gives is the same for every game.
    REQUIRE(ForGame(FirstStart(), {}).read_traps);
    // The game-specific boxes are the global ones.
    Switches dyn = Performance();
    dyn.dk_dyn = false;
    dyn.mkw_dyn = false;
    REQUIRE(!ForGame(dyn, {}).dk_dyn);
    REQUIRE(!ForGame(dyn, {}).mkw_dyn);
    // The shortcuts and RX_RT_FIT follow like the others.
    REQUIRE(ForGame(Apply(Setup::Safe, Performance()), {}).safe_off);
    REQUIRE(ForGame(Apply(Setup::Experimental, Performance()), {}).rt_fit);
}

TEST_CASE("Nx2PerfSettings: Bananza's levels", "[core]") {
    // Low foliage, very low reflection updates and medium shadows, unless the shortcuts are off
    // (Safe): then the game's own.
    REQUIRE(BananzaDefault(Performance()) == BananzaPerformance);
    REQUIRE(BananzaDefault(AllOff()) == BananzaQuality);
    REQUIRE(BananzaQuality.foliage == FoliageHigh);
    REQUIRE(BananzaQuality.reflections == ReflectionsHigh);
    REQUIRE(BananzaQuality.shadow == ShadowVeryHigh);
    const Switches performance = ForGame(Performance(), {});
    REQUIRE(performance.dk_foliage == FoliageLow);
    REQUIRE(performance.dk_reflections == ReflectionsVeryLow);
    REQUIRE(performance.dk_shadow == ShadowMedium);
    const Switches safe = ForGame(AllOff(), {});
    REQUIRE(safe.dk_foliage == FoliageHigh);
    REQUIRE(safe.dk_reflections == ReflectionsHigh);
    REQUIRE(safe.dk_shadow == ShadowVeryHigh);
    // Quality is Performance's boxes, and only changes Bananza's levels.
    for (const Key& key : Keys) {
        REQUIRE(Performance().*key.field == ForGame(Performance(), {}).*key.field);
    }
    auto quality = BalancedBananza;
    quality.erase(std::remove_if(quality.begin(), quality.end(),
                                 [](const Var& var) {
                                     return var.name == "EDEN_NX2_DK_FOLIAGE_DENSITY" ||
                                            var.name == "EDEN_NX2_DK_FOLIAGE_DISTANCE" ||
                                            var.name == "EDEN_NX2_DK_FOLIAGE_THORN_DENSITY" ||
                                            var.name == "EDEN_NX2_DK_SHADOW_CASCADES" ||
                                            var.name == "EDEN_NX2_DK_CUBEMAP_INTERVAL";
                                 }),
                  quality.end());
    Overrides own;
    own.dk_foliage = BananzaQuality.foliage;
    own.dk_reflections = BananzaQuality.reflections;
    own.dk_shadow = BananzaQuality.shadow;
    REQUIRE(Wanted(ForGame(Performance(), own), Game::Bananza) == quality);
    // Only Bananza gets the variables.
    const Var foliage_low{"EDEN_NX2_DK_FOLIAGE_DENSITY", "10"};
    const auto has = [&](const std::vector<Var>& vars) {
        return std::find(vars.begin(), vars.end(), foliage_low) != vars.end();
    };
    REQUIRE(has(Wanted(Performance(), Game::Bananza)));
    REQUIRE_FALSE(has(Wanted(Performance(), Game::MarioKartWorld)));
    REQUIRE_FALSE(has(Wanted(Performance(), Game::Other)));
    REQUIRE_FALSE(has(Wanted(AllOff(), Game::Bananza)));
    // A game's own level goes over the default.
    Overrides one;
    one.dk_foliage = FoliageHigh;
    one.dk_shadow = ShadowLow;
    REQUIRE(ForGame(Performance(), one).dk_foliage == FoliageHigh);
    REQUIRE(ForGame(Performance(), one).dk_shadow == ShadowLow);
    REQUIRE(ForGame(Performance(), one).dk_reflections == ReflectionsVeryLow);
    // A file from before the dropdown, with Bananza's foliage off box: read as the old box.
    Ini old;
    old.Write(Performance());
    REQUIRE(old.Read() == Performance());
}

TEST_CASE("Nx2PerfSettings: an older game's preset is carried over", "[core]") {
    const Switches global = Performance();
    // No preset and not Bananza: nothing to carry.
    Overrides other;
    CarryOverPreset(other, Game::Other, -1, global);
    REQUIRE(other.Empty());
    // Following the global preset: nothing either.
    Overrides follow;
    CarryOverPreset(follow, Game::Bananza, LegacyFollowGlobal, global);
    REQUIRE(follow.Empty());
    // Bananza started from Performance whatever the global one: with Quality or Safe on the global
    // side it keeps its low levels, and with Performance there is nothing to keep.
    Overrides bananza;
    CarryOverPreset(bananza, Game::Bananza, -1, global);
    REQUIRE(bananza.Empty());
    Switches safe_global = AllOff();
    CarryOverPreset(bananza, Game::Bananza, -1, safe_global);
    REQUIRE(bananza.dk_foliage == FoliageLow);
    REQUIRE(bananza.dk_reflections == ReflectionsVeryLow);
    REQUIRE(bananza.dk_shadow == ShadowMedium);
    // Its boxes are not kept (they follow the global ones now).
    for (const auto& box : bananza.boxes) {
        REQUIRE(!box.has_value());
    }
    // A preset a game chose gives the boxes it set that differ from the global ones.
    Overrides safe;
    CarryOverPreset(safe, Game::Other, LegacyPreset::Safe, global);
    REQUIRE(safe.boxes[0] == false);   // read traps
    REQUIRE(safe.boxes[6] == false);   // overlay
    REQUIRE(safe.boxes[7] == true);    // shortcuts
    REQUIRE(!safe.boxes[8].has_value()); // RX_RT_FIT: global's already off
    REQUIRE(!safe.dk_foliage.has_value()); // only Bananza has levels
    Overrides reactive;
    CarryOverPreset(reactive, Game::MarioKartWorld, LegacyPreset::Reactive, global);
    REQUIRE(reactive.boxes[0] == false);
    REQUIRE(reactive.boxes[8] == true);
    REQUIRE(!reactive.boxes[6].has_value());
    // Bananza with Quality: the game's own levels; with Performance chosen, none to keep.
    Overrides quality;
    CarryOverPreset(quality, Game::Bananza, LegacyPreset::Quality, global);
    REQUIRE(quality.dk_foliage == FoliageHigh);
    REQUIRE(quality.dk_reflections == ReflectionsHigh);
    REQUIRE(quality.dk_shadow == ShadowVeryHigh);
    Overrides performance;
    CarryOverPreset(performance, Game::Bananza, LegacyPreset::Performance, global);
    REQUIRE(performance.Empty());
    // What a game already chose is kept.
    Overrides chosen;
    chosen.boxes[0] = true;
    chosen.dk_shadow = ShadowLow;
    CarryOverPreset(chosen, Game::Bananza, LegacyPreset::Safe, global);
    REQUIRE(chosen.boxes[0] == true);
    REQUIRE(chosen.dk_shadow == ShadowLow);
}

TEST_CASE("Nx2PerfSettings: Bananza's foliage", "[core]") {
    const auto foliage = [](const Switches& switches) {
        std::vector<Var> out;
        for (const Var& var : Wanted(switches, Game::Bananza)) {
            if (var.name.starts_with("EDEN_NX2_DK_FOLIAGE")) {
                out.push_back(var);
            }
        }
        return out;
    };
    Switches switches = Performance();
    switches.dk_foliage = FoliageOff;
    REQUIRE(foliage(switches) == std::vector<Var>{{"EDEN_NX2_DK_FOLIAGE_OFF", "1"}});
    switches.dk_foliage = FoliageLow;
    REQUIRE(foliage(switches) == std::vector<Var>{{"EDEN_NX2_DK_FOLIAGE_DENSITY", "10"},
                                                  {"EDEN_NX2_DK_FOLIAGE_DISTANCE", "200"},
                                                  {"EDEN_NX2_DK_FOLIAGE_THORN_DENSITY", "50"}});
    switches.dk_foliage = FoliageMedium;
    REQUIRE(foliage(switches) == std::vector<Var>{{"EDEN_NX2_DK_FOLIAGE_DENSITY", "50"},
                                                  {"EDEN_NX2_DK_FOLIAGE_DISTANCE", "400"},
                                                  {"EDEN_NX2_DK_FOLIAGE_THORN_DENSITY", "75"}});
    switches.dk_foliage = FoliageHigh;
    REQUIRE(foliage(switches).empty());
    for (const Var& var : Wanted(Performance(), Game::Bananza)) {
        REQUIRE(std::find(Names.begin(), Names.end(), var.name) != Names.end());
    }

    // A game's own choice, stored and read back
    Overrides own;
    own.dk_foliage = FoliageMedium;
    std::map<std::string, int> stored;
    SaveOverrides(own, [&](std::string_view key, int value) { stored[std::string{key}] = value; });
    REQUIRE(stored.at("bananza_foliage") == FoliageMedium);
    REQUIRE(stored.at("bananza_foliage_off") == -1);
    REQUIRE(LoadOverrides([&](std::string_view key) { return stored.at(std::string{key}); }) ==
            own);
    // The box it replaces: on is Off, off High
    const auto old_box = [](int value) {
        return LoadOverrides([value](std::string_view key) {
                   return key == "bananza_foliage_off" ? value : -1;
               })
            .dk_foliage;
    };
    REQUIRE(old_box(1) == FoliageOff);
    REQUIRE(old_box(0) == FoliageHigh);
    REQUIRE_FALSE(old_box(-1));
}

TEST_CASE("Nx2PerfSettings: Bananza's shadow quality", "[core]") {
    const auto cascades = [](const std::vector<Var>& vars) -> std::string_view {
        for (const Var& var : vars) {
            if (var.name == "EDEN_NX2_DK_SHADOW_CASCADES") {
                return var.value;
            }
        }
        return {};
    };
    // Very high, the game's own, sets nothing.
    REQUIRE(cascades(Wanted(AllOff(), Game::Bananza)).empty());
    REQUIRE(cascades(Wanted(Performance(), Game::Bananza)) == "2");
    Overrides own;
    own.dk_shadow = ShadowLow;
    REQUIRE_FALSE(own.Empty());
    const Switches low = ForGame(Performance(), own);
    REQUIRE(low.dk_shadow == ShadowLow);
    REQUIRE(cascades(Wanted(low, Game::Bananza)) == "1");
    REQUIRE(cascades(Wanted(low, Game::MarioKartWorld)).empty());
    // Stored per game, read back; anything outside the steps is none.
    std::map<std::string, int> ini;
    SaveOverrides(own, [&](std::string_view key, int value) { ini[std::string{key}] = value; });
    REQUIRE(ini.at("bananza_shadow_quality") == 1);
    REQUIRE(LoadOverrides([&](std::string_view key) { return ini.at(std::string{key}); }) == own);
    ini["bananza_shadow_quality"] = 5;
    REQUIRE_FALSE(
        LoadOverrides([&](std::string_view key) { return ini.at(std::string{key}); }).dk_shadow);
}

TEST_CASE("Nx2PerfSettings: Bananza's reflection updates", "[core]") {
    const auto reflection_vars = [](const Switches& switches, Game game) {
        std::vector<Var> vars;
        for (const Var& var : Wanted(switches, game)) {
            if (var.name == "EDEN_NX2_DK_CUBEMAP_INTERVAL") {
                vars.push_back(var);
            }
        }
        return vars;
    };
    // Very low by default, High (the game's own, nothing set) in Safe.
    REQUIRE(reflection_vars(Performance(), Game::Bananza) ==
            std::vector<Var>{{"EDEN_NX2_DK_CUBEMAP_INTERVAL", "240"}});
    REQUIRE(reflection_vars(WithLevels(Performance(), BananzaQuality), Game::Bananza).empty());
    REQUIRE(reflection_vars(AllOff(), Game::Bananza).empty());
    REQUIRE(reflection_vars(Performance(), Game::MarioKartWorld).empty());
    REQUIRE(reflection_vars(Performance(), Game::Other).empty());
    // Low and Medium: an interval of 32 and 16 frames.
    Overrides own;
    own.dk_reflections = ReflectionsLow;
    REQUIRE_FALSE(own.Empty());
    REQUIRE(reflection_vars(ForGame(Performance(), own), Game::Bananza) ==
            std::vector<Var>{{"EDEN_NX2_DK_CUBEMAP_INTERVAL", "32"}});
    own.dk_reflections = ReflectionsMedium;
    REQUIRE(reflection_vars(ForGame(Performance(), own), Game::Bananza) ==
            std::vector<Var>{{"EDEN_NX2_DK_CUBEMAP_INTERVAL", "16"}});
    // Stored per game and read back; anything outside the steps is none.
    std::map<std::string, int> ini;
    SaveOverrides(own, [&](std::string_view key, int value) { ini[std::string{key}] = value; });
    REQUIRE(ini.at("bananza_reflection_updates") == ReflectionsMedium);
    const auto load = [&] {
        return LoadOverrides([&](std::string_view key) {
            const auto it = ini.find(std::string{key});
            return it == ini.end() ? -1 : it->second;
        });
    };
    REQUIRE(load() == own);
    ini["bananza_reflection_updates"] = 5;
    REQUIRE_FALSE(load().dk_reflections);
    // The box it replaces: ticked is Very low, unticked High; the step wins over it.
    ini["bananza_reflection_updates"] = -1;
    ini["bananza_reflections_off"] = 0;
    REQUIRE(load().dk_reflections == ReflectionsHigh);
    ini["bananza_reflections_off"] = 1;
    REQUIRE(load().dk_reflections == ReflectionsVeryLow);
    ini["bananza_reflection_updates"] = ReflectionsLow;
    REQUIRE(load().dk_reflections == ReflectionsLow);
}
