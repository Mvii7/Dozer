// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "core/loader/nx2_dk_quality.h"

// Donkey Kong Bananza's foliage ("Clustered Rendering") settings are game data: the caps are in
// ClusteredRendering/SystemSettings and the chance of a dynamic placement in each material of
// ClusteredRendering/MaterialSettings (both zstd-compressed SARC archives of one BYML each).
// The quality rows below change those values in the archives as they are read from the RomFS.
namespace Loader::Nx2DkFoliage {

using Loader::Nx2DkQuality::Id;

struct Settings {
    int cap = 0;          ///< % of the models drawn / kept alive at once, 0: the game's
    bool frustum_off = false;
    int density = 0;      ///< % of the dynamic placements, 0: the game's
    bool off = false;     ///< no foliage at all
    int far_clip = 0;     ///< % of each model's far clip distance
    int lod = 0;          ///< % of each model's LOD thresholds
    int emit = 0;         ///< % of the effects started per frame
    int particles = 0;    ///< % of the particles in one emitter
    int alive = 0;        ///< % of the effects alive per effect rule
    int material = 0;     ///< % of the material effects
    bool emit_off = false;     ///< each effect cap at 1
    bool particles_off = false;
    bool alive_off = false;
    bool material_off = false;

    [[nodiscard]] bool Active() const {
        return off || cap != 0 || frustum_off || density != 0 || far_clip != 0 || lod != 0 || emit != 0 || particles != 0 ||
               alive != 0 || material != 0 || emit_off ||
               particles_off || alive_off || material_off;
    }
};

inline int ReadEnv(Id id) {
    const auto& knob = Nx2DkQuality::Knobs[id];
    const char* text = std::getenv(knob.env.data());
    return text != nullptr ? Nx2DkQuality::Sanitize(std::atoi(text), knob) : 0;
}

inline Settings FromEnvironment() {
    return {ReadEnv(Id::FoliageCap),     ReadEnv(Id::FoliageFrustumOff) != 0,
            ReadEnv(Id::FoliageDensity), ReadEnv(Id::FoliageOff) != 0,
            ReadEnv(Id::FoliageFarClip),
            ReadEnv(Id::FoliageLod),     ReadEnv(Id::EffectEmitCap),
            ReadEnv(Id::EffectParticleCap), ReadEnv(Id::EffectAliveCap),
            ReadEnv(Id::EffectMaterialCap), ReadEnv(Id::EffectEmitOff) != 0,
            ReadEnv(Id::EffectParticleOff) != 0, ReadEnv(Id::EffectAliveOff) != 0,
            ReadEnv(Id::EffectMaterialOff) != 0};
}

constexpr std::string_view SystemSettingsPath = "ClusteredRendering/SystemSettings.alsarc.zs";
constexpr std::string_view ModelSettingsPath = "ClusteredRendering/ModelSettings.alsarc.zs";
constexpr std::string_view MaterialSettingsPath = "ClusteredRendering/MaterialSettings.alsarc.zs";
constexpr std::string_view EffectSettingsPath = "SystemData/EffectSettings.alsarc.zs";
constexpr std::string_view SettingUnitPath = "MaterialEffect/SettingUnit.alsarc.zs";

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

/// Changes the caps of a decompressed ClusteredRendering/SystemSettings archive in place.
/// Returns whether it changed anything.
inline bool PatchSystemSettings(std::span<u8> archive, const Settings& settings) {
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            const auto scale = [&](int percent) {
                if ((type == Detail::TypeInt || type == Detail::TypeUInt) && percent != 0) {
                    const u32 value = in.U32(at);
                    Detail::Put(in, at, std::max<u32>(value * static_cast<u32>(percent) / 100, 1));
                    changed = true;
                }
            };
            if (key == "MaxModelsOutputForDrawNum" || key == "MaxModelsOutputForDeadAliveNum") {
                // Off: one model at most (not 0, which a buffer size might not allow)
                if (settings.off && (type == Detail::TypeInt || type == Detail::TypeUInt)) {
                    Detail::Put(in, at, 1);
                    changed = true;
                } else {
                    scale(settings.cap);
                }
            } else if (key == "DynamicPlacementsPerFrameMax") {
                scale(settings.density);
            } else if (settings.frustum_off && type == Detail::TypeFloat &&
                       (key == "HorizontalFrustumExpansionPctg" ||
                        key == "VerticalFrustumExpansionPctg")) {
                Detail::Put(in, at, Detail::AsBits(0.0f));
                changed = true;
            }
        });
    });
    return changed;
}

/// Scales every DynamicPlacementProbability and the number of dynamically placed models
/// (PlacedModelsMinNum, PlacedModelsMaxNum) of a decompressed ClusteredRendering/MaterialSettings
/// archive in place. Returns whether it changed anything.
inline bool PatchMaterialSettings(std::span<u8> archive, const Settings& settings) {
    if (settings.density == 0 && !settings.off) {
        return false;
    }
    // Off: no chance of a dynamic placement and one placed model at most
    const int density = settings.off ? 1 : settings.density;
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (key == "DynamicPlacementProbability" && type == Detail::TypeFloat) {
                const float value = Detail::AsFloat(in.U32(at));
                Detail::Put(in, at,
                            Detail::AsBits(settings.off ? 0.0f
                                                        : value * static_cast<float>(density) / 100.0f));
                changed = true;
            } else if ((key == "PlacedModelsMinNum" || key == "PlacedModelsMaxNum") &&
                       (type == Detail::TypeInt || type == Detail::TypeUInt)) {
                Detail::Put(in, at, std::max<u32>(in.U32(at) * static_cast<u32>(density) / 100, 1));
                changed = true;
            }
        });
    });
    return changed;
}

/// Scales each model's FarClipDistance and LodThreshold0-2 of a decompressed
/// ClusteredRendering/ModelSettings archive in place. Returns whether it changed anything.
inline bool PatchModelSettings(std::span<u8> archive, const Settings& settings) {
    if (settings.far_clip == 0 && settings.lod == 0 && !settings.off) {
        return false;
    }
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (type != Detail::TypeFloat) {
                return;
            }
            int percent = 0;
            if (key == "FarClipDistance") {
                if (settings.off) {
                    // A distance of nothing: not 0, which might be divided by
                    Detail::Put(in, at, Detail::AsBits(0.01f));
                    changed = true;
                    return;
                }
                percent = settings.far_clip;
            } else if (key == "LodThreshold0" || key == "LodThreshold1" ||
                       key == "LodThreshold2") {
                percent = settings.lod;
            }
            if (percent != 0) {
                Detail::Put(in, at,
                            Detail::AsBits(Detail::AsFloat(in.U32(at)) *
                                           static_cast<float>(percent) / 100.0f));
                changed = true;
            }
        });
    });
    return changed;
}

/// Scales the effect caps of a decompressed SystemData/EffectSettings archive in place.
inline bool PatchEffectSettings(std::span<u8> archive, const Settings& settings) {
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            int percent = 0;
            bool off = false;
            if (key == "EmitCountPerFrameMax") {
                percent = settings.emit;
                off = settings.emit_off;
            } else if (key == "OneEmitterMaxParticleNum") {
                percent = settings.particles;
                off = settings.particles_off;
            } else if (key == "MaxAliveEffectsNum") {
                percent = settings.alive;
                off = settings.alive_off;
            } else if (key == "MaxMatEffActiveUsersNumPerSlot") {
                percent = settings.material;
                off = settings.material_off;
            }
            if (!(type == Detail::TypeInt || type == Detail::TypeUInt)) {
                return;
            }
            if (off) {
                // One at most (not 0, which a buffer size might not allow)
                Detail::Put(in, at, 1);
                changed = true;
            } else if (percent != 0) {
                Detail::Put(in, at, std::max<u32>(in.U32(at) * static_cast<u32>(percent) / 100, 1));
                changed = true;
            }
        });
    });
    return changed;
}

/// Scales each MaxActiveEffectNum of a decompressed MaterialEffect/SettingUnit archive in place.
inline bool PatchSettingUnits(std::span<u8> archive, const Settings& settings) {
    if (settings.material == 0 && !settings.material_off) {
        return false;
    }
    Detail::Reader in{archive};
    bool changed = false;
    Detail::ForEachSarcFile(in, [&](size_t base) {
        Detail::WalkByml(in, base, [&](std::string_view key, u8 type, size_t at) {
            if (key == "MaxActiveEffectNum" &&
                (type == Detail::TypeInt || type == Detail::TypeUInt)) {
                Detail::Put(in, at,
                            settings.material_off
                                ? 1
                                : std::max<u32>(in.U32(at) *
                                                    static_cast<u32>(settings.material) / 100, 1));
                changed = true;
            }
        });
    });
    return changed;
}

} // namespace Loader::Nx2DkFoliage
