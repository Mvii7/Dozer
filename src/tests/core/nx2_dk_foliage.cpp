// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_foliage.h"

namespace {
using namespace Loader::Nx2DkFoliage;

struct Entry {
    std::string key;
    u8 type;
    u32 value; ///< the 4 value bytes; for a dictionary the index of its own entries
};

void Put32(std::vector<u8>& out, u32 value) {
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}

u32 Bits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, 4);
    return bits;
}

// A little endian BYML whose root dictionary has `root`; an entry of type 0xC1 refers to the
// dictionary `nested` (written after the root) by its `value` being ignored.
std::vector<u8> Byml(const std::vector<Entry>& root, const std::vector<Entry>& nested = {}) {
    std::vector<std::string> keys;
    const auto key_index = [&](const std::string& key) {
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return static_cast<u32>(i);
            }
        }
        keys.push_back(key);
        return static_cast<u32>(keys.size() - 1);
    };
    for (const auto& e : root) {
        key_index(e.key);
    }
    for (const auto& e : nested) {
        key_index(e.key);
    }
    std::vector<u8> out{'Y', 'B', 7, 0};
    Put32(out, 16);
    Put32(out, 0);
    Put32(out, 0); // the root offset, below
    Put32(out, 0xC2U | static_cast<u32>(keys.size()) << 8);
    u32 offset = 4 + 4 * static_cast<u32>(keys.size() + 1);
    std::vector<u8> strings;
    for (const auto& key : keys) {
        Put32(out, offset);
        offset += static_cast<u32>(key.size() + 1);
        strings.insert(strings.end(), key.begin(), key.end());
        strings.push_back(0);
    }
    Put32(out, offset);
    out.insert(out.end(), strings.begin(), strings.end());
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
    const u32 root_at = static_cast<u32>(out.size());
    const u32 nested_at = root_at + 4 + 8 * static_cast<u32>(root.size());
    Put32(out, 0xC1U | static_cast<u32>(root.size()) << 8);
    for (const auto& e : root) {
        Put32(out, key_index(e.key) | (static_cast<u32>(e.type) << 24));
        Put32(out, e.type == 0xC1 ? nested_at : e.value);
    }
    if (!nested.empty()) {
        Put32(out, 0xC1U | static_cast<u32>(nested.size()) << 8);
        for (const auto& e : nested) {
            Put32(out, key_index(e.key) | (static_cast<u32>(e.type) << 24));
            Put32(out, e.value);
        }
    }
    std::memcpy(out.data() + 12, &root_at, 4);
    return out;
}

// A SARC with one file
std::vector<u8> Sarc(const std::vector<u8>& file) {
    std::vector<u8> out{'S', 'A', 'R', 'C', 0x14, 0, 0xFF, 0xFE};
    Put32(out, 0); // the size, below
    Put32(out, 0x60);
    Put32(out, 0x100);
    for (const char c : {'S', 'F', 'A', 'T'}) {
        out.push_back(static_cast<u8>(c));
    }
    out.insert(out.end(), {0x0C, 0, 1, 0});
    Put32(out, 0x65);
    Put32(out, 0x1234);
    Put32(out, 0x01000000U);
    Put32(out, 0);
    Put32(out, static_cast<u32>(file.size()));
    out.resize(0x60, 0);
    out.insert(out.end(), file.begin(), file.end());
    const u32 size = static_cast<u32>(out.size());
    std::memcpy(out.data() + 8, &size, 4);
    return out;
}

u32 Read32(const std::vector<u8>& data, size_t at) {
    u32 value;
    std::memcpy(&value, data.data() + at, 4);
    return value;
}
} // Anonymous namespace

TEST_CASE("Nx2DkFoliage: foliage off", "[core]") {
    auto models = Sarc(Byml({{"FarClipDistance", 0xD2, Bits(200.0f)},
                             {"NearClipDistance", 0xD2, Bits(9.0f)}}));
    REQUIRE(PatchModelSettings(models));
    const size_t model = 0x60 + Read32(models, 12 + 0x60) + 4;
    REQUIRE(Read32(models, model + 4) == Bits(0.01f));
    REQUIRE(Read32(models, model + 8 + 4) == Bits(9.0f)); // the near clip is left alone

    auto system = Sarc(Byml({{"MaxModelsOutputForDeadAliveNum", 0xD1, 50000},
                             {"MaxModelsOutputForDrawNum", 0xD1, 25000},
                             {"MaxStaticModelsNum", 0xD1, 400000}}));
    REQUIRE(PatchSystemSettings(system));
    const size_t caps = 0x60 + Read32(system, 12 + 0x60) + 4;
    REQUIRE(Read32(system, caps + 4) == 1);
    REQUIRE(Read32(system, caps + 8 + 4) == 1);
    REQUIRE(Read32(system, caps + 16 + 4) == 400000);

    auto material = Sarc(Byml({{"DynamicPlacementProbability", 0xD2, Bits(100.0f)},
                               {"PlacedModelsMaxNum", 0xD1, 30},
                               {"PlacedModelsMinNum", 0xD1, 20}}));
    REQUIRE(PatchMaterialSettings(material));
    const size_t entries = 0x60 + Read32(material, 12 + 0x60) + 4;
    REQUIRE(Read32(material, entries + 4) == Bits(0.0f));
    REQUIRE(Read32(material, entries + 8 + 4) == 1);
    REQUIRE(Read32(material, entries + 16 + 4) == 1);
}

TEST_CASE("Nx2DkFoliage: data that is not an archive is left alone", "[core]") {
    std::vector<u8> junk(300, 0x5A);
    const auto before = junk;
    REQUIRE_FALSE(PatchSystemSettings(junk));
    REQUIRE_FALSE(PatchMaterialSettings(junk));
    REQUIRE_FALSE(PatchModelSettings(junk));
    REQUIRE(junk == before);
}

namespace {

// A big endian ProceduralPlacement-like BYML: root {ReferencedMaterials: ["Grass050",
// "Money000", "Thorn003"], Posture: [plants]}, each plant {X, Y, Z, SX, ReferencedMaterialIndex}.
struct BigPlant {
    float x, y, z;
    u32 material;
};

void PutBig(std::vector<u8>& out, u32 value) {
    out.insert(out.end(), {static_cast<u8>(value >> 24), static_cast<u8>(value >> 16),
                           static_cast<u8>(value >> 8), static_cast<u8>(value)});
}

u32 ReadBig(const std::vector<u8>& data, size_t at) {
    return u32{data[at]} << 24 | u32{data[at + 1]} << 16 | u32{data[at + 2]} << 8 | data[at + 3];
}

void PutTable(std::vector<u8>& out, const std::vector<std::string>& names) {
    PutBig(out, 0xC2000000U | static_cast<u32>(names.size()));
    u32 offset = 4 + 4 * static_cast<u32>(names.size() + 1);
    for (const auto& name : names) {
        PutBig(out, offset);
        offset += static_cast<u32>(name.size() + 1);
    }
    PutBig(out, offset);
    for (const auto& name : names) {
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(0);
    }
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
}

// Keys sorted as a BYML wants them
const std::vector<std::string> PlacementKeys{"Posture", "ReferencedMaterialIndex",
                                             "ReferencedMaterials", "SX", "X", "Y", "Z"};

std::vector<u8> Placement(const std::vector<BigPlant>& plants) {
    std::vector<u8> out{'B', 'Y', 0, 4};
    PutBig(out, 16);
    PutBig(out, 0); // strings, below
    PutBig(out, 0); // root, below
    PutTable(out, PlacementKeys);
    const u32 strings = static_cast<u32>(out.size());
    PutTable(out, {"Grass050", "Money000", "Thorn003"});
    const auto key = [](std::string_view name) {
        for (size_t i = 0; i < PlacementKeys.size(); ++i) {
            if (PlacementKeys[i] == name) {
                return static_cast<u32>(i) << 8;
            }
        }
        return 0U;
    };
    const u32 root = static_cast<u32>(out.size());
    const u32 materials = root + 4 + 2 * 8;
    const u32 postures = materials + 4 + 4 + 3 * 4;
    const u32 first_plant = postures + 4 + (static_cast<u32>(plants.size()) + 3) / 4 * 4 +
                            4 * static_cast<u32>(plants.size());
    PutBig(out, 0xC1000002U);
    PutBig(out, key("Posture") | 0xC0);
    PutBig(out, postures);
    PutBig(out, key("ReferencedMaterials") | 0xC0);
    PutBig(out, materials);
    PutBig(out, 0xC0000003U);
    out.insert(out.end(), {0xA0, 0xA0, 0xA0, 0});
    PutBig(out, 0);
    PutBig(out, 1);
    PutBig(out, 2);
    PutBig(out, 0xC0000000U | static_cast<u32>(plants.size()));
    for (size_t i = 0; i < plants.size(); ++i) {
        out.push_back(0xC1);
    }
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
    constexpr u32 PlantSize = 4 + 5 * 8;
    for (size_t i = 0; i < plants.size(); ++i) {
        PutBig(out, first_plant + static_cast<u32>(i) * PlantSize);
    }
    for (const auto& plant : plants) {
        PutBig(out, 0xC1000005U);
        PutBig(out, key("ReferencedMaterialIndex") | 0xD1);
        PutBig(out, plant.material);
        PutBig(out, key("SX") | 0xD2);
        PutBig(out, Bits(1.0f));
        PutBig(out, key("X") | 0xD2);
        PutBig(out, Bits(plant.x));
        PutBig(out, key("Y") | 0xD2);
        PutBig(out, Bits(plant.y));
        PutBig(out, key("Z") | 0xD2);
        PutBig(out, Bits(plant.z));
    }
    out[8] = static_cast<u8>(strings >> 24);
    out[9] = static_cast<u8>(strings >> 16);
    out[10] = static_cast<u8>(strings >> 8);
    out[11] = static_cast<u8>(strings);
    out[12] = static_cast<u8>(root >> 24);
    out[13] = static_cast<u8>(root >> 16);
    out[14] = static_cast<u8>(root >> 8);
    out[15] = static_cast<u8>(root);
    return out;
}

} // namespace

TEST_CASE("Nx2DkFoliage: the foliage placed in a stage thinned in patches", "[core]") {
    std::vector<BigPlant> plants;
    for (int i = 0; i < 100; ++i) {
        plants.push_back({static_cast<float>(i % 10) * 2.5f, 5.0f,
                          static_cast<float>(i / 10) * 2.5f, i < 90 ? 0U : 1U});
    }
    plants.push_back({std::numeric_limits<float>::quiet_NaN(), 5.0f, 0.0f, 0U}); // left alone
    const auto game = Placement(plants);
    auto file = game;
    Levels levels;
    REQUIRE_FALSE(PlacementsWanted(levels));
    REQUIRE(PatchPlacement(file, levels) == 0);
    REQUIRE(file == game);
    levels.density = 40;
    REQUIRE(PlacementsWanted(levels));
    REQUIRE(PatchPlacement(file, levels) == 54); // 60% of the 90 on grass
    REQUIRE(file.size() == game.size());

    const size_t first = file.size() - plants.size() * 44;
    size_t moved = 0;
    for (size_t i = 0; i < plants.size(); ++i) {
        const size_t plant = first + i * 44;
        const float y = Detail::AsFloat(ReadBig(file, plant + 4 + 3 * 8 + 4));
        if (y < -5000.0f) {
            ++moved;
            REQUIRE(i < 90); // the gold is kept
            REQUIRE(ReadBig(file, plant + 4 + 8 + 4) == Bits(0.0f));
        } else {
            REQUIRE(y == 5.0f);
            REQUIRE(ReadBig(file, plant + 4 + 8 + 4) == Bits(1.0f));
        }
    }
    REQUIRE(moved == 54);
}

TEST_CASE("Nx2DkFoliage: the thorns thinned with their own density", "[core]") {
    std::vector<BigPlant> plants;
    for (int i = 0; i < 100; ++i) {
        plants.push_back({static_cast<float>(i % 10) * 2.5f, 5.0f,
                          static_cast<float>(i / 10) * 2.5f, i < 50 ? 0U : 2U});
    }
    auto file = Placement(plants);
    // 10% of the 50 on grass kept, 30% of the 50 thorns
    REQUIRE(PatchPlacement(file, Levels{10, 0, 30}) == 45 + 35);
    const size_t first = file.size() - plants.size() * 44;
    size_t thorns = 0;
    for (size_t i = 50; i < plants.size(); ++i) {
        if (Detail::AsFloat(ReadBig(file, first + i * 44 + 4 + 3 * 8 + 4)) == 5.0f) {
            ++thorns;
        }
    }
    REQUIRE(thorns == 15);
    // Thorns at 100%: only the grass
    auto kept = Placement(plants);
    REQUIRE(PatchPlacement(kept, Levels{10, 0, 100}) == 45);
    // Unset, the thorns follow the density
    REQUIRE(Levels{10, 0, 0}.Kept(true) == 10);
    REQUIRE(Levels{0, 0, 30}.Kept(false) == 100);
    REQUIRE(Levels{0, 0, 30}.Active());
    REQUIRE_FALSE(Levels{0, 0, 100}.Active());
    REQUIRE(IsThorn("Thorn003"));
    REQUIRE(IsThorn("Work/Voxel/VoxelMaterial/Thorn003.al__VoxelMaterial.gyml"));
    REQUIRE_FALSE(IsThorn("Work/Voxel/VoxelMaterial/Grass050.al__VoxelMaterial.gyml"));
    REQUIRE_FALSE(IsThorn("TestThorn000"));
    REQUIRE(IsGold("Money000"));
    REQUIRE(IsGold("Work/Voxel/VoxelMaterial/Money000.al__VoxelMaterial.gyml"));
    REQUIRE_FALSE(IsGold("Grass050"));
}

TEST_CASE("Nx2DkFoliage: the levels' variables", "[core]") {
    REQUIRE(LevelFrom(nullptr, MinDensity, MaxDensity) == 0);
    REQUIRE(LevelFrom("", MinDensity, MaxDensity) == 0);
    REQUIRE(LevelFrom("10", MinDensity, MaxDensity) == 10);
    REQUIRE(LevelFrom("100", MinDensity, MaxDensity) == 0); // the game's own: nothing to do
    REQUIRE(LevelFrom("0", MinDensity, MaxDensity) == 0);
    REQUIRE(LevelFrom("200", MinDistance, MaxDistance) == 200);
    REQUIRE(LevelFrom("10", MinDistance, MaxDistance) == 0);
    REQUIRE(LevelFrom("2x", MinDistance, MaxDistance) == 0);
    REQUIRE(LevelFrom("99999999999", MinDistance, MaxDistance) == 0);
    REQUIRE_FALSE(Levels{}.Active());
    REQUIRE(Levels{0, 200}.Active());
}

TEST_CASE("Nx2DkFoliage: the levels in the settings files", "[core]") {
    const Levels levels{10, 200};
    auto models = Sarc(Byml({{"FarClipDistance", 0xD2, Bits(500.0f)},
                             {"NearClipDistance", 0xD2, Bits(9.0f)}}));
    REQUIRE_FALSE(PatchModelSettings(models, Levels{10, 0}));
    REQUIRE(PatchModelSettings(models, levels));
    const size_t model = 0x60 + Read32(models, 12 + 0x60) + 4;
    REQUIRE(Read32(models, model + 4) == Bits(200.0f));
    REQUIRE(Read32(models, model + 8 + 4) == Bits(9.0f));
    // A model drawn closer than the distance keeps its own
    auto near = Sarc(Byml({{"FarClipDistance", 0xD2, Bits(150.0f)}}));
    REQUIRE_FALSE(PatchModelSettings(near, levels));

    auto system = Sarc(Byml({{"DynamicPlacementsPerFrameMax", 0xD1, 128},
                             {"MaxModelsOutputForDrawNum", 0xD1, 25000}}));
    REQUIRE_FALSE(PatchSystemSettings(system, Levels{0, 200}));
    REQUIRE(PatchSystemSettings(system, levels));
    const size_t caps = 0x60 + Read32(system, 12 + 0x60) + 4;
    REQUIRE(Read32(system, caps + 4) == 12);
    REQUIRE(Read32(system, caps + 8 + 4) == 25000); // the caps are foliage off's

    auto material = Sarc(Byml({{"DynamicPlacementProbability", 0xD2, Bits(50.0f)},
                               {"PlacedModelsMaxNum", 0xD1, 30},
                               {"PlacedModelsMinNum", 0xD1, 5}}));
    REQUIRE(PatchMaterialSettings(material, levels));
    const size_t entries = 0x60 + Read32(material, 12 + 0x60) + 4;
    REQUIRE(Read32(material, entries + 4) == Bits(5.0f));
    REQUIRE(Read32(material, entries + 8 + 4) == 3);
    REQUIRE(Read32(material, entries + 16 + 4) == 1);
    // A 0 stays 0
    auto none = Sarc(Byml({{"PlacedModelsMinNum", 0xD1, 0}}));
    REQUIRE(PatchMaterialSettings(none, levels));
    REQUIRE(Read32(none, 0x60 + Read32(none, 12 + 0x60) + 4 + 4) == 0);
}

TEST_CASE("Nx2DkFoliage: the code's default draw distance", "[core]") {
    // 0x1000 bytes of text, then the read-only data: the field's name, then its default
    constexpr u32 TextSize = 0xF00, AlignedSize = 0x1000;
    constexpr size_t NameAt = AlignedSize + 0x10, DefaultAt = AlignedSize + 0x40, Site = 0x100;
    std::vector<u32> words(0x2000 / 4, 0);
    const auto bytes = [&] {
        return std::span<u8>{reinterpret_cast<u8*>(words.data()), words.size() * 4};
    };
    const auto adrp_add = [&](size_t at, u32 reg, size_t target) {
        const u32 pages = static_cast<u32>((target >> 12) - ((at * 4) >> 12));
        words[at] = 0x90000000U | (pages & 3) << 29 | (pages >> 2 & 0x7FFFF) << 5 | reg;
        words[at + 1] = 0x91000000U | static_cast<u32>(target & 0xFFF) << 10 | reg << 5 | reg;
    };
    std::memcpy(bytes().data() + NameAt, "FarClipDistance", sizeof("FarClipDistance"));
    const float game = 100.0f;
    std::memcpy(bytes().data() + DefaultAt, &game, sizeof(game));
    adrp_add(Site, 9, NameAt);
    adrp_add(Site + 7, 8, DefaultAt);
    const auto now = [&] {
        float value;
        std::memcpy(&value, bytes().data() + DefaultAt, sizeof(value));
        return value;
    };

    REQUIRE_FALSE(PatchDefaultDistance(bytes(), TextSize, AlignedSize, Levels{10, 0}));
    REQUIRE_FALSE(PatchDefaultDistance(bytes(), TextSize, AlignedSize, Levels{10, 200}));
    REQUIRE(now() == 100.0f); // already closer
    // Another load of the same word (a 100.0f the linker shares): left alone
    const auto saved = words;
    words[0x200] = 0x90000000U | 0x20000000U | 0x3; // adrp x3, the next page
    words[0x201] = 0xBD400060U | (0x40 / 4) << 10;  // ldr s0, [x3, #0x40]
    REQUIRE_FALSE(PatchDefaultDistance(bytes(), TextSize, AlignedSize, Levels{10, 40}));
    REQUIRE(now() == 100.0f);
    words = saved;
    REQUIRE(PatchDefaultDistance(bytes(), TextSize, AlignedSize, Levels{10, 40}));
    REQUIRE(now() == 40.0f);
    // Not the game's default there any more: left alone
    REQUIRE_FALSE(PatchDefaultDistance(bytes(), TextSize, AlignedSize, Levels{10, 30}));
    REQUIRE(now() == 40.0f);
}
