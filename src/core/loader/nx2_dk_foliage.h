// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include "common/common_types.h"

// Donkey Kong Bananza's foliage ("Clustered Rendering") settings are game data: the caps are in
// ClusteredRendering/SystemSettings, the chance of a dynamic placement in each material of
// ClusteredRendering/MaterialSettings and each model's draw distance in
// ClusteredRendering/ModelSettings (zstd-compressed SARC archives of BYML files). Foliage off
// changes those values in the archives as they are read from the RomFS (file_sys/patch_manager.cpp).
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

} // namespace Loader::Nx2DkFoliage
