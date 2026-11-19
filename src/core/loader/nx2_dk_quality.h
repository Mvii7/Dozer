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
    CameraFar,         ///< the camera's far distance, % (EDEN_NX2_FAR_SCALE of the Selevet fork)
    ColorDecalMax,     ///< ColorDecalMaxNum
    ActorDrawDistance, ///< the actors' far clip distances (FarClipLevel), % of the game's
    NonClipDistance,   ///< a far clip distance for the never clipped actors (NonClip, or
                       ///< clipping invalidated), x10 units
    ShadowCascades,    ///< the most sun shadow cascades (depth_shadow_cascade_num)
    ShadowDistance,    ///< the sun shadow's far distance (depth_shadow_far), % of the game's
    GgxFilterOff,      ///< IsUseGGXFilter forced off (reflection cube map filtering)
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
    EffectEmitCap,     ///< EmitCountPerFrameMax (effects started per frame), % of the game's
    EffectParticleCap, ///< OneEmitterMaxParticleNum, % of the game's
    EffectAliveCap,    ///< the MaxAliveEffectsNum of the effect rules, % of the game's
    EffectMaterialCap, ///< MaxMatEffActiveUsersNumPerSlot and each MaxActiveEffectNum, %
    EffectEmitOff,     ///< effects started per frame at 1
    EffectParticleOff, ///< particles per emitter at 1
    EffectAliveOff,    ///< effects alive per rule at 1
    EffectMaterialOff, ///< terrain material effects at 1
    SceneClipOn,       ///< each stage's IsEnabledFarClipping on and FarClippingDistanceOffset 0
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
    {"camera_far", "EDEN_NX2_DKQ_CAMERA_FAR", 10, 100, 100},
    {"color_decal_max", "EDEN_NX2_DKQ_COLOR_DECAL_MAX", 1, 660, 660},
    {"actor_draw_distance", "EDEN_NX2_DKQ_ACTOR_DRAW_DISTANCE", 10, 100, 100},
    {"nonclip_distance", "EDEN_NX2_DKQ_NONCLIP_DISTANCE", 10, 300, 300},
    {"shadow_cascades", "EDEN_NX2_DKQ_SHADOW_CASCADES", 1, 4, 4},
    {"shadow_distance", "EDEN_NX2_DKQ_SHADOW_DISTANCE", 10, 100, 100},
    {"ggx_filter_off", "EDEN_NX2_DKQ_GGX_FILTER_OFF", 1, 1, 0},
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
    {"effect_emit_cap", "EDEN_NX2_DKQ_EFFECT_EMIT_CAP", 5, 100, 100},
    {"effect_particle_cap", "EDEN_NX2_DKQ_EFFECT_PARTICLE_CAP", 5, 100, 100},
    {"effect_alive_cap", "EDEN_NX2_DKQ_EFFECT_ALIVE_CAP", 5, 100, 100},
    {"effect_material_cap", "EDEN_NX2_DKQ_EFFECT_MATERIAL_CAP", 5, 100, 100},
    {"effect_emit_off", "EDEN_NX2_DKQ_EFFECT_EMIT_OFF", 1, 1, 0},
    {"effect_particle_off", "EDEN_NX2_DKQ_EFFECT_PARTICLE_OFF", 1, 1, 0},
    {"effect_alive_off", "EDEN_NX2_DKQ_EFFECT_ALIVE_OFF", 1, 1, 0},
    {"effect_material_off", "EDEN_NX2_DKQ_EFFECT_MATERIAL_OFF", 1, 1, 0},
    {"scene_clip_on", "EDEN_NX2_DKQ_SCENE_CLIP_ON", 1, 1, 0},
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

/// A preset's values (0: the game's). Custom has none of its own: nothing set.
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
/// the camera far routines start at word 20.
constexpr size_t CaveOffset = 20;
constexpr size_t FarRoutine = 0; ///< three
constexpr size_t FarRoutineWords = 5;
constexpr size_t DebrisRoutine = FarRoutine + 3 * FarRoutineWords; ///< after the far ones
constexpr size_t DebrisRoutineWords = 4;
constexpr size_t CascadeRoutine = DebrisRoutine + DebrisRoutineWords; ///< after the debris one
constexpr size_t CascadeRoutineWords = 5;
constexpr size_t ShadowFarRoutine = CascadeRoutine + CascadeRoutineWords; ///< three
constexpr size_t ShadowFarRoutineWords = 5;
/// Thirteen after one shared distance, past words 64-68 of the free space (Shadow quality's).
constexpr size_t InvalidRoutine = 50;
constexpr size_t InvalidSites = 13;
constexpr size_t InvalidRoutineWords = 4;
constexpr size_t CaveWords = InvalidRoutine + 1 + InvalidSites * InvalidRoutineWords;

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
    false_getter(ShadowMaskOff, ShadowMaskParam);
    float_getter(GpuTarget, GpuTargetParam, static_cast<float>(set(GpuTarget)));
    float_getter(FrameMinScale, FrameMinScaleParam, percent(set(FrameMinScale)));
    float_getter(RadialBlurScale, RadialBlurScaleParam, percent(set(RadialBlurScale)));
    int_getter(RadialBlurSamples, RadialBlurSamplesParam);
    int_getter(CubeMapInterval, CubeMapIntervalParam);
    int_getter(ColorDecalMax, ColorDecalMaxParam);

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

    // An actor's ClippingParam names a FarClipLevel (NonClip, Default, EasyClipLv1-3,
    // HardClipLv1-5); the distance of each level is a table of the read-only data:
    //   -1 (never clipped), 200, 50, 100, 150, 300, 450, 700, 900, 1200
    // Each distance but NonClip's is scaled in place; NonClip's -1 can be given a distance of its
    // own (the big level models and landmarks use it). An actor whose ClippingParam invalidates its
    // clipping (flag bit 3) skips the table for a -1 of the code's own, in thirteen places:
    //   fmov s0, #-1.0; up to 4 words; tbnz wR, #3, store; ...; store: ldr x0, [xN, #0x18];
    //   str s0, [xN, #0x54]
    // With a NonClip distance each tbnz becomes a jump to a routine that loads it first:
    //   tbz wR, #3, +12; ldr s0, <distance>; b store; b back   (the distance shared, before them)
    const auto PutInvalidDistance = [&](float distance) {
        constexpr u32 FmovMinusOne = 0x1E3E1000U; // fmov s0, #-1.0
        std::array<std::array<size_t, 3>, InvalidSites> sites{}; // tbnz, store, register
        size_t found = 0;
        for (size_t i = 0; i + Reach < count; ++i) {
            if (word(i) != FmovMinusOne) {
                continue;
            }
            for (size_t j = i + 1; j <= i + 5; ++j) {
                const u32 test = word(j);
                if ((test & 0xFFF80000U) != 0x37180000U) { // tbnz wR, #3
                    continue;
                }
                const size_t store = j + ((test >> 5) & 0x3FFFU);
                if (store + 1 < count && (word(store) & 0xFFFFFC1FU) == 0xF9400C00U &&
                    word(store + 1) == (0xBD005400U | (word(store) & 0x3E0U))) {
                    if (found < sites.size()) {
                        sites[found] = {j, store, test & 31U};
                    }
                    ++found;
                }
                break;
            }
        }
        if (found != sites.size() ||
            !CaveFree(InvalidRoutine, 1 + InvalidSites * InvalidRoutineWords)) {
            return;
        }
        const size_t literal = cave + InvalidRoutine;
        put(literal, std::bit_cast<u32>(distance));
        const auto branch = [](size_t from, size_t to) {
            return 0x14000000U | (static_cast<u32>(to - from) & 0x3FFFFFFU);
        };
        for (size_t i = 0; i < sites.size(); ++i) {
            const auto [from, store, reg] = sites[i];
            const size_t routine = literal + 1 + i * InvalidRoutineWords;
            put(routine, 0x36180060U | reg); // tbz wR, #3, +12
            put(routine + 1,
                0x1C000000U | ((static_cast<u32>(literal - (routine + 1)) & 0x7FFFFU) << 5));
            put(routine + 2, branch(routine + 2, store));
            put(routine + 3, branch(routine + 3, from + 1));
            put(from, branch(from, routine));
        }
    };
    const int actor_distance = set(ActorDrawDistance), nonclip = set(NonClipDistance);
    if (actor_distance != 0 || nonclip != 0) {
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
            if (actor_distance != 0) {
                for (size_t i = 1; i < Table.size(); ++i) {
                    const float scaled = Table[i] * percent(actor_distance);
                    std::memcpy(text.data() + at + i * sizeof(float), &scaled, sizeof(scaled));
                }
                result[ActorDrawDistance] = State::Applied;
            }
            if (nonclip != 0) {
                const float nonclip_far = static_cast<float>(nonclip) * 10.0f;
                std::memcpy(text.data() + at, &nonclip_far, sizeof(nonclip_far));
                result[NonClipDistance] = State::Applied;
                PutInvalidDistance(nonclip_far);
            }
        }
    }

    // The sun shadow's settings (gsys depth shadow: depth_shadow_cascade_num_0 at +0x1a48 of the
    // settings, the split distances after it, depth_shadow_far_0 at +0x1ae8) are set up for each
    // shadow, the count clamped to the shadow's own most:
    //   ldr w8, [x27, #0x1a48]; cmp w8, w9; csel w8, w8, w9, lt
    // The load becomes a jump to a routine that caps it first (the flags are set again next):
    //   ldr w8, [x27, #0x1a48]; cmp w8, #n; b.le +8; movz w8, #n; b back
    // Each cascade draws the shadow casters again.
    if (const int cascades = set(ShadowCascades); cascades != 0) {
        constexpr u32 LdrCount = 0xB95A4B68U; // ldr w8, [x27, #0x1a48]
        std::array<size_t, 1> at{};
        if (CaveFree(CascadeRoutine, CascadeRoutineWords) &&
            find({{0, LdrCount}, {1, 0x6B09011FU}, {2, 0x1A89B108U}}, at)) {
            const size_t routine = cave + CascadeRoutine;
            const size_t from = at[0];
            const u32 n = static_cast<u32>(cascades);
            put(routine, LdrCount);
            put(routine + 1, 0x7100011FU | (n << 10)); // cmp w8, #n
            put(routine + 2, 0x5400004DU); // b.le +8: past the movz, to the jump back
            put(routine + 3, Movz(8, n));
            put(routine + 4,
                0x14000000U | (static_cast<u32>((from + 1) - (routine + 4)) & 0x3FFFFFFU));
            put(from, 0x14000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU));
            result[ShadowCascades] = State::Applied;
        }
    }

    // The shadow's far distance is put after the last cascade's split in three places, each
    // `ldr s0, [xN, #0x1ae8]` followed by one known word. Each load becomes a jump to a routine
    // that scales it (s31 is used nowhere in those functions):
    //   ldr s0, [xN, #0x1ae8]; ldr s31, <scale>; fmul s0, s0, s31; b back; <scale>
    // Shorter: the far cascade covers less, so fewer casters are drawn into it.
    if (const int distance = set(ShadowDistance); distance != 0) {
        constexpr std::array<std::pair<u32, u32>, 3> sites{{
            {0xBD5AE940U, 0xB9822AC8U}, // ldr s0, [x10, #0x1ae8]; ldrsw x8, [x22, #0x228]
            {0xBD5AE900U, 0x9BAA674AU}, // ldr s0, [x8, #0x1ae8]; umaddl x10, w26, w10, x25
            {0xBD5AEB60U, 0xB9822AE8U}, // ldr s0, [x27, #0x1ae8]; ldrsw x8, [x23, #0x228]
        }};
        std::array<std::array<size_t, 1>, 3> at{};
        bool found = CaveFree(ShadowFarRoutine, 3 * ShadowFarRoutineWords);
        for (size_t i = 0; found && i < sites.size(); ++i) {
            found = find({{0, sites[i].first}, {1, sites[i].second}}, at[i]);
        }
        if (found) {
            const float factor = percent(distance);
            u32 scale;
            std::memcpy(&scale, &factor, sizeof(scale));
            for (size_t i = 0; i < sites.size(); ++i) {
                const size_t routine = cave + ShadowFarRoutine + i * ShadowFarRoutineWords;
                const size_t from = at[i][0];
                put(routine, sites[i].first);
                put(routine + 1, 0x1C00007FU); // ldr s31, #12
                put(routine + 2, 0x1E3F0800U); // fmul s0, s0, s31
                put(routine + 3,
                    0x14000000U | (static_cast<u32>((from + 1) - (routine + 3)) & 0x3FFFFFFU));
                put(routine + 4, scale);
                put(from, 0x14000000U | (static_cast<u32>(routine - from) & 0x3FFFFFFU));
            }
            result[ShadowDistance] = State::Applied;
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
