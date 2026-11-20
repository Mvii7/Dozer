// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// Donkey Kong Bananza's foliage ("Clustered Rendering") settings are game data: the caps are in
// ClusteredRendering/SystemSettings, the chance of a dynamic placement in each material of
// ClusteredRendering/MaterialSettings and each model's draw distance in
// ClusteredRendering/ModelSettings (zstd-compressed SARC archives of BYML files). Foliage off and
// the levels below change those values in the archives as they are read from the RomFS
// (file_sys/patch_manager.cpp).
namespace Loader::Nx2DkFoliage {

/// EDEN_NX2_DK_FOLIAGE_OFF=1 (Bananza's Foliage off box, on in the Performance and Experimental
/// presets).
constexpr const char* OffSwitch = "EDEN_NX2_DK_FOLIAGE_OFF";

inline bool OffFromEnvironment() {
    const char* const value = std::getenv(OffSwitch);
    return value != nullptr && std::string_view{value} == "1";
}

constexpr std::string_view SystemSettingsPath = "ClusteredRendering/SystemSettings.alsarc.zs";
constexpr std::string_view ModelSettingsPath = "ClusteredRendering/ModelSettings.alsarc.zs";
constexpr std::string_view MaterialSettingsPath = "ClusteredRendering/MaterialSettings.alsarc.zs";

namespace Detail {

constexpr u8 TypeArray = 0xC0, TypeDict = 0xC1, TypeFloat = 0xD2, TypeInt = 0xD1, TypeUInt = 0xD3;

struct Reader {
    std::span<u8> data;

    [[nodiscard]] bool Has(size_t offset, size_t size) const {
        return offset <= data.size() && size <= data.size() - offset;
    }
    [[nodiscard]] u32 U32(size_t offset) const {
        u32 value = 0;
        if (Has(offset, 4)) {
            std::memcpy(&value, data.data() + offset, 4);
        }
        return value;
    }
    [[nodiscard]] u16 U16(size_t offset) const {
        u16 value = 0;
        if (Has(offset, 2)) {
            std::memcpy(&value, data.data() + offset, 2);
        }
        return value;
    }
};

/// Calls `visit(key, type, offset of the 4 value bytes)` for each entry of each dictionary of
/// a little endian BYML that starts at `base`. Entries are visited only up to `Budget` nodes.
template <typename Visit>
void WalkByml(const Reader& in, size_t base, Visit&& visit) {
    if (!in.Has(base, 16) || in.data[base] != 'Y' || in.data[base + 1] != 'B') {
        return;
    }
    const u32 key_table = in.U32(base + 4);
    const u32 root = in.U32(base + 12);
    if (key_table == 0 || root == 0) {
        return;
    }
    const size_t key_base = base + key_table;
    const u32 key_count = in.U32(key_base) >> 8;
    const auto key_at = [&](u32 index) -> std::string_view {
        if (index >= key_count) {
            return {};
        }
        const size_t start = key_base + in.U32(key_base + 4 + size_t{index} * 4);
        size_t end = start;
        while (in.Has(end, 1) && in.data[end] != 0) {
            ++end;
        }
        return in.Has(start, end - start)
                   ? std::string_view{reinterpret_cast<const char*>(in.data.data()) + start,
                                      end - start}
                   : std::string_view{};
    };

    size_t budget = 1u << 20;
    const auto walk = [&](auto&& self, size_t node, int depth) -> void {
        if (depth > 16 || budget == 0 || !in.Has(base + node, 4)) {
            return;
        }
        --budget;
        const u8 type = in.data[base + node];
        const u32 count = in.U32(base + node) >> 8;
        if (type == TypeDict) {
            for (u32 i = 0; i < count && in.Has(base + node + 4 + size_t{i} * 8, 8); ++i) {
                const size_t entry = base + node + 4 + size_t{i} * 8;
                const u32 head = in.U32(entry);
                const u8 entry_type = static_cast<u8>(head >> 24);
                visit(key_at(head & 0xFFFFFF), entry_type, entry + 4);
                if (entry_type == TypeDict || entry_type == TypeArray) {
                    self(self, in.U32(entry + 4), depth + 1);
                }
            }
        } else if (type == TypeArray) {
            const size_t values = base + node + 4 + (size_t{count} + 3) / 4 * 4;
            for (u32 i = 0; i < count && in.Has(base + node + 4 + i, 1); ++i) {
                const u8 element = in.data[base + node + 4 + i];
                if (element == TypeDict || element == TypeArray) {
                    self(self, in.U32(values + size_t{i} * 4), depth + 1);
                }
            }
        }
    };
    walk(walk, root, 0);
}

/// Calls `visit(in, offset of the file's data in the archive)` for each file of a SARC.
template <typename Visit>
void ForEachSarcFile(const Reader& in, Visit&& visit) {
    if (!in.Has(0, 0x20) || std::memcmp(in.data.data(), "SARC", 4) != 0 ||
        in.U16(6) != 0xFEFF) {
        return;
    }
    const u32 data_offset = in.U32(0xC);
    const u32 count = in.U16(0x14 + 6);
    for (u32 i = 0; i < count && in.Has(0x20 + size_t{i} * 16, 16); ++i) {
        const size_t entry = 0x14 + 12 + size_t{i} * 16;
        const u32 start = in.U32(entry + 8);
        const u32 end = in.U32(entry + 12);
        if (end > start && in.Has(size_t{data_offset} + start, end - start)) {
            visit(size_t{data_offset} + start);
        }
    }
}

inline float AsFloat(u32 bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline u32 AsBits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline void Put(Reader& in, size_t offset, u32 value) {
    if (in.Has(offset, 4)) {
        std::memcpy(in.data.data() + offset, &value, 4);
    }
}

} // namespace Detail

/// Sets the most foliage models drawn and kept alive at once of a decompressed
/// ClusteredRendering/SystemSettings archive to 1 (not 0, which a buffer size might not allow).
/// Returns whether it changed anything.
inline bool PatchSystemSettings(std::span<u8> archive) {
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if ((key == "MaxModelsOutputForDrawNum" || key == "MaxModelsOutputForDeadAliveNum") &&
                (type == Detail::TypeInt || type == Detail::TypeUInt)) {
                Detail::Put(in, at, 1);
                changed = true;
            }
        });
    });
    return changed;
}

/// Sets every DynamicPlacementProbability of a decompressed ClusteredRendering/MaterialSettings
/// archive to 0 and the number of placed models (PlacedModelsMinNum, PlacedModelsMaxNum) to 1.
inline bool PatchMaterialSettings(std::span<u8> archive) {
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (key == "DynamicPlacementProbability" && type == Detail::TypeFloat) {
                Detail::Put(in, at, Detail::AsBits(0.0f));
                changed = true;
            } else if ((key == "PlacedModelsMinNum" || key == "PlacedModelsMaxNum") &&
                       (type == Detail::TypeInt || type == Detail::TypeUInt)) {
                Detail::Put(in, at, 1);
                changed = true;
            }
        });
    });
    return changed;
}

/// Sets each model's FarClipDistance of a decompressed ClusteredRendering/ModelSettings archive
/// to a distance of nothing (0.01, not 0, which might be divided by).
inline bool PatchModelSettings(std::span<u8> archive) {
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (key == "FarClipDistance" && type == Detail::TypeFloat) {
                Detail::Put(in, at, Detail::AsBits(0.01f));
                changed = true;
            }
        });
    });
    return changed;
}

/// Bananza's foliage levels between off and the game's own (the Tuning tab's Foliage dropdown):
/// EDEN_NX2_DK_FOLIAGE_DENSITY=<1-99>, the % of the foliage kept (placed in the stage's data, in
/// patches, and placed while playing), and EDEN_NX2_DK_FOLIAGE_DISTANCE=<20-700>, the farthest any
/// foliage is drawn, in units. Foliage off wins over both.
constexpr const char* DensitySwitch = "EDEN_NX2_DK_FOLIAGE_DENSITY";
constexpr const char* DistanceSwitch = "EDEN_NX2_DK_FOLIAGE_DISTANCE";
constexpr int MinDensity = 1, MaxDensity = 99;
constexpr int MinDistance = 20, MaxDistance = 700;

struct Levels {
    int density = 0;  ///< % kept, 0: the game's
    int distance = 0; ///< units, 0: each model's own

    [[nodiscard]] bool Active() const {
        return density != 0 || distance != 0;
    }
};

/// `value` as a whole number from `min` to `max`, else 0.
constexpr int LevelFrom(const char* value, int min, int max) {
    if (value == nullptr || *value == 0) {
        return 0;
    }
    int number = 0;
    for (const char* c = value; *c != 0; ++c) {
        if (*c < '0' || *c > '9' || number > max) {
            return 0;
        }
        number = number * 10 + (*c - '0');
    }
    return number >= min && number <= max ? number : 0;
}

/// The levels from the environment; none with foliage off.
inline Levels LevelsFromEnvironment() {
    if (OffFromEnvironment()) {
        return {};
    }
    return {LevelFrom(std::getenv(DensitySwitch), MinDensity, MaxDensity),
            LevelFrom(std::getenv(DistanceSwitch), MinDistance, MaxDistance)};
}

/// Scales the dynamic placements per frame of a decompressed ClusteredRendering/SystemSettings
/// archive by the density. Returns whether it changed anything.
inline bool PatchSystemSettings(std::span<u8> archive, const Levels& levels) {
    if (levels.density == 0) {
        return false;
    }
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (key == "DynamicPlacementsPerFrameMax" &&
                (type == Detail::TypeInt || type == Detail::TypeUInt)) {
                Detail::Put(in, at,
                            std::max<u32>(in.U32(at) * static_cast<u32>(levels.density) / 100, 1));
                changed = true;
            }
        });
    });
    return changed;
}

/// Scales every DynamicPlacementProbability and the number of dynamically placed models of a
/// decompressed ClusteredRendering/MaterialSettings archive by the density.
inline bool PatchMaterialSettings(std::span<u8> archive, const Levels& levels) {
    if (levels.density == 0) {
        return false;
    }
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (key == "DynamicPlacementProbability" && type == Detail::TypeFloat) {
                Detail::Put(in, at,
                            Detail::AsBits(Detail::AsFloat(in.U32(at)) *
                                           static_cast<float>(levels.density) / 100.0f));
                changed = true;
            } else if ((key == "PlacedModelsMinNum" || key == "PlacedModelsMaxNum") &&
                       (type == Detail::TypeInt || type == Detail::TypeUInt)) {
                Detail::Put(in, at,
                            std::max<u32>(in.U32(at) * static_cast<u32>(levels.density) / 100, 1));
                changed = true;
            }
        });
    });
    return changed;
}

/// Brings each model's FarClipDistance of a decompressed ClusteredRendering/ModelSettings archive
/// down to the distance, so all foliage ends at one line whatever each model's own (80 to 700).
inline bool PatchModelSettings(std::span<u8> archive, const Levels& levels) {
    if (levels.distance == 0) {
        return false;
    }
    const float limit = static_cast<float>(levels.distance);
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (key == "FarClipDistance" && type == Detail::TypeFloat &&
                Detail::AsFloat(in.U32(at)) > limit) {
                Detail::Put(in, at, Detail::AsBits(limit));
                changed = true;
            }
        });
    });
    return changed;
}

/// The models without a FarClipDistance of their own (174 of 418, a third of the placed foliage)
/// take its reader's default, 100 in the read-only data. Brings it down to the distance in
/// `image` (a module from its text segment on, read-only data after the `aligned_size` bytes of
/// text), found from its reader:
///   adrp x9, "FarClipDistance"; add x9, x9, ...; 5 words; adrp x8, default; add x8, x8, ...
/// Returns whether it was found (once).
inline bool PatchDefaultDistance(std::span<u8> image, u32 text_size, u32 aligned_size,
                                 const Levels& levels) {
    constexpr std::string_view Name{"FarClipDistance", sizeof("FarClipDistance")};
    constexpr float GameDefault = 100.0f;
    if (levels.distance == 0 || image.size() < aligned_size || aligned_size < text_size) {
        return false;
    }
    u8* const code = image.data();
    const size_t count = text_size / sizeof(u32);
    const auto word = [code](size_t index) {
        u32 value;
        std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto target = [&](size_t i, u32 reg) -> size_t {
        const u32 adrp = word(i), add = word(i + 1);
        if ((adrp & 0x9F00001FU) != (0x90000000U | reg) ||
            (add & 0xFFC003FFU) != (0x91000000U | reg << 5 | reg)) {
            return 0;
        }
        s64 pages = static_cast<s64>(((adrp >> 5) & 0x7FFFFU) << 2 | ((adrp >> 29) & 3));
        if (pages & (s64{1} << 20)) {
            pages -= s64{1} << 21;
        }
        return static_cast<size_t>(static_cast<s64>(i * 4 & ~size_t{0xFFF}) + pages * 0x1000) +
               ((add >> 10) & 0xFFF);
    };
    const std::string_view data{reinterpret_cast<const char*>(code), image.size()};
    size_t name = data.find(Name, aligned_size);
    while (name != std::string_view::npos && data[name - 1] != 0) {
        name = data.find(Name, name + 1);
    }
    size_t at = 0, found = 0;
    for (size_t i = 0; name != std::string_view::npos && i + 12 < count; ++i) {
        if (target(i, 9) != name) {
            continue;
        }
        for (size_t j = i + 2; j < i + 10; ++j) {
            const size_t value = target(j, 8);
            float stored = 0.0f;
            if (value != 0 && value + sizeof(stored) <= image.size()) {
                std::memcpy(&stored, code + value, sizeof(stored));
            }
            if (stored == GameDefault) {
                at = value;
                ++found;
                break;
            }
        }
    }
    if (found != 1) {
        return false;
    }
    const float distance = std::min(GameDefault, static_cast<float>(levels.distance));
    std::memcpy(code + at, &distance, sizeof(distance));
    return true;
}

/// Each stage's foliage placed ahead of time (ClusteredRendering/StageData/<stage>/<scenario>
/// .ProceduralPlacement.byml, big endian BYMLs of every plant's position): the density thins
/// these, in patches.
constexpr std::string_view PlacementFolder = "ClusteredRendering/StageData";
constexpr std::string_view PlacementSuffix = ".ProceduralPlacement.byml";

inline bool PlacementsWanted(const Levels& levels) {
    return levels.density != 0;
}

namespace Detail {

inline u32 Big32(const Reader& in, size_t offset) {
    const u32 v = in.U32(offset);
    return (v >> 24) | ((v >> 8) & 0xFF00U) | ((v << 8) & 0xFF0000U) | (v << 24);
}

inline void PutBig32(Reader& in, size_t offset, u32 value) {
    Put(in, offset, (value >> 24) | ((value >> 8) & 0xFF00U) | ((value << 8) & 0xFF0000U) |
                        (value << 24));
}

inline float Hash01(s64 x, s64 z, u32 seed) {
    u64 h = (static_cast<u64>(x) * 0x9E3779B97F4A7C15ULL) ^
            (static_cast<u64>(z) * 0xC2B2AE3D27D4EB4FULL) ^ seed;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 29;
    return static_cast<float>(h >> 40) / static_cast<float>(1ULL << 24);
}

/// Smooth noise over the ground, about `PatchSize` units across, so thinning leaves clumps and
/// bare patches rather than an even thin carpet
inline float PatchNoise(float x, float z) {
    constexpr float PatchSize = 8.0f;
    const float fx = x / PatchSize, fz = z / PatchSize;
    const float ix = std::floor(fx), iz = std::floor(fz);
    const float tx = fx - ix, tz = fz - iz;
    const float sx = tx * tx * (3 - 2 * tx), sz = tz * tz * (3 - 2 * tz);
    const s64 x0 = static_cast<s64>(ix), z0 = static_cast<s64>(iz);
    const float a = Hash01(x0, z0, 1), b = Hash01(x0 + 1, z0, 1);
    const float c = Hash01(x0, z0 + 1, 1), d = Hash01(x0 + 1, z0 + 1, 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz;
}

} // namespace Detail

/// Keeps `levels.density` % of the plants of a ProceduralPlacement BYML in place: the others are
/// moved far below the stage and scaled to 0, so the counts the file keeps stay right. Plants on
/// Money materials (gold) are kept. Returns how many were removed.
inline size_t PatchPlacement(std::span<u8> file, const Levels& levels) {
    if (!PlacementsWanted(levels)) {
        return 0;
    }
    Detail::Reader in{file};
    if (!in.Has(0, 16) || in.data[0] != 'B' || in.data[1] != 'Y') {
        return 0;
    }
    const u32 key_table = Detail::Big32(in, 4);
    const u32 string_table = Detail::Big32(in, 8);
    const u32 root = Detail::Big32(in, 12);
    if (key_table == 0 || root == 0) {
        return 0;
    }
    const auto name_at = [&](u32 table, u32 index) -> std::string_view {
        if (table == 0 || index >= (Detail::Big32(in, table) & 0xFFFFFF)) {
            return {};
        }
        const size_t start = table + size_t{Detail::Big32(in, table + 4 + size_t{index} * 4)};
        size_t end = start;
        while (in.Has(end, 1) && in.data[end] != 0) {
            ++end;
        }
        return in.Has(start, end - start)
                   ? std::string_view{reinterpret_cast<const char*>(in.data.data()) + start,
                                      end - start}
                   : std::string_view{};
    };

    struct Plant {
        float x, z, score;
        size_t y_at, scale_at[3];
        u32 material;
    };
    std::vector<Plant> plants;
    std::vector<std::string_view> materials;
    size_t budget = 1u << 22;
    const auto walk = [&](auto&& self, size_t node, std::string_view parent, int depth) -> void {
        if (depth > 24 || budget == 0 || !in.Has(node, 4)) {
            return;
        }
        --budget;
        const u8 type = in.data[node];
        const u32 count = Detail::Big32(in, node) & 0xFFFFFF;
        if (type == Detail::TypeArray) {
            const size_t values = node + 4 + (size_t{count} + 3) / 4 * 4;
            for (u32 i = 0; i < count && in.Has(values + size_t{i} * 4, 4); ++i) {
                const u8 element = in.data[node + 4 + i];
                const u32 value = Detail::Big32(in, values + size_t{i} * 4);
                if (element == Detail::TypeDict || element == Detail::TypeArray) {
                    self(self, value, parent, depth + 1);
                } else if (element == 0xA0 && parent == "ReferencedMaterials") {
                    materials.push_back(name_at(string_table, value));
                }
            }
            return;
        }
        if (type != Detail::TypeDict) {
            return;
        }
        Plant plant{0, 0, 0, 0, {0, 0, 0}, ~0U};
        bool has_x = false, has_z = false;
        for (u32 i = 0; i < count && in.Has(node + 4 + size_t{i} * 8, 8); ++i) {
            const size_t entry = node + 4 + size_t{i} * 8;
            const u32 head = Detail::Big32(in, entry);
            const u8 entry_type = static_cast<u8>(head & 0xFF);
            const std::string_view key = name_at(key_table, head >> 8);
            const size_t at = entry + 4;
            if (entry_type == Detail::TypeDict || entry_type == Detail::TypeArray) {
                self(self, Detail::Big32(in, at), key, depth + 1);
                continue;
            }
            if (parent != "Posture") {
                continue;
            }
            if (entry_type == Detail::TypeFloat) {
                const float value = Detail::AsFloat(Detail::Big32(in, at));
                if (key == "X") {
                    plant.x = value;
                    has_x = true;
                } else if (key == "Z") {
                    plant.z = value;
                    has_z = true;
                } else if (key == "Y") {
                    plant.y_at = at;
                } else if (key == "SX" || key == "SY" || key == "SZ") {
                    plant.scale_at[key[1] - 'X'] = at;
                }
            } else if (key == "ReferencedMaterialIndex" &&
                       (entry_type == Detail::TypeInt || entry_type == Detail::TypeUInt)) {
                plant.material = Detail::Big32(in, at);
            }
        }
        if (parent == "Posture" && has_x && has_z && plant.y_at != 0) {
            plants.push_back(plant);
        }
    };
    walk(walk, root, {}, 0);

    // Each plant's place in the patches, with a little of its own so patch edges are not sharp
    std::vector<Plant*> thinned;
    for (auto& plant : plants) {
        if (plant.material < materials.size() && materials[plant.material].starts_with("Money")) {
            continue;
        }
        plant.score = 0.8f * Detail::PatchNoise(plant.x, plant.z) +
                      0.2f * Detail::Hash01(static_cast<s64>(plant.x * 16.0f),
                                            static_cast<s64>(plant.z * 16.0f), 2);
        thinned.push_back(&plant);
    }
    const size_t keep = thinned.size() * static_cast<size_t>(levels.density) / 100;
    if (keep >= thinned.size()) {
        return 0;
    }
    std::nth_element(thinned.begin(), thinned.begin() + static_cast<std::ptrdiff_t>(keep),
                     thinned.end(),
                     [](const Plant* a, const Plant* b) { return a->score < b->score; });
    for (auto it = thinned.begin() + static_cast<std::ptrdiff_t>(keep); it != thinned.end(); ++it) {
        const Plant& plant = **it;
        const float y = Detail::AsFloat(Detail::Big32(in, plant.y_at));
        Detail::PutBig32(in, plant.y_at, Detail::AsBits(y - 10000.0f));
        for (const size_t at : plant.scale_at) {
            if (at != 0) {
                Detail::PutBig32(in, at, Detail::AsBits(0.0f));
            }
        }
    }
    return thinned.size() - keep;
}

} // namespace Loader::Nx2DkFoliage
