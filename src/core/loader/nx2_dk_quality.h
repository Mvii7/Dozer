// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/loader/nx2_dk_dynamic_fps.h"
#include "core/nx2_perf_settings.h"

// EDEN_NX2_DKQ_*=<n> (all unset by default): Donkey Kong Bananza's quality settings, the Bananza
// tab of the menu program. Each one replaces a value the game reads from its graphics parameters
// (GraphicsParam, VoxelMeshLodParam) by a constant, in the main module as it loads. A setting is
// only put in when its instructions are found exactly where expected; unset, nothing is touched.
// Found in the game's 1.0.0 main module (build id e25c90c1c136...); experimental, see
// NX2_NOTES.md.
namespace Loader::Nx2DkQuality {

enum Id : size_t {
    ResBest,           ///< highest render resolution, % of the output (NdsrUpscaleMinRate)
    ResWorst,          ///< lowest render resolution, % of the output (NdsrUpscaleMaxRate)
    GpuTarget,         ///< GPU load the resolution control aims for, % (MaintainGpuPercentage)
    FrameMinScale,     ///< FrameBufferMinScale, %
    BloomScale,        ///< BloomBufferBaseScale, %
    CubeMapInterval,   ///< CubeMapShotInterval, frames
    RadialBlurScale,   ///< RadialBlurRenderScale, %
    RadialBlurSamples, ///< RadialBlurSamplingNum
    VoxelLod1Dist,     ///< terrain Lod1Dist (the nearer LOD step), % of the game's
    VoxelLod1Value,    ///< terrain Lod1Value, % of the game's
    VoxelLod2Dist,     ///< terrain Lod2Dist (the farther LOD step), % of the game's
    VoxelLod2Value,    ///< terrain Lod2Value, % of the game's
    CameraFar,         ///< the camera's far distance, % (EDEN_NX2_FAR_SCALE of the Selevet fork)
    ColorDecalMax,     ///< ColorDecalMaxNum
    MeshSimplify,      ///< terrain MeshSimplifyType: 1 Normal, 2 High, 3 SuperHigh, 4 NoSimplify, 5 Low
    CharacterLod,      ///< each model's own LodThreshold0-2 (characters), % of the game's
    ActorDrawDistance, ///< the actors' far clip distances (FarClipLevel), % of the game's
    VoxelDamageOff,    ///< the voxel effect emitter's DamageKey made empty
    VoxelDestructionOff, ///< the voxel effect emitter's DestructionKey made empty
    GgxFilterOff,      ///< IsUseGGXFilter forced off (reflection cube map filtering)
    CubeMapOcclusionOff, ///< IsUseCubeMapOcclusionCheck forced off
    OcclusionDepth,    ///< OcclusionQueryThresholdDepth, % of the game's
    OcclusionRadius,   ///< OcclusionQueryThresholdRadius, %: objects under it get a query
    OcclusionPolygon,  ///< OcclusionQueryThresholdPolygon, %: objects over it get a query
    VoxelBorderMeshOff, ///< terrain IsNoBorderMesh forced on
    ShadowMaskOff,     ///< IsUseShadowMask forced off
    DecalOff,          ///< IsUseDecal forced off
    DepthBloomOff,     ///< IsUseDepthBloom forced off
    BloomExtensionOff, ///< IsUseBloomExtention forced off
    RadialBlurOff,     ///< IsUseRadialBlurDrawer forced off
    ThunderOff,        ///< IsUseThunderRenderer forced off
    CloudShapeOff,     ///< IsUseCloudShape forced off
    DofIndirectOff,    ///< IsUseDofIndirectCtrl forced off
    CubeMapOff, ///< IsUseCubeMap forced off
    FogOff, ///< IsUseFog forced off
    LightMapOff, ///< IsUseLightMapDirector forced off
    ModelForegroundOff, ///< IsUseModelForegroundDrawer forced off
    PrePassLightOff, ///< IsUsePrePassLight forced off
    FoliageCap,        ///< foliage models drawn / kept alive at once, % (ClusteredRendering)
    FoliageFrustumOff, ///< foliage frustum expansion set to 0
    FoliageDensity,    ///< foliage dynamic placement chance and rate, %
    FoliageFarClip,    ///< each foliage model's FarClipDistance, % of the game's
    FoliageLod,        ///< each foliage model's LodThreshold0-2, % of the game's (direction not known)
    ObjectLod,         ///< the graphics parameters' ModelLodThresholdFirst-Third, % of the game's
    EffectEmitCap,     ///< EmitCountPerFrameMax (effects started per frame), % of the game's
    EffectParticleCap, ///< OneEmitterMaxParticleNum, % of the game's
    EffectAliveCap,    ///< the MaxAliveEffectsNum of the effect rules, % of the game's
    EffectMaterialCap, ///< MaxMatEffActiveUsersNumPerSlot and each MaxActiveEffectNum, %
    EffectEmitOff,     ///< effects started per frame at 1
    EffectParticleOff, ///< particles per emitter at 1
    EffectAliveOff,    ///< effects alive per rule at 1
    EffectMaterialOff, ///< terrain material effects at 1
    VoxelEmitterBuffer, ///< buffer of the voxel effect emitter (voxel debris), %
    VoxelEmitterOff,   ///< the voxel effect emitter's buffer set to 0
    VoxelSurfaceBuffer, ///< buffer of the voxel surface effects, %
    VoxelSurfaceEffectOff, ///< voxel surface effects off
    DepthCollisionEffectOff, ///< depth collision effects off
    DebrisOff,         ///< every voxel material's FragmentType forced to DisableForce
    Count,
};

struct Knob {
    std::string_view key; ///< in the [Bananza] group of qt-config.ini
    std::string_view env;
    int min;
    int max;
    int game; ///< the game's own value, shown only (the % of the game's ones: 100)
};

/// A row with nothing to choose but on: its variable is 1 and it forces something off.
constexpr bool IsToggle(const Knob& knob) {
    return knob.min == knob.max;
}


inline constexpr std::array<Knob, Count> Knobs{{
    {"res_best", "EDEN_NX2_DKQ_RES_BEST", 25, 100, 73},
    {"res_worst", "EDEN_NX2_DKQ_RES_WORST", 25, 100, 50},
    {"gpu_target", "EDEN_NX2_DKQ_GPU_TARGET", 40, 100, 90},
    {"frame_min_scale", "EDEN_NX2_DKQ_FRAME_MIN_SCALE", 25, 100, 100},
    {"bloom_scale", "EDEN_NX2_DKQ_BLOOM_SCALE", 10, 100, 25},
    {"cubemap_interval", "EDEN_NX2_DKQ_CUBEMAP_INTERVAL", 1, 240, 8},
    {"radial_blur_scale", "EDEN_NX2_DKQ_RADIAL_BLUR_SCALE", 10, 100, 50},
    {"radial_blur_samples", "EDEN_NX2_DKQ_RADIAL_BLUR_SAMPLES", 1, 32, 20},
    {"voxel_lod1_dist", "EDEN_NX2_DKQ_VOXEL_LOD1_DIST", 10, 400, 100},
    {"voxel_lod1_value", "EDEN_NX2_DKQ_VOXEL_LOD1_VALUE", 10, 1000, 100},
    {"voxel_lod2_dist", "EDEN_NX2_DKQ_VOXEL_LOD2_DIST", 10, 400, 100},
    {"voxel_lod2_value", "EDEN_NX2_DKQ_VOXEL_LOD2_VALUE", 10, 1000, 100},
    {"camera_far", "EDEN_NX2_DKQ_CAMERA_FAR", 10, 100, 100},
    {"color_decal_max", "EDEN_NX2_DKQ_COLOR_DECAL_MAX", 1, 660, 660},
    {"mesh_simplify", "EDEN_NX2_DKQ_MESH_SIMPLIFY", 1, 5, 1},
    {"character_lod", "EDEN_NX2_DKQ_CHARACTER_LOD", 25, 400, 100},
    {"actor_draw_distance", "EDEN_NX2_DKQ_ACTOR_DRAW_DISTANCE", 10, 100, 100},
    {"voxel_damage_off", "EDEN_NX2_DKQ_VOXEL_DAMAGE_OFF", 1, 1, 0},
    {"voxel_destruction_off", "EDEN_NX2_DKQ_VOXEL_DESTRUCTION_OFF", 1, 1, 0},
    {"ggx_filter_off", "EDEN_NX2_DKQ_GGX_FILTER_OFF", 1, 1, 0},
    {"cubemap_occlusion_off", "EDEN_NX2_DKQ_CUBEMAP_OCCLUSION_OFF", 1, 1, 0},
    {"occlusion_depth", "EDEN_NX2_DKQ_OCCLUSION_DEPTH", 10, 400, 100},
    {"occlusion_radius", "EDEN_NX2_DKQ_OCCLUSION_RADIUS", 10, 1000, 100},
    {"occlusion_polygon", "EDEN_NX2_DKQ_OCCLUSION_POLYGON", 1, 400, 100},
    {"voxel_border_mesh_off", "EDEN_NX2_DKQ_VOXEL_BORDER_MESH_OFF", 1, 1, 0},
    {"shadow_mask_off", "EDEN_NX2_DKQ_SHADOW_MASK_OFF", 1, 1, 0},
    {"decal_off", "EDEN_NX2_DKQ_DECAL_OFF", 1, 1, 0},
    {"depth_bloom_off", "EDEN_NX2_DKQ_DEPTH_BLOOM_OFF", 1, 1, 0},
    {"bloom_extension_off", "EDEN_NX2_DKQ_BLOOM_EXTENSION_OFF", 1, 1, 0},
    {"radial_blur_off", "EDEN_NX2_DKQ_RADIAL_BLUR_OFF", 1, 1, 0},
    {"thunder_off", "EDEN_NX2_DKQ_THUNDER_OFF", 1, 1, 0},
    {"cloud_shape_off", "EDEN_NX2_DKQ_CLOUD_SHAPE_OFF", 1, 1, 0},
    {"dof_off", "EDEN_NX2_DKQ_DOF_OFF", 1, 1, 0},
    {"cubemap_off", "EDEN_NX2_DKQ_CUBEMAP_OFF", 1, 1, 0},
    {"fog_off", "EDEN_NX2_DKQ_FOG_OFF", 1, 1, 0},
    {"lightmap_off", "EDEN_NX2_DKQ_LIGHTMAP_OFF", 1, 1, 0},
    {"model_foreground_off", "EDEN_NX2_DKQ_MODEL_FOREGROUND_OFF", 1, 1, 0},
    {"prepass_light_off", "EDEN_NX2_DKQ_PREPASS_LIGHT_OFF", 1, 1, 0},
    {"foliage_cap", "EDEN_NX2_DKQ_FOLIAGE_CAP", 1, 100, 100},
    {"foliage_frustum_off", "EDEN_NX2_DKQ_FOLIAGE_FRUSTUM_OFF", 1, 1, 0},
    {"foliage_density", "EDEN_NX2_DKQ_FOLIAGE_DENSITY", 1, 100, 100},
    {"foliage_far_clip", "EDEN_NX2_DKQ_FOLIAGE_FAR_CLIP", 1, 100, 100},
    {"foliage_lod", "EDEN_NX2_DKQ_FOLIAGE_LOD", 10, 1000, 100},
    {"object_lod", "EDEN_NX2_DKQ_OBJECT_LOD", 25, 400, 100},
    {"effect_emit_cap", "EDEN_NX2_DKQ_EFFECT_EMIT_CAP", 5, 100, 100},
    {"effect_particle_cap", "EDEN_NX2_DKQ_EFFECT_PARTICLE_CAP", 5, 100, 100},
    {"effect_alive_cap", "EDEN_NX2_DKQ_EFFECT_ALIVE_CAP", 5, 100, 100},
    {"effect_material_cap", "EDEN_NX2_DKQ_EFFECT_MATERIAL_CAP", 5, 100, 100},
    {"effect_emit_off", "EDEN_NX2_DKQ_EFFECT_EMIT_OFF", 1, 1, 0},
    {"effect_particle_off", "EDEN_NX2_DKQ_EFFECT_PARTICLE_OFF", 1, 1, 0},
    {"effect_alive_off", "EDEN_NX2_DKQ_EFFECT_ALIVE_OFF", 1, 1, 0},
    {"effect_material_off", "EDEN_NX2_DKQ_EFFECT_MATERIAL_OFF", 1, 1, 0},
    {"voxel_emitter_buffer", "EDEN_NX2_DKQ_VOXEL_EMITTER_BUFFER", 1, 100, 100},
    {"voxel_emitter_off", "EDEN_NX2_DKQ_VOXEL_EMITTER_OFF", 1, 1, 0},
    {"voxel_surface_buffer", "EDEN_NX2_DKQ_VOXEL_SURFACE_BUFFER", 1, 100, 100},
    {"voxel_surface_effect_off", "EDEN_NX2_DKQ_VOXEL_SURFACE_EFFECT_OFF", 1, 1, 0},
    {"depth_collision_effect_off", "EDEN_NX2_DKQ_DEPTH_COLLISION_EFFECT_OFF", 1, 1, 0},
    {"debris_off", "EDEN_NX2_DKQ_DEBRIS_OFF", 1, 1, 0},
}};

/// One number per knob; 0 leaves the game's own.
using Values = std::array<int, Count>;

/// The presets of the Quality tab. Default leaves every setting to the game; Custom is whatever
/// else the rows hold.
enum class Preset : u8 {
    Default,
    Balanced,
    Performance,
    Custom,
};

/// A preset's values (0: the game's). Custom has none of its own: nothing set. The terrain LOD
/// distance and mesh simplification are not in them: the first Performance preset with them was
/// slower than the game's own, and which row did it is not known yet.
constexpr Values PresetValues(Preset preset) {
    Values values{};
    if (preset == Preset::Balanced) {
        values[ResBest] = 65;
        values[ResWorst] = 45;
        values[BloomScale] = 20;
        values[CubeMapInterval] = 12;
        values[RadialBlurScale] = 40;
        values[RadialBlurSamples] = 12;
        values[CameraFar] = 80;
        values[ColorDecalMax] = 400;
    } else if (preset == Preset::Performance) {
        values[ResBest] = 50;
        values[ResWorst] = 35;
        values[BloomScale] = 12;
        values[CubeMapInterval] = 24;
        values[RadialBlurScale] = 25;
        values[RadialBlurSamples] = 8;
        values[CameraFar] = 50;
        values[ColorDecalMax] = 200;
    }
    return values;
}

/// The preset the values are (Custom when none).
constexpr Preset PresetOf(const Values& values) {
    for (const Preset preset : {Preset::Default, Preset::Balanced, Preset::Performance}) {
        if (values == PresetValues(preset)) {
            return preset;
        }
    }
    return Preset::Custom;
}

/// A stored or given number: 0 unless it is within the knob's range.
constexpr int Sanitize(int value, const Knob& knob) {
    return value >= knob.min && value <= knob.max ? value : 0;
}

/// A variable's value: a whole number within the knob's range, nothing else around it. 0 when
/// unset, -1 when refused.
constexpr int Parse(const char* text, const Knob& knob) {
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    int value = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9' || i >= 3) {
            return -1;
        }
        value = value * 10 + (text[i] - '0');
    }
    return Sanitize(value, knob) != 0 ? value : -1;
}

/// "0" to "999", for the variables' values.
inline constexpr auto NumberTexts = [] {
    std::array<std::array<char, 4>, 1000> texts{};
    for (size_t i = 0; i < texts.size(); ++i) {
        size_t digits = i >= 100 ? 3 : i >= 10 ? 2 : 1;
        size_t rest = i;
        while (digits-- > 0) {
            texts[i][digits] = static_cast<char>('0' + rest % 10);
            rest /= 10;
        }
    }
    return texts;
}();

/// The variables the settings ask for, Bananza only.
inline std::vector<Core::Nx2PerfSettings::Var> Wanted(const Values& values, bool bananza) {
    std::vector<Core::Nx2PerfSettings::Var> vars;
    for (size_t i = 0; bananza && i < Count; ++i) {
        if (const int value = Sanitize(values[i], Knobs[i]); value != 0) {
            vars.push_back({Knobs[i].env, NumberTexts[static_cast<size_t>(value)].data()});
        }
    }
    return vars;
}

/// Reads every knob through `read(key, default)`.
template <typename Read>
Values Load(Read&& read) {
    Values values{};
    for (size_t i = 0; i < Count; ++i) {
        values[i] = Sanitize(read(Knobs[i].key, 0), Knobs[i]);
    }
    return values;
}

/// Writes every knob through `write(key, value, default)`.
template <typename Write>
void Save(const Values& values, Write&& write) {
    for (size_t i = 0; i < Count; ++i) {
        write(Knobs[i].key, values[i], 0);
    }
}

enum class State : u8 {
    Unset,   ///< not asked for
    Applied, ///< put in
    Missed,  ///< asked for, but the instructions are not where expected: left alone
};

using Result = std::array<State, Count>;

/// The game's own upscale rates (output size / render size), from its ProjectGraphicsParam:
/// 2880x1620 from 2095x1179 at best and 1440x810 at worst.
constexpr float GameMinRate = 1.375f;
constexpr float GameMaxRate = 2.0f;

/// Words of free space past the end of the text segment: the dynamic FPS patch has the first 19,
/// the terrain LOD routines start at word 20.
constexpr size_t CaveOffset = 20;
constexpr size_t CaveRoutineWords = 5;
constexpr size_t MeshRoutine = 4 * CaveRoutineWords; ///< after the terrain LOD routines
constexpr size_t MeshRoutineWords = 4;
constexpr size_t FarRoutine = MeshRoutine + MeshRoutineWords; ///< three, after the mesh one
constexpr size_t FarRoutineWords = 5;
constexpr size_t DebrisRoutine = FarRoutine + 3 * FarRoutineWords; ///< after the far ones
constexpr size_t DebrisRoutineWords = 4;
constexpr size_t DepthRoutine = DebrisRoutine + DebrisRoutineWords; ///< after the debris one
constexpr size_t DepthRoutineWords = 5;
constexpr size_t LodRoutine = DepthRoutine + DepthRoutineWords; ///< three, after the depth one
constexpr size_t LodRoutineWords = 5;
constexpr size_t CaveWords = LodRoutine + 3 * LodRoutineWords;

/// The upper half of a float, rounded: the constants are put in with one `movz wN, #x, lsl #16`.
constexpr u32 FloatHigh(float value) {
    return (std::bit_cast<u32>(value) + 0x8000U) >> 16;
}
constexpr u32 MovzHigh(u32 reg, u32 high) {
    return 0x52A00000U | (high << 5) | reg; // movz wN, #high, lsl #16
}
constexpr u32 Movz(u32 reg, u32 value) {
    return 0x52800000U | (value << 5) | reg; // movz wN, #value
}
constexpr u32 FmovFromW(u32 s_reg, u32 w_reg) {
    return 0x1E270000U | (w_reg << 5) | s_reg; // fmov sN, wM
}
constexpr u32 LdrX(u32 rt, u32 rn, u32 offset) {
    return 0xF9400000U | ((offset / 8) << 10) | (rn << 5) | rt; // ldr xT, [xN, #offset]
}
constexpr u32 LdrS(u32 rt, u32 rn, u32 offset) {
    return 0xBD400000U | ((offset / 4) << 10) | (rn << 5) | rt; // ldr sT, [xN, #offset]
}
constexpr u32 LdrW(u32 rt, u32 rn, u32 offset) {
    return 0xB9400000U | ((offset / 4) << 10) | (rn << 5) | rt; // ldr wT, [xN, #offset]
}
constexpr u32 LdrBW0X8(u32 offset) {
    return 0x39400100U | (offset << 10); // ldrb w0, [x8, #offset]
}
constexpr u32 Ret = 0xD65F03C0U;

// A graphics parameter is an object with its value at +0x64, held by GraphicsParam at a fixed
// offset; the game reads one with `ldr xA, [xB, #offset]; ldr sC, [xA, #0x64]`.
constexpr u32 ValueOffset = 0x64;
constexpr u32 MinRateParam = 0x5c8;      // NdsrUpscaleMinRate
constexpr u32 MaxRateParam = 0x5d0;      // NdsrUpscaleMaxRate
constexpr u32 GpuTargetParam = 0x538;       // MaintainGpuPercentage
constexpr u32 FrameMinScaleParam = 0x540;   // FrameBufferMinScale
constexpr u32 BloomScaleParam = 0x570;   // BloomBufferBaseScale
constexpr u32 CubeMapIntervalParam = 0x220; // CubeMapShotInterval
constexpr u32 RadialBlurScaleParam = 0x508; // RadialBlurRenderScale
constexpr u32 RadialBlurSamplesParam = 0x500; // RadialBlurSamplingNum
constexpr u32 ColorDecalMaxParam = 0x280;     // ColorDecalMaxNum
constexpr u32 GgxFilterParam = 0x210;         // IsUseGGXFilter
// The switches read in place, where the game builds its directors: the param's offset
constexpr std::array<std::pair<Id, u32>, 12> FeatureFlags{{
    {DecalOff, 0x80}, // IsUseDecal
    {DepthBloomOff, 0x118}, // IsUseDepthBloom
    {BloomExtensionOff, 0x110}, // IsUseBloomExtention
    {RadialBlurOff, 0xa0}, // IsUseRadialBlurDrawer
    {ThunderOff, 0xf8}, // IsUseThunderRenderer
    {CloudShapeOff, 0x188}, // IsUseCloudShape
    {DofIndirectOff, 0x128}, // IsUseDofIndirectCtrl
    {CubeMapOff, 0x28}, // IsUseCubeMap
    {FogOff, 0x60}, // IsUseFog
    {LightMapOff, 0x130}, // IsUseLightMapDirector
    {ModelForegroundOff, 0x160}, // IsUseModelForegroundDrawer
    {PrePassLightOff, 0xb0}, // IsUsePrePassLight
}};
constexpr u32 ShadowMaskParam = 0xe0;           // IsUseShadowMask
constexpr u32 CubeMapOcclusionParam = 0x218;  // IsUseCubeMapOcclusionCheck
constexpr u32 OcclusionDepthParam = 0x520;    // OcclusionQueryThresholdDepth
constexpr u32 OcclusionRadiusParam = 0x528;   // OcclusionQueryThresholdRadius
constexpr u32 OcclusionPolygonParam = 0x530;  // OcclusionQueryThresholdPolygon

// The game's values of the occlusion rows, from its ProjectGraphicsParam (Default).
constexpr float GameOcclusionRadius = 50.0f;
constexpr int GameOcclusionPolygon = 1000;

// `text` starts at the text segment and holds at least `aligned_size` bytes, the segment's size
// rounded up to whole pages; `text_size` is its size in the NSO header.
inline Result Apply(std::span<u8> text, u32 text_size, u32 aligned_size, const Values& values) {
    Result result{};
    bool any = false;
    // The rows from FoliageCap on are game data (nx2_dk_foliage.h), not instructions: not
    // marked here, except Debris off, which is
    for (size_t i = 0; i < Count; ++i) {
        if ((i < FoliageCap || i == DebrisOff) && Sanitize(values[i], Knobs[i]) != 0) {
            result[i] = State::Missed;
            any = true;
        }
    }
    if (!any || text.size() < aligned_size || aligned_size < text_size) {
        return result;
    }
    u8* const code = text.data();
    const size_t count = text_size / sizeof(u32);
    const auto word = [code](size_t index) {
        u32 value;
        std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto put = [code](size_t index, u32 value) {
        std::memcpy(code + index * sizeof(u32), &value, sizeof(value));
    };
    using Pattern = std::initializer_list<std::pair<size_t, u32>>; // word distance, word
    constexpr size_t Reach = 12;
    // The places a pattern is at, when there are exactly `out.size()` of them.
    const auto find = [&](Pattern pattern, std::span<size_t> out) {
        size_t found = 0;
        for (size_t i = 0; i + Reach < count; ++i) {
            bool same = true;
            for (const auto& [distance, value] : pattern) {
                if (word(i + distance) != value) {
                    same = false;
                    break;
                }
            }
            if (same) {
                if (found < out.size()) {
                    out[found] = i;
                }
                ++found;
            }
        }
        return found == out.size();
    };
    const auto set = [&](Id id) { return Sanitize(values[id], Knobs[id]); };
    const size_t cave = (text_size + 15) / 16 * 4 + CaveOffset;
    // Whether `words` words of the free space from word `first` on are unused.
    const auto CaveFree = [&](size_t first, size_t words) {
        bool free = (cave + CaveWords) * sizeof(u32) <= aligned_size;
        for (size_t i = first; free && i < first + words; ++i) {
            free = word(cave + i) == 0;
        }
        return free;
    };
    const auto percent = [](int value) { return static_cast<float>(value) / 100.0f; };

    // The resolution control steps the upscale rate between NdsrUpscaleMinRate and
    // NdsrUpscaleMaxRate; it reads each in two places:
    //   ldr x8, [x10, #0x5c8]; ldr s2, [x8, #0x64]            (x8 is loaded again next)
    //   ldr x10, [x10, #0x5c8]; 8 words; ldr s2, [x10, #0x64] (x10 unused between, set next)
    //   ldr x8, [x10, #0x5d0]; ldr s3, [x8, #0x64]
    //   ldr x10, [x10, #0x5d0]; 10 words; ldr s5, [x10, #0x64]
    if (const int best = set(ResBest), worst = set(ResWorst); best != 0 || worst != 0) {
        float min_rate = best != 0 ? 100.0f / static_cast<float>(best) : GameMinRate;
        float max_rate = worst != 0 ? 100.0f / static_cast<float>(worst) : GameMaxRate;
        // Best below worst: the one that was set decides (best, when both were).
        if (min_rate > max_rate) {
            if (best != 0) {
                max_rate = min_rate;
            } else {
                min_rate = max_rate;
            }
        }
        std::array<size_t, 1> min_near{}, min_far{}, max_near{}, max_far{};
        if (find({{0, LdrX(8, 10, MinRateParam)}, {1, LdrS(2, 8, ValueOffset)}}, min_near) &&
            find({{0, LdrX(10, 10, MinRateParam)}, {9, LdrS(2, 10, ValueOffset)}}, min_far) &&
            find({{0, LdrX(8, 10, MaxRateParam)}, {1, LdrS(3, 8, ValueOffset)}}, max_near) &&
            find({{0, LdrX(10, 10, MaxRateParam)}, {11, LdrS(5, 10, ValueOffset)}}, max_far)) {
            if (min_rate != GameMinRate) {
                const u32 high = FloatHigh(min_rate);
                put(min_near[0], MovzHigh(8, high));
                put(min_near[0] + 1, FmovFromW(2, 8));
                put(min_far[0], MovzHigh(10, high));
                put(min_far[0] + 9, FmovFromW(2, 10));
            }
            if (max_rate != GameMaxRate) {
                const u32 high = FloatHigh(max_rate);
                put(max_near[0], MovzHigh(8, high));
                put(max_near[0] + 1, FmovFromW(3, 8));
                put(max_far[0], MovzHigh(10, high));
                put(max_far[0] + 11, FmovFromW(5, 10));
            }
            if (best != 0) {
                result[ResBest] = State::Applied;
            }
            if (worst != 0) {
                result[ResWorst] = State::Applied;
            }
        }
    }

    // Getters: `ldr x8, [x0, #offset]; ldr s0 / w0, [x8, #0x64]; ret`.
    const auto float_getter = [&](Id id, u32 offset, float value) {
        std::array<size_t, 1> at{};
        if (set(id) != 0 &&
            find({{0, LdrX(8, 0, offset)}, {1, LdrS(0, 8, ValueOffset)}, {2, Ret}}, at)) {
            put(at[0], MovzHigh(8, FloatHigh(value)));
            put(at[0] + 1, FmovFromW(0, 8));
            result[id] = State::Applied;
        }
    };
    const auto int_getter = [&](Id id, u32 offset, int value = 0) {
        std::array<size_t, 1> at{};
        if (set(id) != 0 &&
            find({{0, LdrX(8, 0, offset)}, {1, LdrW(0, 8, ValueOffset)}, {2, Ret}}, at)) {
            put(at[0], Movz(0, static_cast<u32>(value != 0 ? value : set(id))));
            put(at[0] + 1, Ret);
            result[id] = State::Applied;
        }
    };
    // A boolean getter: `ldr x8, [x0, #offset]; ldrb w0, [x8, #0x64]; ret`, forced to false.
    const auto false_getter = [&](Id id, u32 offset) {
        std::array<size_t, 1> at{};
        if (set(id) != 0 &&
            find({{0, LdrX(8, 0, offset)}, {1, LdrBW0X8(ValueOffset)}, {2, Ret}}, at)) {
            put(at[0], Movz(0, 0));
            put(at[0] + 1, Ret);
            result[id] = State::Applied;
        }
    };
    false_getter(GgxFilterOff, GgxFilterParam);
    false_getter(CubeMapOcclusionOff, CubeMapOcclusionParam);
    false_getter(ShadowMaskOff, ShadowMaskParam);
    float_getter(OcclusionRadius, OcclusionRadiusParam,
                 GameOcclusionRadius * percent(set(OcclusionRadius)));
    float_getter(GpuTarget, GpuTargetParam, static_cast<float>(set(GpuTarget)));
    float_getter(FrameMinScale, FrameMinScaleParam, percent(set(FrameMinScale)));
    float_getter(RadialBlurScale, RadialBlurScaleParam, percent(set(RadialBlurScale)));
    int_getter(RadialBlurSamples, RadialBlurSamplesParam);
    int_getter(CubeMapInterval, CubeMapIntervalParam);
    int_getter(ColorDecalMax, ColorDecalMaxParam);
    int_getter(OcclusionPolygon, OcclusionPolygonParam,
               GameOcclusionPolygon * set(OcclusionPolygon) / 100);

    // BloomBufferBaseScale is read in three places; the register it comes through is loaded
    // again right after each:
    //   ldr x8, [x8, #0x570]; ldr s1, [x8, #0x64]
    //   ldr x11, [x26, #0x570]; ldr s0, [x11, #0x64]   (twice)
    if (set(BloomScale) != 0) {
        std::array<size_t, 1> first{};
        std::array<size_t, 2> others{};
        if (find({{0, LdrX(8, 8, BloomScaleParam)}, {1, LdrS(1, 8, ValueOffset)}}, first) &&
            find({{0, LdrX(11, 26, BloomScaleParam)}, {1, LdrS(0, 11, ValueOffset)}}, others)) {
            const u32 high = FloatHigh(percent(set(BloomScale)));
            put(first[0], MovzHigh(8, high));
            put(first[0] + 1, FmovFromW(1, 8));
            for (const size_t at : others) {
                put(at, MovzHigh(11, high));
                put(at + 1, FmovFromW(0, 11));
            }
            result[BloomScale] = State::Applied;
        }
    }

    // The terrain (voxel field) takes its two LOD steps from VoxelMeshLodParam, one place (LOD 1
    // is the nearer step, LOD 2 the farther one):
    //   bl Lod1Dist;  ldr s8, [x0];  mov x0, x24
    //   bl Lod1Value; ldr s9, [x0];  mov x0, x24
    //   bl Lod2Dist;  ldr s10, [x0]; mov x0, x24
    //   bl Lod2Value; ldr s0, [x0]
    // Each load asked for becomes a call to a routine in the free space past the text segment
    // that loads and scales (x16, s16 and x30 are free right after a call):
    //   ldr sN, [x0]; movz w16, #scale, lsl #16; fmov s16, w16; fmul sN, sN, s16; ret
    const std::array<int, 4> lod{set(VoxelLod1Dist), set(VoxelLod1Value), set(VoxelLod2Dist),
                                 set(VoxelLod2Value)};
    if (lod[0] != 0 || lod[1] != 0 || lod[2] != 0 || lod[3] != 0) {
        constexpr u32 MovX0X24 = 0xAA1803E0U;
        std::array<size_t, 1> at{};
        const bool cave_free = CaveFree(0, MeshRoutine);
        if (cave_free && find({{1, LdrS(8, 0, 0)},
                               {2, MovX0X24},
                               {4, LdrS(9, 0, 0)},
                               {5, MovX0X24},
                               {7, LdrS(10, 0, 0)},
                               {8, MovX0X24},
                               {10, LdrS(0, 0, 0)}},
                              at)) {
            struct Load {
                size_t distance;
                u32 reg;
                int scale;
            };
            const std::array<Load, 4> loads{
                {{1, 8, lod[0]}, {4, 9, lod[1]}, {7, 10, lod[2]}, {10, 0, lod[3]}}};
            for (size_t i = 0; i < loads.size(); ++i) {
                const Load& load = loads[i];
                if (load.scale == 0) {
                    continue;
                }
                const size_t from = at[0] + load.distance;
                const size_t routine = cave + i * CaveRoutineWords;
                put(routine, LdrS(load.reg, 0, 0));
                put(routine + 1, MovzHigh(16, FloatHigh(percent(load.scale))));
                put(routine + 2, FmovFromW(16, 16));
                put(routine + 3, 0x1E300800U | (load.reg << 5) | load.reg); // fmul sN, sN, s16
                put(routine + 4, Ret);
                put(from, 0x94000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU)); // bl
            }
            const std::array<Id, 4> ids{VoxelLod1Dist, VoxelLod1Value, VoxelLod2Dist, VoxelLod2Value};
            for (size_t i = 0; i < ids.size(); ++i) {
                if (lod[i] != 0) {
                    result[ids[i]] = State::Applied;
                }
            }
        }
    }

    // The terrain's mesh simplification type (VoxelMeshLodParam's MeshSimplifyType: Normal 0,
    // High 1, SuperHigh 2, NoSimplify 3, Low 4) is stored in the loader and read after the one
    // place all of its paths meet:
    //   str w8, [sp, #0x94]; ldr x0, [x19, #0x60]
    // The load becomes a jump to a routine that stores the chosen type first (w8 is loaded
    // again right after):
    //   movz w8, #type; str w8, [sp, #0x94]; ldr x0, [x19, #0x60]; b back
    if (const int type = set(MeshSimplify); type != 0) {
        constexpr u32 StrW8Sp94 = 0xB90097E8U;
        constexpr u32 LdrX0X19 = 0xF9403260U;
        std::array<size_t, 1> at{};
        if (CaveFree(MeshRoutine, MeshRoutineWords) && find({{0, StrW8Sp94}, {1, LdrX0X19}}, at)) {
            const size_t routine = cave + MeshRoutine;
            const size_t from = at[0] + 1;
            put(routine, Movz(8, static_cast<u32>(type - 1)));
            put(routine + 1, StrW8Sp94);
            put(routine + 2, LdrX0X19);
            put(routine + 3,
                0x14000000U | (static_cast<u32>((from + 1) - (routine + 3)) & 0x3FFFFFFU));
            put(from, 0x14000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU));
            result[MeshSimplify] = State::Applied;
        }
    }

    // The camera's far distance (the Selevet fork's EDEN_NX2_FAR_SCALE, from its pull request 15):
    // three places load a fresh far into s1 before the camera setter. Each instruction becomes a
    // jump to a routine that runs it, then multiplies s1 by the scale; s2 is written by the next
    // instruction that uses it at all three, so the routine borrows it:
    //   <the instruction>; ldr s2, <scale>; fmul s1, s1, s2; b back; <scale>
    // Beyond the far the game does not draw: a low scale drops the scenery in the distance.
    if (const int far_percent = set(CameraFar); far_percent != 0) {
        constexpr u32 FmovS0S8 = 0x1E204100U, FmovS1S9 = 0x1E204121U, AddX2Sp8 = 0x910023E2U;
        constexpr u32 FmovS2S10 = 0x1E204142U, FaddS1S1S2 = 0x1E222821U;
        constexpr u32 LdrS2Literal = 0x1C000062U; // ldr s2, #12: the word after the branch
        constexpr u32 FmulS1S1S2 = 0x1E220821U;
        std::array<size_t, 1> first{}, second{}, third{};
        if (CaveFree(FarRoutine, 3 * FarRoutineWords) &&
            find({{0, FmovS0S8}, {1, FmovS1S9}, {2, AddX2Sp8}, {3, FmovS2S10}}, first) &&
            find({{0, FmovS0S8}, {1, AddX2Sp8}, {2, FmovS1S9}, {3, FmovS2S10}, {4, 0xAA1303E0U}},
                 second) &&
            find({{0, 0xAA1503E0U}, {1, FaddS1S1S2}, {2, 0x2D418D02U}}, third)) {
            const std::array<size_t, 3> sites{first[0] + 1, second[0] + 2, third[0] + 1};
            const float scale = percent(far_percent);
            for (size_t i = 0; i < sites.size(); ++i) {
                const size_t routine = cave + FarRoutine + i * FarRoutineWords;
                put(routine, word(sites[i]));
                put(routine + 1, LdrS2Literal);
                put(routine + 2, FmulS1S1S2);
                put(routine + 3, 0x14000000U |
                                     (static_cast<u32>((sites[i] + 1) - (routine + 3)) &
                                      0x3FFFFFFU));
                put(routine + 4, std::bit_cast<u32>(scale));
                put(sites[i],
                    0x14000000U | (static_cast<u32>(routine - sites[i]) & 0x3FFFFFFU));
            }
            result[CameraFar] = State::Applied;
        }
    }

    // The switches read in place: `ldr xM, [xK, #offset]; ldrb wN, [xM, #0x64]`, each with
    // exactly one such pair. The load of the value becomes `movz wN, #0`.
    for (const auto& [id, offset] : FeatureFlags) {
        if (set(id) == 0) {
            continue;
        }
        size_t found = 0, at = 0;
        for (size_t i = 0; i + 1 < count; ++i) {
            const u32 first = word(i), second = word(i + 1);
            if ((first & 0xFFC00000U) == 0xF9400000U && ((first >> 10) & 0xFFFU) * 8 == offset &&
                (second & 0xFFFFFC00U) == (0x39400000U | (ValueOffset << 10)) &&
                ((second >> 5) & 31U) == (first & 31U)) {
                at = i + 1;
                ++found;
            }
        }
        if (found == 1) {
            put(at, Movz(word(at) & 31U, 0));
            result[id] = State::Applied;
        }
    }

    // The terrain's IsNoBorderMesh is read into [sp, #0x7c] and used once, after the call:
    //   ldrb w8, [sp, #0x7c]; ldr x6, [sp, #0x28]; cmp w8, #0; cset w8, eq
    // The load becomes `mov w8, #1`: no border mesh between neighbouring chunks (cracks possible).
    if (set(VoxelBorderMeshOff) != 0) {
        std::array<size_t, 1> at{};
        if (find({{0, 0x3941F3E8U}, {1, 0xF94017E6U}, {2, 0x7100011FU}, {3, 0x1A9F17E8U}}, at)) {
            put(at[0], Movz(8, 1));
            result[VoxelBorderMeshOff] = State::Applied;
        }
    }

    // OcclusionQueryThresholdDepth differs between the graphics variants (90 in the stages, 50
    // elsewhere), so it is scaled rather than replaced: the getter's load becomes a jump to
    //   ldr s0, [x8, #0x64]; ldr s1, <scale>; fmul s0, s0, s1; ret; <scale>
    if (const int depth = set(OcclusionDepth); depth != 0) {
        constexpr u32 LdrS1Literal = 0x1C000061U; // ldr s1, #12: the word after the ret
        constexpr u32 FmulS0S0S1 = 0x1E210800U;
        std::array<size_t, 1> at{};
        if (CaveFree(DepthRoutine, DepthRoutineWords) &&
            find({{0, LdrX(8, 0, OcclusionDepthParam)}, {1, LdrS(0, 8, ValueOffset)}, {2, Ret}},
                 at)) {
            const size_t routine = cave + DepthRoutine;
            const size_t from = at[0] + 1;
            put(routine, LdrS(0, 8, ValueOffset));
            put(routine + 1, LdrS1Literal);
            put(routine + 2, FmulS0S0S1);
            put(routine + 3, Ret);
            u32 scale;
            const float factor = percent(depth);
            std::memcpy(&scale, &factor, sizeof(scale));
            put(routine + 4, scale);
            put(from, 0x14000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU));
            result[OcclusionDepth] = State::Applied;
        }
    }

    // A model's own LOD thresholds (its ModelParam's LodThreshold0-2, which a model with
    // IsUseUniqueLodThreshold, Donkey Kong's say, uses instead of the graphics parameters') are
    // copied into the model as it loads; a parent's are loaded at the same three places:
    //   ldr s0, [x20, #0xac]; ldr x8, [x19, #0x268]; str s0, [x8]      (and #0xb0 / #4, #0xb4 / #8)
    // Each store becomes a jump to a routine that scales s0 first (s1 is not used there):
    //   ldr s1, <scale>; fmul s0, s0, s1; str s0, [x8, #k]; b back; <scale>
    // A threshold is the screen size above which the finer mesh is used: higher, coarser sooner.
    if (const int character_lod = set(CharacterLod); character_lod != 0) {
        constexpr u32 LdrX8X19Lods = 0xF9413668U;     // ldr x8, [x19, #0x268]
        constexpr u32 LdrS1Literal = 0x1C000081U;     // ldr s1, #16: the word after the b
        constexpr u32 FmulS0S0S1 = 0x1E210800U;
        constexpr std::array<std::pair<u32, u32>, 3> sites{{
            {0xac, 0xBD000100U}, // str s0, [x8]
            {0xb0, 0xBD000500U}, // str s0, [x8, #4]
            {0xb4, 0xBD000900U}, // str s0, [x8, #8]
        }};
        std::array<std::array<size_t, 1>, 3> at{};
        bool found = CaveFree(LodRoutine, 3 * LodRoutineWords);
        for (size_t i = 0; found && i < sites.size(); ++i) {
            found = find({{0, LdrS(0, 20, sites[i].first)}, {1, LdrX8X19Lods}, {2, sites[i].second}},
                          at[i]);
        }
        if (found) {
            const float factor = percent(character_lod);
            u32 scale;
            std::memcpy(&scale, &factor, sizeof(scale));
            for (size_t i = 0; i < sites.size(); ++i) {
                const size_t routine = cave + LodRoutine + i * LodRoutineWords;
                const size_t from = at[i][0] + 2;
                put(routine, LdrS1Literal);
                put(routine + 1, FmulS0S0S1);
                put(routine + 2, sites[i].second);
                put(routine + 3,
                    0x14000000U | (static_cast<u32>((from + 1) - (routine + 3)) & 0x3FFFFFFU));
                put(routine + 4, scale);
                put(from, 0x14000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU));
            }
            result[CharacterLod] = State::Applied;
        }
    }

    // An actor's ClippingParam names a FarClipLevel (NonClip, Default, EasyClipLv1-3,
    // HardClipLv1-5); the distance of each level is a table of the read-only data:
    //   -1 (never clipped), 200, 50, 100, 150, 300, 450, 700, 900, 1200
    // Each distance but NonClip's is scaled in place.
    if (const int distance = set(ActorDrawDistance); distance != 0) {
        constexpr std::array<float, 10> Table{-1.0f, 200.0f, 50.0f,  100.0f, 150.0f,
                                              300.0f, 450.0f, 700.0f, 900.0f, 1200.0f};
        std::array<u8, sizeof(Table)> bytes{};
        std::memcpy(bytes.data(), Table.data(), bytes.size());
        size_t found = 0, at = 0;
        for (auto it = std::search(text.begin(), text.end(), bytes.begin(), bytes.end());
             it != text.end() && found < 2;
             it = std::search(it + 4, text.end(), bytes.begin(), bytes.end())) {
            if ((it - text.begin()) % 4 == 0) {
                at = static_cast<size_t>(it - text.begin());
                ++found;
            }
        }
        if (found == 1) {
            for (size_t i = 1; i < Table.size(); ++i) {
                const float scaled = Table[i] * percent(distance);
                std::memcpy(text.data() + at + i * sizeof(float), &scaled, sizeof(scaled));
            }
            result[ActorDrawDistance] = State::Applied;
        }
    }

    // The voxel effect emitter's settings start with the effect keys it emits when terrain is
    // damaged and destroyed ("Damage", "Destruction"; the effect settings file sets neither):
    //   add x8, x8, #0x16e ("Damage"); adrp x9; add x9, x9, #0x757 ("Destruction");
    //   strb wzr, [x19, #0x46]; stp x8, x9, [x19, #0x30]; mov w8, #0x200
    // A key off points at its own string's terminating zero instead: an empty key, no effect.
    if (set(VoxelDamageOff) != 0 || set(VoxelDestructionOff) != 0) {
        constexpr u32 AddDamage = 0x9105B908U, AddDestruction = 0x911D5D29U;
        constexpr u32 StrbFlag = 0x39011A7FU, MovBuffer = 0x52804008U;
        std::array<size_t, 1> at{};
        if (find({{0, AddDamage}, {2, AddDestruction}, {3, StrbFlag}, {5, MovBuffer}}, at)) {
            const auto empty = [&](size_t index, u32 length) {
                // The add's immediate moves past the string's characters
                const u32 add = word(index);
                put(index, (add & ~(0xFFFU << 10)) | ((((add >> 10) & 0xFFFU) + length) << 10));
            };
            if (set(VoxelDamageOff) != 0) {
                empty(at[0], 6); // "Damage"
                result[VoxelDamageOff] = State::Applied;
            }
            if (set(VoxelDestructionOff) != 0) {
                empty(at[0] + 2, 11); // "Destruction"
                result[VoxelDestructionOff] = State::Applied;
            }
        }
    }

    // A voxel material type's FragmentType (NoOverwrite 0, EnableForce 1, DisableForce 2) is a
    // word at +0xa0 of the material, which the loader binds as its 16th field (index 0xf):
    //   mov w8, #0xf; 13 words; add x8, x20, #0xc0 (the next field)
    // The add becomes a jump to a routine that stores DisableForce first (x9 is set next):
    //   movz w9, #2; str w9, [x20, #0xa0]; add x8, x20, #0xc0; b back
    if (set(DebrisOff) != 0) {
        constexpr u32 MovW8Field = 0x528001E8U, AddX8X20Next = 0x91030288U;
        constexpr u32 StrW9X20Fragment = 0xB900A289U;
        std::array<size_t, 1> at{};
        if (CaveFree(DebrisRoutine, DebrisRoutineWords) &&
            find({{0, MovW8Field}, {14, AddX8X20Next}}, at)) {
            const size_t routine = cave + DebrisRoutine;
            const size_t from = at[0] + 14;
            put(routine, Movz(9, 2));
            put(routine + 1, StrW9X20Fragment);
            put(routine + 2, AddX8X20Next);
            put(routine + 3,
                0x14000000U | (static_cast<u32>((from + 1) - (routine + 3)) & 0x3FFFFFFU));
            put(from, 0x14000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU));
            result[DebrisOff] = State::Applied;
        }
    }
    return result;
}

/// The knobs' values from the environment through `get(name)`; a refused one is left at 0 and
/// reported through `refused(knob, text)`.
template <typename Get, typename Refused>
Values FromEnvironment(Get&& get, Refused&& refused) {
    Values values{};
    for (size_t i = 0; i < Count; ++i) {
        const char* const text = get(Knobs[i].env);
        const int value = Parse(text, Knobs[i]);
        if (value < 0) {
            refused(Knobs[i], text);
        } else {
            values[i] = value;
        }
    }
    return values;
}

/// Whether a module is the one the settings are for: Bananza's main.
constexpr bool IsTarget(u64 program_id, std::string_view module_name) {
    return module_name == "main" && program_id == Nx2DkDynamicFps::ProgramId;
}

} // namespace Loader::Nx2DkQuality
