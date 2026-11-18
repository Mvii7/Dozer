// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
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

TEST_CASE("Nx2DkFoliage: the caps and the frustum of the system settings", "[core]") {
    auto archive = Sarc(Byml({{"DynamicPlacementsPerFrameMax", 0xD1, 128},
                              {"HorizontalFrustumExpansionPctg", 0xD2, Bits(50.0f)},
                              {"MaxModelsOutputForDeadAliveNum", 0xD1, 50000},
                              {"MaxModelsOutputForDrawNum", 0xD1, 25000},
                              {"MaxStaticModelsNum", 0xD1, 400000},
                              {"VerticalFrustumExpansionPctg", 0xD2, Bits(50.0f)}}));
    const auto before = archive;
    Settings settings;
    REQUIRE_FALSE(settings.Active());
    REQUIRE_FALSE(PatchSystemSettings(archive, settings));
    REQUIRE(archive == before);

    settings.cap = 40;
    settings.frustum_off = true;
    settings.density = 25;
    REQUIRE(PatchSystemSettings(archive, settings));
    size_t changed = 0;
    for (size_t i = 0; i + 4 <= archive.size(); i += 4) {
        changed += Read32(archive, i) != Read32(before, i) ? 1 : 0;
    }
    REQUIRE(changed == 5); // the static model count is left alone
    const size_t root = 0x60 + Read32(before, 12 + 0x60) + 4;
    REQUIRE(Read32(archive, root + 4 + 8 * 0) == 32);          // 128 * 25 %
    REQUIRE(Read32(archive, root + 4 + 8 * 1) == Bits(0.0f));  // the horizontal expansion
    REQUIRE(Read32(archive, root + 4 + 8 * 2) == 20000);       // 50000 * 40 %
    REQUIRE(Read32(archive, root + 4 + 8 * 3) == 10000);       // 25000 * 40 %
    REQUIRE(Read32(archive, root + 4 + 8 * 4) == 400000);
    REQUIRE(Read32(archive, root + 4 + 8 * 5) == Bits(0.0f));  // the vertical expansion
}

TEST_CASE("Nx2DkFoliage: the placement chance of the material settings", "[core]") {
    auto archive = Sarc(Byml({{"DynamicSetting", 0xC1, 0}, {"MaterialName", 0xD1, 7}},
                             {{"DynamicPlacementProbability", 0xD2, Bits(1.0f)}}));
    Settings settings;
    settings.cap = 50;
    REQUIRE_FALSE(PatchMaterialSettings(archive, settings)); // only the density does it
    settings.density = 50;
    REQUIRE(PatchMaterialSettings(archive, settings));
    const size_t root = 0x60 + Read32(archive, 12 + 0x60) + 4;
    const size_t nested = root + 8 * 2; // the header of the nested dictionary
    REQUIRE(Read32(archive, nested + 8) == Bits(0.5f));
    REQUIRE(Read32(archive, root + 8 + 4) == 7);
}

TEST_CASE("Nx2DkFoliage: the effect caps", "[core]") {
    auto archive = Sarc(Byml({{"EmitCountPerFrameMax", 0xD1, 156},
                              {"ELinkMaxUsersNum", 0xD1, 5120},
                              {"MaxMatEffActiveUsersNumPerSlot", 0xD1, 10},
                              {"OneEmitterMaxParticleNum", 0xD1, 512}}));
    Settings settings;
    settings.emit = 50;
    settings.particles = 25;
    settings.material = 30;
    REQUIRE(PatchEffectSettings(archive, settings));
    const size_t root = 0x60 + Read32(archive, 12 + 0x60) + 4;
    REQUIRE(Read32(archive, root + 4) == 78);
    REQUIRE(Read32(archive, root + 8 + 4) == 5120); // the pool size is not touched
    REQUIRE(Read32(archive, root + 16 + 4) == 3);
    REQUIRE(Read32(archive, root + 24 + 4) == 128);
    Settings none;
    REQUIRE_FALSE(PatchEffectSettings(archive, none));
}

TEST_CASE("Nx2DkFoliage: the material effect caps", "[core]") {
    auto archive = Sarc(Byml({{"MaxActiveEffectNum", 0xD1, 256}, {"InfluenceRadius", 0xD2, Bits(30.0f)}}));
    Settings settings;
    settings.alive = 50;
    REQUIRE_FALSE(PatchSettingUnits(archive, settings));
    settings.material = 50;
    REQUIRE(PatchSettingUnits(archive, settings));
    const size_t root = 0x60 + Read32(archive, 12 + 0x60) + 4;
    REQUIRE(Read32(archive, root + 4) == 128);
    REQUIRE(Read32(archive, root + 8 + 4) == Bits(30.0f));
}

TEST_CASE("Nx2DkFoliage: the models' far clip and LOD thresholds", "[core]") {
    auto archive = Sarc(Byml({{"FarClipDistance", 0xD2, Bits(200.0f)},
                              {"LodThreshold0", 0xD2, Bits(0.1f)},
                              {"NearClipDistance", 0xD2, Bits(9.0f)}}));
    Settings settings;
    REQUIRE_FALSE(PatchModelSettings(archive, settings));
    settings.far_clip = 50;
    REQUIRE(PatchModelSettings(archive, settings));
    const size_t root = 0x60 + Read32(archive, 12 + 0x60) + 4;
    REQUIRE(Read32(archive, root + 4) == Bits(100.0f));
    REQUIRE(Read32(archive, root + 8 + 4) == Bits(0.1f)); // the LOD row is not set
    settings.lod = 200;
    REQUIRE(PatchModelSettings(archive, settings));
    REQUIRE(Read32(archive, root + 8 + 4) == Bits(0.1f * 2.0f));
    REQUIRE(Read32(archive, root + 16 + 4) == Bits(9.0f)); // the near clip is left alone
}

TEST_CASE("Nx2DkFoliage: the number of placed models", "[core]") {
    auto archive = Sarc(Byml({{"PlacedModelsMaxNum", 0xD1, 30}, {"PlacedModelsMinNum", 0xD1, 20}}));
    Settings settings;
    settings.density = 50;
    REQUIRE(PatchMaterialSettings(archive, settings));
    const size_t root = 0x60 + Read32(archive, 12 + 0x60) + 4;
    REQUIRE(Read32(archive, root + 4) == 15);
    REQUIRE(Read32(archive, root + 8 + 4) == 10);
}

TEST_CASE("Nx2DkFoliage: foliage off", "[core]") {
    Settings settings;
    settings.off = true;
    REQUIRE(settings.Active());
    auto models = Sarc(Byml({{"FarClipDistance", 0xD2, Bits(200.0f)}}));
    REQUIRE(PatchModelSettings(models, settings));
    const size_t root = 0x60 + Read32(models, 12 + 0x60) + 4;
    REQUIRE(Read32(models, root + 4) == Bits(0.01f));
    auto system = Sarc(Byml({{"MaxModelsOutputForDrawNum", 0xD1, 25000}}));
    REQUIRE(PatchSystemSettings(system, settings));
    REQUIRE(Read32(system, 0x60 + Read32(system, 12 + 0x60) + 4 + 4) == 1);
    auto material = Sarc(Byml({{"DynamicPlacementProbability", 0xD2, Bits(100.0f)},
                               {"PlacedModelsMaxNum", 0xD1, 30}}));
    REQUIRE(PatchMaterialSettings(material, settings));
    const size_t entries = 0x60 + Read32(material, 12 + 0x60) + 4;
    REQUIRE(Read32(material, entries + 4) == Bits(0.0f));
    REQUIRE(Read32(material, entries + 8 + 4) == 1);
}

TEST_CASE("Nx2DkFoliage: data that is not an archive is left alone", "[core]") {
    std::vector<u8> junk(300, 0x5A);
    const auto before = junk;
    Settings settings;
    settings.cap = 50;
    settings.frustum_off = true;
    settings.density = 50;
    REQUIRE_FALSE(PatchSystemSettings(junk, settings));
    REQUIRE_FALSE(PatchMaterialSettings(junk, settings));
    REQUIRE(junk == before);
    std::vector<u8> empty;
    REQUIRE_FALSE(PatchSystemSettings(empty, settings));
}

TEST_CASE("Nx2DkFoliage: effect caps off", "[core]") {
    Settings settings;
    settings.emit_off = true;
    settings.material_off = true;
    REQUIRE(settings.Active());
    auto effects = Sarc(Byml({{"EmitCountPerFrameMax", 0xD1, 156}}));
    REQUIRE(PatchEffectSettings(effects, settings));
    REQUIRE(Read32(effects, 0x60 + Read32(effects, 12 + 0x60) + 4 + 4) == 1);
    auto units = Sarc(Byml({{"MaxActiveEffectNum", 0xD1, 8}}));
    REQUIRE(PatchSettingUnits(units, settings));
    REQUIRE(Read32(units, 0x60 + Read32(units, 12 + 0x60) + 4 + 4) == 1);
}

TEST_CASE("Nx2DkFoliage: voxel effect rows", "[core]") {
    auto effects = Sarc(Byml({{"BufferSize", 0xD1, 1024}, {"IsUseDepthCollisionEffect", 0xD0, 1}}));
    Settings settings;
    settings.voxel_emitter = 25;
    settings.depth_collision_off = true;
    REQUIRE(settings.Active());
    REQUIRE(PatchEffectSettings(effects, settings));
    const size_t root = 0x60 + Read32(effects, 12 + 0x60) + 4;
    REQUIRE(Read32(effects, root + 4) == 256);
    REQUIRE(Read32(effects, root + 8 + 4) == 0);
}

TEST_CASE("Nx2DkFoliage: the object LOD thresholds", "[core]") {
    auto archive = Sarc(Byml({{"ModelLodThresholdFirst", 0xD2, Bits(0.25f)},
                              {"ModelLodThresholdSecond", 0xD2, Bits(0.125f)},
                              {"ShadowLodOffset", 0xD2, Bits(1.0f)}}));
    Settings settings;
    REQUIRE_FALSE(PatchGraphicsParam(archive, settings));
    settings.object_lod = 200;
    REQUIRE(settings.Active());
    REQUIRE(PatchGraphicsParam(archive, settings));
    const size_t root = 0x60 + Read32(archive, 12 + 0x60) + 4;
    REQUIRE(Read32(archive, root + 4) == Bits(0.5f));
    REQUIRE(Read32(archive, root + 8 + 4) == Bits(0.25f));
    REQUIRE(Read32(archive, root + 16 + 4) == Bits(1.0f)); // the others are left alone
}
