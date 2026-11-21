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
    {"EDEN_NX2_DK_SHADOW_CASCADES", "2"},
    {"EDEN_NX2_DK_CUBEMAP_INTERVAL", "240"},
    {"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"},
    {"EDEN_NX2_MODE_PACE", "2"},
    {"EDEN_NX2_PACE_SPIN_US", "1500"},
};
// Of those, the ones Bananza gets that another game does not.
constexpr std::size_t BalancedBananzaOwn = 8;

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
    "EDEN_NX2_DK_SHADOW_CASCADES",
    "EDEN_NX2_DK_CUBEMAP_INTERVAL",
};

// An ini group as the config sees it: the on / off keys and "preset".
struct Ini {
    std::map<std::string, bool> values;
    std::optional<int> preset;

    Switches Read() const {
        return Load(
            [this](std::string_view key, bool def) {
                const auto it = values.find(std::string{key});
                return it == values.end() ? def : it->second;
            },
            [this](std::string_view, int def) { return preset.value_or(def); });
    }
    void Write(const Switches& switches) {
        Save(
            switches,
            [this](std::string_view key, bool value, bool) { values[std::string{key}] = value; },
            [this](std::string_view key, int value, int) {
                REQUIRE(key == "preset");
                preset = value;
            });
    }
};

bool NoneOutside(std::string_view) {
    return false;
}

Switches Custom(Switches switches) {
    switches.preset = Preset::Custom;
    return switches;
}

} // Anonymous namespace

TEST_CASE("Nx2PerfSettings: the first start is the performance setup", "[core]") {
    const Ini empty;
    REQUIRE(empty.Read() == Performance());
    REQUIRE(empty.Read() == FromPreset(Preset::Performance, Performance()));
    REQUIRE(empty.Read().preset == Preset::Performance);
    REQUIRE(Wanted(Performance(), Game::Bananza) == BalancedBananza);
}

TEST_CASE("Nx2PerfSettings: every setting reads back what was written", "[core]") {
    for (const Key& key : Keys) {
        Switches switches = Performance();
        switches.*key.field = false;
        switches = AfterBoxChange(switches);
        REQUIRE(switches.preset == Preset::Custom);
        Ini ini;
        ini.Write(switches);
        REQUIRE(ini.values.size() == Keys.size() + GameKeys.size());
        REQUIRE(ini.Read() == switches);
        REQUIRE(ini.values.at(std::string{key.name}) == false);
    }
    // The game-specific settings are stored like the others, but are no part of any preset.
    for (const Key& key : GameKeys) {
        for (const Preset preset :
             {Preset::Quality, Preset::Performance, Preset::Reactive, Preset::Safe}) {
            Switches switches = FromPreset(preset, Performance());
            switches.*key.field = false;
            REQUIRE(AfterBoxChange(switches).preset == preset);
            REQUIRE(FromPreset(preset, switches).*key.field == false);
            REQUIRE(FromPreset(Preset::Custom, switches).*key.field == false);
            Ini ini;
            ini.Write(switches);
            REQUIRE(ini.values.at(std::string{key.name}) == false);
            REQUIRE(ini.Read() == switches);
        }
        // Missing from the file (before the setting existed): on.
        Ini old;
        old.Write(Performance());
        old.values.erase(std::string{key.name});
        REQUIRE(old.Read() == Performance());
    }
    for (const Preset preset :
         {Preset::Quality, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        Ini ini;
        ini.Write(FromPreset(preset, Performance()));
        REQUIRE(ini.Read() == FromPreset(preset, Performance()));
    }
}

TEST_CASE("Nx2PerfSettings: a stored Performance from before read traps were on in it", "[core]") {
    // Performance had read traps off: such a file is Performance with them on, not Custom.
    Ini old;
    old.Write(Performance());
    old.values["read_traps"] = false;
    REQUIRE(old.Read() == Performance());
    REQUIRE(old.Read().read_traps);
    // A stored Balanced (0) is Quality now; read traps as stored.
    Ini balanced;
    balanced.Write(Performance());
    balanced.preset = 0;
    REQUIRE(balanced.Read().preset == Preset::Quality);
}

TEST_CASE("Nx2PerfSettings: presets set the launchers' boxes", "[core]") {
    for (const Preset preset : {Preset::Quality, Preset::Performance, Preset::Reactive}) {
        const Switches switches = FromPreset(preset, AllOff());
        REQUIRE(switches.preset == preset);
        for (const Key& key : Keys) {
            const bool on = key.name != "read_traps" || preset != Preset::Reactive;
            REQUIRE(switches.*key.field == on);
        }
    }
    REQUIRE(FromPreset(Preset::Quality, AllOff()).read_traps);
    REQUIRE(FromPreset(Preset::Performance, AllOff()).read_traps);
    REQUIRE(!FromPreset(Preset::Reactive, AllOff()).read_traps);
    const Switches safe = FromPreset(Preset::Safe, Performance());
    REQUIRE(safe == AllOff());
    for (const Key& key : Keys) {
        REQUIRE(!(safe.*key.field));
    }
    // Custom keeps the boxes as they are.
    Switches mine = Performance();
    mine.osd = false;
    REQUIRE(FromPreset(Preset::Custom, mine) == Custom(mine));
    // Reactive Flushing: Balanced and Performance off, Experimental (Reactive) on, Safe and Custom
    // leave it.
    REQUIRE(ReactiveFlushing(Preset::Quality) == false);
    REQUIRE(ReactiveFlushing(Preset::Performance) == false);
    REQUIRE(ReactiveFlushing(Preset::Reactive) == true);
    REQUIRE(!ReactiveFlushing(Preset::Safe).has_value());
    REQUIRE(!ReactiveFlushing(Preset::Custom).has_value());
    // Barrier feedback loops: on with Reactive (it needs them), the others leave it.
    REQUIRE(BarrierFeedbackLoops(Preset::Reactive) == true);
    REQUIRE(!BarrierFeedbackLoops(Preset::Quality).has_value());
    REQUIRE(!BarrierFeedbackLoops(Preset::Safe).has_value());
    REQUIRE(!BarrierFeedbackLoops(Preset::Custom).has_value());
}

TEST_CASE("Nx2PerfSettings: a box changed by hand makes it Custom", "[core]") {
    for (const Preset preset :
         {Preset::Quality, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        for (const Key& key : Keys) {
            Switches switches = FromPreset(preset, Performance());
            REQUIRE(AfterBoxChange(switches).preset == preset);
            switches.*key.field = !(switches.*key.field);
            REQUIRE(AfterBoxChange(switches).preset == Preset::Custom);
            // Changed back: still the preset's boxes, so the preset stays (a later check).
            switches.*key.field = !(switches.*key.field);
            REQUIRE(AfterBoxChange(switches).preset == preset);
        }
    }
    // Custom stays Custom whatever the boxes.
    REQUIRE(AfterBoxChange(Custom(Performance())).preset == Preset::Custom);
    // A file edited by hand: boxes not the stored preset's read as Custom; a bad preset too.
    Ini ini;
    ini.Write(Performance());
    ini.values["osd"] = false;
    REQUIRE(ini.Read().preset == Preset::Custom);
    Ini bad;
    bad.preset = 9;
    REQUIRE(bad.Read().preset == Preset::Custom);
    // A file from before read_traps: the key is missing, the preset stays, and Performance / Experimental
    // / Safe keep read traps off.
    for (const Preset preset :
         {Preset::Quality, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        Ini old;
        old.Write(FromPreset(preset, Performance()));
        old.values.erase("read_traps");
        REQUIRE(old.Read() == FromPreset(preset, Performance()));
        // The same for a file from before read_trap_budget: Safe keeps it off.
        Ini before_budget;
        before_budget.Write(FromPreset(preset, Performance()));
        before_budget.values.erase("read_trap_budget");
        REQUIRE(before_budget.Read() == FromPreset(preset, Performance()));
    }
    Ini stored_performance;
    stored_performance.preset = static_cast<int>(Preset::Performance);
    REQUIRE(stored_performance.Read() == FromPreset(Preset::Performance, Performance()));
    REQUIRE(IndexOf(Preset::Performance) == 0);
    REQUIRE(IndexOf(Preset::Quality) == 1);
    REQUIRE(PresetOrder[static_cast<std::size_t>(IndexOf(Preset::Reactive))] == Preset::Reactive);
}

TEST_CASE("Nx2PerfSettings: presets set the launchers' variables", "[core]") {
    // Performance: start-performance.bat's, read traps on (unset).
    const auto performance = BalancedBananza;
    REQUIRE(Wanted(FromPreset(Preset::Performance, Performance()), Game::Bananza) == performance);
    // Experimental: Performance's, read traps off and RX_RT_FIT=3 (start-reactive-experimental.bat).
    auto reactive = performance;
    reactive.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    reactive.push_back({"EDEN_NX2_RX_RT_FIT", "3"});
    REQUIRE(Wanted(FromPreset(Preset::Reactive, Performance()), Game::Bananza) == reactive);
    // Safe: none of the boxes, read traps off, every per-draw shortcut =0 (start-safe.bat).
    // The game-specific settings are not Safe's to change: a game's own variables stay.
    for (const Game game : {Game::Bananza, Game::MarioKartWorld, Game::Other}) {
        const auto safe = Wanted(AllOff(), game);
        const auto own = GameVars(game);
        REQUIRE(safe.size() == own.size() + SafeOff.size() + 2);
        REQUIRE(std::vector<Var>(safe.begin(), safe.begin() + own.size()) == own);
        REQUIRE(safe[own.size()] == Var{"EDEN_NX2_READ_TRAPS", "0"});
        REQUIRE(safe[own.size() + 1] == Var{"EDEN_NX2_READ_TRAP_BUDGET_US", "0"});
        for (std::size_t i = 0; i < SafeOff.size(); ++i) {
            REQUIRE(safe[own.size() + 2 + i] == Var{SafeOff[i], "0"});
        }
    }
    // Custom with everything off sets read traps and their budget off only; Custom with the boxes
    // on: only the boxes' (read traps on sets nothing for that switch).
    auto only_traps = BananzaVars;
    only_traps.push_back({"EDEN_NX2_READ_TRAPS", "0"});
    only_traps.push_back({"EDEN_NX2_READ_TRAP_BUDGET_US", "0"});
    REQUIRE(Wanted(Custom(AllOff()), Game::Bananza) == only_traps);
    REQUIRE(Wanted(Custom(Performance()), Game::Bananza) == BalancedBananza);
}

TEST_CASE("Nx2PerfSettings: keys are distinct and cover every field", "[core]") {
    std::set<std::string_view> names;
    for (const Key& key : Keys) {
        names.insert(key.name);
        Switches one = AllOff();
        one.*key.field = true;
        REQUIRE(one != AllOff());
    }
    for (const Key& key : GameKeys) {
        names.insert(key.name);
        Switches one = Performance();
        one.*key.field = !(one.*key.field);
        REQUIRE(one != Performance());
    }
    REQUIRE(names.size() == Keys.size() + GameKeys.size());
    REQUIRE(names.count("preset") == 0);
    for (const Key& key : Keys) {
        REQUIRE(!(AllOff().*key.field));
        REQUIRE(Performance().*key.field);
    }
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
    for (const Preset preset :
         {Preset::Quality, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        for (const Var& var : Wanted(FromPreset(preset, Performance()), Game::Other)) {
            REQUIRE(BananzaOnly.count(var.name) == 0);
            REQUIRE(var.name != "EDEN_NX2_MKW_DYN");
        }
        for (const Var& var : Wanted(FromPreset(preset, Performance()), Game::MarioKartWorld)) {
            REQUIRE(BananzaOnly.count(var.name) == 0);
        }
        for (const Var& var : Wanted(FromPreset(preset, Performance()), Game::Bananza)) {
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
    REQUIRE(Plan(AfterBoxChange(off), Game::Bananza, is_outside) == plan);

    // Safe's shortcuts set outside: theirs too (READ_TRAPS and READ_TRAP_BUDGET_US =0 besides).
    const auto light = [](std::string_view name) { return name == "EDEN_NX2_LIGHT_DRAW"; };
    REQUIRE(Plan(AllOff(), Game::Bananza, light).size() == SafeOff.size() + 1 + BananzaVars.size());

    // Everything set outside: nothing to set.
    REQUIRE(Plan(Performance(), Game::Bananza, [](std::string_view) { return true; }).empty());
    REQUIRE(Plan(AllOff(), Game::Bananza, [](std::string_view) { return true; }).empty());
}

TEST_CASE("Nx2PerfSettings: every name is one a preset or a game's setting can set", "[core]") {
    std::set<std::string_view> wanted;
    for (const Preset preset :
         {Preset::Quality, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        for (const Game game : {Game::Bananza, Game::MarioKartWorld}) {
            for (const Var& var : Wanted(FromPreset(preset, Performance()), game)) {
                wanted.insert(var.name);
            }
        }
    }
    // Bananza's Low shadows, Medium reflection updates and foliage off are no preset's
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
    REQUIRE(Plan(AfterBoxChange(changed), Game::Bananza, NoneOutside) != first_bananza);
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
    REQUIRE(!NeedsRestart(applied, plan_now(Custom(Performance())))); // same boxes, same variables
    REQUIRE(NeedsRestart(applied, plan_now(AllOff())));
    REQUIRE(NeedsRestart(applied, plan_now(FromPreset(Preset::Quality, Performance()))));
    REQUIRE(NeedsRestart(applied, plan_now(FromPreset(Preset::Reactive, Performance()))));
    Switches no_osd = Performance();
    no_osd.osd = false;
    REQUIRE(NeedsRestart(applied, plan_now(AfterBoxChange(no_osd))));
    // A change that only touches variables set outside: nothing of ours differs, no restart.
    const auto osd_outside = [](std::string_view name) { return name == "EDEN_NX2_OSD"; };
    const std::optional<std::vector<Var>> applied_outside =
        Plan(Performance(), Game::Bananza, osd_outside);
    REQUIRE(!NeedsRestart(applied_outside, Plan(AfterBoxChange(no_osd), Game::Bananza, osd_outside)));
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
    // Presets leave it, Safe turns it off.
    REQUIRE(FrameGenFor(Preset::Safe) == FrameGen::Off);
    REQUIRE(!FrameGenFor(Preset::Quality).has_value());
    REQUIRE(!FrameGenFor(Preset::Performance).has_value());
    REQUIRE(!FrameGenFor(Preset::Reactive).has_value());
    REQUIRE(!FrameGenFor(Preset::Custom).has_value());
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
    REQUIRE(with_traps.preset == global.preset);
    REQUIRE(with_traps.osd == global.osd);

    // Read traps on leave EDEN_NX2_READ_TRAPS unset; off sets it to 0.
    const auto has_read_traps = [](const std::vector<Var>& vars) {
        for (const Var& var : vars) {
            if (var.name == "EDEN_NX2_READ_TRAPS") {
                return true;
            }
        }
        return false;
    };
    REQUIRE(has_read_traps(Wanted(global, Game::Bananza)));
    REQUIRE(!has_read_traps(Wanted(with_traps, Game::MarioKartWorld)));
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
    overrides.boxes[4] = false;
    overrides.boxes[KeyCount - 1] = true;
    overrides.preset = Preset::Safe;

    std::map<std::string, int> ini;
    SaveOverrides(overrides, [&](std::string_view key, int value) { ini[std::string{key}] = value; });
    // The boxes, the preset, the shadow quality, the reflection updates and the foliage, and the
    // boxes these two replace (cleared).
    REQUIRE(ini.size() == KeyCount + 6);
    REQUIRE(ini.at("bananza_shadow_quality") == -1);
    REQUIRE(ini.at("bananza_reflection_updates") == -1);
    REQUIRE(ini.at("bananza_reflections_off") == -1);
    REQUIRE(ini.at("preset") == static_cast<int>(Preset::Safe));
    REQUIRE(ini.at("read_traps") == 1);
    REQUIRE(ini.at("jit_same") == 0);
    REQUIRE(ini.at("acquire_peek") == -1);

    const auto read = [&](std::string_view key) {
        const auto it = ini.find(std::string{key});
        return it == ini.end() ? -1 : it->second;
    };
    REQUIRE(LoadOverrides(read) == overrides);

    // Missing keys and values that mean nothing: no override.
    REQUIRE(LoadOverrides([](std::string_view) { return -1; }).Empty());
    REQUIRE(LoadOverrides([](std::string_view) { return 7; }).Empty());
    // Custom is no preset to choose.
    Overrides custom;
    custom.preset = Preset::Custom;
    std::map<std::string, int> custom_ini;
    SaveOverrides(custom, [&](std::string_view key, int value) {
        custom_ini[std::string{key}] = value;
    });
    REQUIRE(LoadOverrides([&](std::string_view key) { return custom_ini.at(std::string{key}); })
                .Empty());
}

TEST_CASE("Nx2PerfSettings: Bananza starts from Performance, the others from the global preset",
          "[core]") {
    REQUIRE(Keys[0].name == "read_traps");
    // A first start is Performance, read traps on; so is a file with no stored choice.
    REQUIRE(FirstStart() == Performance());
    REQUIRE(FirstStart().preset == Preset::Performance);
    REQUIRE(FirstStart().read_traps);
    // Whatever the global preset is, Bananza is Performance: read traps on, foliage low.
    for (const Preset preset : {Preset::Performance, Preset::Quality, Preset::Reactive,
                                Preset::Safe, Preset::Custom}) {
        const Switches global = FromPreset(preset, Performance());
        REQUIRE(ForGame(global, Game::Bananza, {}) == FromPreset(Preset::Performance, global));
        REQUIRE(ForGame(global, Game::Bananza, {}).read_traps);
        REQUIRE(ForGame(global, Game::Bananza, {}).dk_foliage == FoliageLow);
        // Any other game, Mario Kart World too: the global settings as they are.
        REQUIRE(ForGame(global, Game::MarioKartWorld, {}) == global);
        REQUIRE(ForGame(global, Game::Other, {}) == global);
    }
    REQUIRE(ForGame(FirstStart(), Game::MarioKartWorld, {}).read_traps);
    // The game-specific boxes are the global ones.
    Switches global = Performance();
    global.dk_dyn = false;
    global.mkw_dyn = false;
    REQUIRE(!ForGame(global, Game::Bananza, {}).dk_dyn);
    REQUIRE(!ForGame(global, Game::MarioKartWorld, {}).mkw_dyn);

    // The game's own preset goes over its default, its boxes over the preset.
    Overrides own;
    own.preset = Preset::Safe;
    REQUIRE(ForGame(global, Game::Bananza, own).preset == Preset::Safe);
    own = {};
    own.boxes[0] = true;
    REQUIRE(ForGame(global, Game::Bananza, own).read_traps);
    REQUIRE(ForGame(global, Game::Bananza, own).preset == Preset::Performance);
    REQUIRE(ForGame(global, Game::Bananza, own).osd == global.osd);

    // What the game's variables say: neither sets READ_TRAPS=0 (Bananza does with Experimental).
    const auto has_read_traps = [](const std::vector<Var>& vars) {
        for (const Var& var : vars) {
            if (var.name == "EDEN_NX2_READ_TRAPS") {
                return true;
            }
        }
        return false;
    };
    REQUIRE(!has_read_traps(Wanted(ForGame(Performance(), Game::Bananza, {}), Game::Bananza)));
    Overrides reactive;
    reactive.preset = Preset::Reactive;
    REQUIRE(has_read_traps(Wanted(ForGame(Performance(), Game::Bananza, reactive), Game::Bananza)));
    REQUIRE(!has_read_traps(
        Wanted(ForGame(FirstStart(), Game::MarioKartWorld, {}), Game::MarioKartWorld)));
    // With the first start's preset the two games differ, so a session asks to restart between
    // them.
    const auto plan = [&](Game game) {
        return Plan(ForGame(FirstStart(), game, {}), game, NoneOutside);
    };
    REQUIRE(NeedsRestart(plan(Game::Bananza), plan(Game::MarioKartWorld)));
}

TEST_CASE("Nx2PerfSettings: a game with its own preset can follow the global one", "[core]") {
    const Switches global = Quality();
    REQUIRE(PresetFor(global.preset, Game::Bananza, {}) == Preset::Performance);
    REQUIRE(PresetFor(global.preset, Game::MarioKartWorld, {}) == global.preset);

    Overrides follow;
    follow.global_preset = true;
    REQUIRE(!follow.Empty());
    REQUIRE(PresetFor(global.preset, Game::Bananza, follow) == global.preset);
    REQUIRE(ForGame(global, Game::Bananza, follow) == global);
    REQUIRE(ForGame(global, Game::Bananza, follow).read_traps);
    // A game with no preset of its own follows it anyway.
    REQUIRE(ForGame(global, Game::Other, follow) == global);
    // Its own choice wins over following.
    follow.preset = Preset::Safe;
    REQUIRE(PresetFor(global.preset, Game::Bananza, follow) == Preset::Safe);

    // Stored as its own value, apart from a preset and from no choice.
    Overrides stored;
    stored.global_preset = true;
    int preset = 0;
    SaveOverrides(stored, [&](std::string_view key, int value) {
        if (key == "preset") {
            preset = value;
        }
    });
    REQUIRE(preset == FollowGlobalPreset);
    const Overrides loaded = LoadOverrides([&](std::string_view key) {
        return key == "preset" ? FollowGlobalPreset : -1;
    });
    REQUIRE(loaded == stored);
    REQUIRE(!loaded.preset);
}

TEST_CASE("Nx2PerfSettings: Bananza's foliage, shadows and reflections follow the preset", "[core]") {
    // Low foliage, static reflections and medium shadows in Performance and Experimental; the
    // game's own in Quality and Safe; Custom keeps them.
    for (const Preset preset : {Preset::Performance, Preset::Reactive}) {
        const Switches switches = FromPreset(preset, Quality());
        REQUIRE(switches.dk_foliage == FoliageLow);
        REQUIRE(switches.dk_reflections == ReflectionsVeryLow);
        REQUIRE(switches.dk_shadow == ShadowMedium);
    }
    for (const Preset preset : {Preset::Safe, Preset::Quality}) {
        const Switches switches = FromPreset(preset, Performance());
        REQUIRE(switches.dk_foliage == FoliageHigh);
        REQUIRE(switches.dk_reflections == ReflectionsHigh);
        REQUIRE(switches.dk_shadow == ShadowVeryHigh);
    }
    REQUIRE(FromPreset(Preset::Quality, Performance()).read_traps);
    REQUIRE(FromPreset(Preset::Custom, Quality()) .dk_shadow == ShadowVeryHigh);
    REQUIRE(FromPreset(Preset::Custom, Performance()).dk_foliage == FoliageLow);
    // Quality is Performance's boxes otherwise, and sets none of Bananza's variables.
    for (const Key& key : Keys) {
        REQUIRE(Quality().*key.field == Performance().*key.field);
    }
    REQUIRE_FALSE(ReactiveFlushing(Preset::Quality).value_or(true));
    auto quality = BalancedBananza;
    quality.erase(std::remove_if(quality.begin(), quality.end(),
                                 [](const Var& var) {
                                     return var.name == "EDEN_NX2_DK_FOLIAGE_DENSITY" ||
                                            var.name == "EDEN_NX2_DK_FOLIAGE_DISTANCE" ||
                                            var.name == "EDEN_NX2_DK_SHADOW_CASCADES" ||
                                            var.name == "EDEN_NX2_DK_CUBEMAP_INTERVAL";
                                 }),
                  quality.end());
    REQUIRE(Wanted(Quality(), Game::Bananza) == quality);
    // Only Bananza gets the variables.
    const Var off{"EDEN_NX2_DK_FOLIAGE_DENSITY", "10"};
    const auto has = [&](const std::vector<Var>& vars) {
        return std::find(vars.begin(), vars.end(), off) != vars.end();
    };
    REQUIRE(has(Wanted(Performance(), Game::Bananza)));
    REQUIRE_FALSE(has(Wanted(Performance(), Game::MarioKartWorld)));
    REQUIRE_FALSE(has(Wanted(Performance(), Game::Other)));
    REQUIRE_FALSE(has(Wanted(AllOff(), Game::Bananza)));
    // A game's own foliage or shadow quality goes over its preset's.
    Overrides own;
    own.dk_foliage = FoliageHigh;
    own.dk_shadow = ShadowLow;
    REQUIRE(ForGame(Performance(), Game::Bananza, own).dk_foliage == FoliageHigh);
    REQUIRE(ForGame(Performance(), Game::Bananza, own).dk_shadow == ShadowLow);
    // Bananza's own Quality preset, stored and read back; a stored 5 (the Quality preset of a
    // build that had a third one) is none.
    Overrides quality_preset;
    quality_preset.preset = Preset::Quality;
    std::map<std::string, int> stored;
    SaveOverrides(quality_preset,
                  [&](std::string_view key, int value) { stored[std::string{key}] = value; });
    REQUIRE(LoadOverrides([&](std::string_view key) { return stored.at(std::string{key}); }) ==
            quality_preset);
    REQUIRE(ForGame(Performance(), Game::Bananza, quality_preset) ==
            FromPreset(Preset::Quality, Performance()));
    REQUIRE_FALSE(LoadOverrides([](std::string_view key) { return key == "preset" ? 5 : -1; })
                      .preset);
    // A file from before the dropdown, with Bananza's foliage off box: the preset's.
    for (const Preset preset :
         {Preset::Quality, Preset::Performance, Preset::Reactive, Preset::Safe}) {
        Ini old;
        old.Write(FromPreset(preset, Performance()));
        old.values["bananza_foliage_off"] = true;
        REQUIRE(old.Read() == FromPreset(preset, Performance()));
    }
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
                                                  {"EDEN_NX2_DK_FOLIAGE_DISTANCE", "200"}});
    switches.dk_foliage = FoliageMedium;
    REQUIRE(foliage(switches) == std::vector<Var>{{"EDEN_NX2_DK_FOLIAGE_DENSITY", "50"},
                                                  {"EDEN_NX2_DK_FOLIAGE_DISTANCE", "400"}});
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
    const Switches low = ForGame(Performance(), Game::Bananza, own);
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
    // Very low in Performance and Experimental, High (the game's own, nothing set) in Quality and
    // Safe.
    REQUIRE(reflection_vars(Performance(), Game::Bananza) ==
            std::vector<Var>{{"EDEN_NX2_DK_CUBEMAP_INTERVAL", "240"}});
    REQUIRE(FromPreset(Preset::Reactive, Quality()).dk_reflections == ReflectionsVeryLow);
    REQUIRE(reflection_vars(Quality(), Game::Bananza).empty());
    REQUIRE(reflection_vars(AllOff(), Game::Bananza).empty());
    REQUIRE(reflection_vars(Performance(), Game::MarioKartWorld).empty());
    REQUIRE(reflection_vars(Performance(), Game::Other).empty());
    // Low and Medium: an interval of 32 and 16 frames.
    Overrides own;
    own.dk_reflections = ReflectionsLow;
    REQUIRE_FALSE(own.Empty());
    REQUIRE(reflection_vars(ForGame(Performance(), Game::Bananza, own), Game::Bananza) ==
            std::vector<Var>{{"EDEN_NX2_DK_CUBEMAP_INTERVAL", "32"}});
    own.dk_reflections = ReflectionsMedium;
    REQUIRE(reflection_vars(ForGame(Performance(), Game::Bananza, own), Game::Bananza) ==
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
