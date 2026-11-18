// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_quality.h"

namespace {
using namespace Loader::Nx2DkQuality;

constexpr u32 Filler = 0x12345678;
// A text segment of 0x2e80 bytes in 0x3000: the free space starts at word 0xba0, the routines
// at 0xba0 + CaveOffset.
constexpr u32 TextSize = 0x2e80;
constexpr u32 AlignedSize = 0x3000;
constexpr size_t Cave = 0xba0 + CaveOffset;
constexpr size_t MinNear = 0x100, MaxFar = 0x120, MaxNear = 0x140, MinFar = 0x160;
constexpr size_t GpuGetter = 0x200, FrameGetter = 0x210, BlurScaleGetter = 0x220;
constexpr size_t BlurSamplesGetter = 0x230, CubeMapGetter = 0x240;
constexpr size_t BloomFirst = 0x300, BloomSecond = 0x310, BloomThird = 0x320;
constexpr size_t DecalGetter = 0x260, GgxGetter = 0x270;
constexpr size_t FarA = 0x600, FarB = 0x620, FarC = 0x640;
constexpr size_t DebrisSite = 0x780, CascadeSite = 0x7a0, ShadowFarSites = 0x7c0;
constexpr size_t BorderSite = 0x740, ShadowMaskGetter = 0x760,
    FlagSite = 0x800;

constexpr u32 ShadowFarLoads[3][2]{
    {0xBD5AE940, 0xB9822AC8}, {0xBD5AE900, 0x9BAA674A}, {0xBD5AEB60, 0xB9822AE8}};

struct Text {
    std::vector<u32> words;

    Text() : words(AlignedSize / 4, 0) {
        for (size_t i = 0; i < TextSize / 4; ++i) {
            words[i] = Filler;
        }
        words[MinNear] = LdrX(8, 10, MinRateParam);
        words[MinNear + 1] = LdrS(2, 8, ValueOffset);
        words[MinFar] = LdrX(10, 10, MinRateParam);
        words[MinFar + 9] = LdrS(2, 10, ValueOffset);
        words[MaxNear] = LdrX(8, 10, MaxRateParam);
        words[MaxNear + 1] = LdrS(3, 8, ValueOffset);
        words[MaxFar] = LdrX(10, 10, MaxRateParam);
        words[MaxFar + 11] = LdrS(5, 10, ValueOffset);
        PutGetter(GpuGetter, GpuTargetParam, true);
        PutGetter(FrameGetter, FrameMinScaleParam, true);
        PutGetter(BlurScaleGetter, RadialBlurScaleParam, true);
        words[GgxGetter] = LdrX(8, 0, GgxFilterParam);
        words[GgxGetter + 1] = 0x39419100; // ldrb w0, [x8, #0x64]
        words[GgxGetter + 2] = Ret;
        words[ShadowMaskGetter] = LdrX(8, 0, ShadowMaskParam);
        words[ShadowMaskGetter + 1] = 0x39419100; // ldrb w0, [x8, #0x64]
        words[ShadowMaskGetter + 2] = Ret;
        for (size_t i = 0; i < FeatureFlags.size(); ++i) {
            words[FlagSite + i * 4] = 0xF9400000U | ((FeatureFlags[i].second / 8) << 10) | (9 << 5) | 10;
            words[FlagSite + i * 4 + 1] = 0x39419000U | (10 << 5) | 11; // ldrb w11, [x10, #0x64]
        }
        PutGetter(BlurSamplesGetter, RadialBlurSamplesParam, false);
        PutGetter(CubeMapGetter, CubeMapIntervalParam, false);
        PutGetter(DecalGetter, ColorDecalMaxParam, false);
        const u32 first[]{0x1E204100, 0x1E204121, 0x910023E2, 0x1E204142};
        const u32 second[]{0x1E204100, 0x910023E2, 0x1E204121, 0x1E204142, 0xAA1303E0};
        const u32 third[]{0xAA1503E0, 0x1E222821, 0x2D418D02};
        std::copy(std::begin(first), std::end(first), &words[FarA]);
        std::copy(std::begin(second), std::end(second), &words[FarB]);
        std::copy(std::begin(third), std::end(third), &words[FarC]);
        words[DebrisSite] = 0x528001E8;      // mov w8, #0xf
        const u32 cascade[]{0xB95A4B68, 0x6B09011F, 0x1A89B108};
        std::copy(std::begin(cascade), std::end(cascade), &words[CascadeSite]);
        for (size_t i = 0; i < 3; ++i) {
            words[ShadowFarSites + i * 4] = ShadowFarLoads[i][0];
            words[ShadowFarSites + i * 4 + 1] = ShadowFarLoads[i][1];
        }
        words[DebrisSite + 14] = 0x91030288; // add x8, x20, #0xc0
        const u32 border[]{0x3941F3E8, 0xF94017E6, 0x7100011F, 0x1A9F17E8};
        std::copy(std::begin(border), std::end(border), &words[BorderSite]);
        words[BloomFirst] = LdrX(8, 8, BloomScaleParam);
        words[BloomFirst + 1] = LdrS(1, 8, ValueOffset);
        for (const size_t at : {BloomSecond, BloomThird}) {
            words[at] = LdrX(11, 26, BloomScaleParam);
            words[at + 1] = LdrS(0, 11, ValueOffset);
        }
    }

    void PutGetter(size_t at, u32 offset, bool is_float) {
        words[at] = LdrX(8, 0, offset);
        words[at + 1] = is_float ? LdrS(0, 8, ValueOffset) : LdrW(0, 8, ValueOffset);
        words[at + 2] = Ret;
    }

    std::span<u8> Bytes() {
        return {reinterpret_cast<u8*>(words.data()), words.size() * 4};
    }
};

Values Only(Id id, int value) {
    Values values{};
    values[id] = value;
    return values;
}
} // Anonymous namespace

TEST_CASE("Nx2DkQuality: nothing set, nothing touched", "[core]") {
    Text text;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Values{});
    for (const State state : result) {
        REQUIRE(state == State::Unset);
    }
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: the resolution range", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    values[ResBest] = 50;  // rate 2.0
    values[ResWorst] = 25; // rate 4.0
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    REQUIRE(result[ResBest] == State::Applied);
    REQUIRE(result[ResWorst] == State::Applied);
    expected[MinNear] = 0x52A80008;     // movz w8, #0x4000, lsl #16 (2.0)
    expected[MinNear + 1] = 0x1E270102; // fmov s2, w8
    expected[MinFar] = 0x52A8000A;      // movz w10, #0x4000, lsl #16
    expected[MinFar + 9] = 0x1E270142;  // fmov s2, w10
    expected[MaxNear] = 0x52A81008;     // movz w8, #0x4080, lsl #16 (4.0)
    expected[MaxNear + 1] = 0x1E270103; // fmov s3, w8
    expected[MaxFar] = 0x52A8100A;      // movz w10, #0x4080, lsl #16
    expected[MaxFar + 11] = 0x1E270145; // fmov s5, w10
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: a best resolution below the game's worst moves the worst too", "[core]") {
    Text text;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ResBest, 40));
    REQUIRE(result[ResBest] == State::Applied);
    REQUIRE(result[ResWorst] == State::Unset);
    REQUIRE(text.words[MinNear] == 0x52A80408); // 2.5
    REQUIRE(text.words[MaxNear] == 0x52A80408); // 2.5 as well: the game's 2.0 would be below
}

TEST_CASE("Nx2DkQuality: the game's own worst resolution is not moved by a best of 50", "[core]") {
    Text text;
    Apply(text.Bytes(), TextSize, AlignedSize, Only(ResBest, 50));
    REQUIRE(text.words[MaxNear] == LdrX(8, 10, MaxRateParam));
}

TEST_CASE("Nx2DkQuality: a best resolution within the game's range leaves the worst", "[core]") {
    Text text;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ResBest, 60));
    REQUIRE(result[ResBest] == State::Applied);
    REQUIRE(text.words[MaxNear] == LdrX(8, 10, MaxRateParam));
    REQUIRE(text.words[MaxFar] == LdrX(10, 10, MaxRateParam));
}

TEST_CASE("Nx2DkQuality: the getters", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    values[GpuTarget] = 75;
    values[FrameMinScale] = 50;
    values[RadialBlurScale] = 25;
    values[RadialBlurSamples] = 4;
    values[CubeMapInterval] = 30;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    for (const Id id : {GpuTarget, FrameMinScale, RadialBlurScale, RadialBlurSamples,
                        CubeMapInterval}) {
        REQUIRE(result[id] == State::Applied);
    }
    expected[GpuGetter] = 0x52A852C8;         // movz w8, #0x4296, lsl #16 (75.0)
    expected[GpuGetter + 1] = 0x1E270100;     // fmov s0, w8
    expected[FrameGetter] = 0x52A7E008;       // movz w8, #0x3f00, lsl #16 (0.5)
    expected[FrameGetter + 1] = 0x1E270100;
    expected[BlurScaleGetter] = 0x52A7D008;   // movz w8, #0x3e80, lsl #16 (0.25)
    expected[BlurScaleGetter + 1] = 0x1E270100;
    expected[BlurSamplesGetter] = 0x52800080; // movz w0, #4
    expected[BlurSamplesGetter + 1] = Ret;
    expected[CubeMapGetter] = 0x528003C0;     // movz w0, #30
    expected[CubeMapGetter + 1] = Ret;
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: reflection filtering off", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(GgxFilterOff, 1));
    REQUIRE(result[GgxFilterOff] == State::Applied);
    expected[GgxGetter] = 0x52800000; // movz w0, #0
    expected[GgxGetter + 1] = Ret;
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the decal limit", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ColorDecalMax, 32));
    REQUIRE(result[ColorDecalMax] == State::Applied);
    expected[DecalGetter] = 0x52800400; // movz w0, #32
    expected[DecalGetter + 1] = Ret;
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the actors' far clip distances", "[core]") {
    // The table is in the read-only data after the text segment
    Text text;
    const std::array<float, 10> table{-1.0f, 200.0f, 50.0f,  100.0f, 150.0f,
                                      300.0f, 450.0f, 700.0f, 900.0f, 1200.0f};
    std::vector<u32> image = text.words;
    image.resize(image.size() + 0x40, 0);
    const size_t at = text.words.size() + 0x10;
    std::memcpy(&image[at], table.data(), sizeof(table));
    std::span<u8> bytes{reinterpret_cast<u8*>(image.data()), image.size() * 4};
    const Result result = Apply(bytes, TextSize, AlignedSize, Only(ActorDrawDistance, 50));
    REQUIRE(result[ActorDrawDistance] == State::Applied);
    std::array<float, 10> now{};
    std::memcpy(now.data(), &image[at], sizeof(now));
    REQUIRE(now[0] == -1.0f); // never clipped stays so
    REQUIRE(now[1] == 100.0f);
    REQUIRE(now[2] == 25.0f);
    REQUIRE(now[9] == 600.0f);
    // The never clipped ones get a distance of their own only when asked
    std::vector<u32> image2 = text.words;
    image2.resize(image2.size() + 0x40, 0);
    std::memcpy(&image2[at], table.data(), sizeof(table));
    std::span<u8> bytes2{reinterpret_cast<u8*>(image2.data()), image2.size() * 4};
    const Result both = Apply(bytes2, TextSize, AlignedSize, Only(NonClipDistance, 80));
    REQUIRE(both[NonClipDistance] == State::Applied);
    REQUIRE(both[ActorDrawDistance] == State::Unset);
    std::memcpy(now.data(), &image2[at], sizeof(now));
    REQUIRE(now[0] == 800.0f);
    REQUIRE(now[1] == 200.0f);
    // Without the table (or with two) it is not applied
    Text plain;
    REQUIRE(Apply(plain.Bytes(), TextSize, AlignedSize, Only(ActorDrawDistance, 50))[ActorDrawDistance] ==
            State::Missed);
}

TEST_CASE("Nx2DkQuality: the sun shadow cascades", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ShadowCascades, 2));
    REQUIRE(result[ShadowCascades] == State::Applied);
    const size_t routine = Cave + CascadeRoutine;
    expected[CascadeSite] = 0x14000000U | static_cast<u32>(routine - CascadeSite);
    expected[routine] = 0xB95A4B68;     // ldr w8, [x27, #0x1a48]
    expected[routine + 1] = 0x7100091F; // cmp w8, #2
    expected[routine + 2] = 0x5400004D; // b.le +8
    expected[routine + 3] = 0x52800048; // movz w8, #2
    expected[routine + 4] =
        0x14000000U | (static_cast<u32>((CascadeSite + 1) - (routine + 4)) & 0x3FFFFFFU);
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the sun shadow distance, three places", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(ShadowDistance, 50));
    REQUIRE(result[ShadowDistance] == State::Applied);
    for (size_t i = 0; i < 3; ++i) {
        const size_t routine = Cave + ShadowFarRoutine + i * ShadowFarRoutineWords;
        const size_t from = ShadowFarSites + i * 4;
        expected[from] = 0x14000000U | static_cast<u32>(routine - from);
        expected[routine] = ShadowFarLoads[i][0];
        expected[routine + 1] = 0x1C00007F; // ldr s31, #12
        expected[routine + 2] = 0x1E3F0800; // fmul s0, s0, s31
        expected[routine + 3] =
            0x14000000U | (static_cast<u32>((from + 1) - (routine + 3)) & 0x3FFFFFFU);
        expected[routine + 4] = 0x3F000000; // 0.5
    }
    REQUIRE(text.words == expected);
    // One place missing: none is changed
    Text partial;
    partial.words[ShadowFarSites + 8] = Filler;
    const std::vector<u32> before = partial.words;
    REQUIRE(Apply(partial.Bytes(), TextSize, AlignedSize, Only(ShadowDistance, 50))[ShadowDistance] ==
            State::Missed);
    REQUIRE(partial.words == before);
}

TEST_CASE("Nx2DkQuality: debris off", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(DebrisOff, 1));
    REQUIRE(result[DebrisOff] == State::Applied);
    const size_t routine = Cave + DebrisRoutine;
    const size_t from = DebrisSite + 14;
    expected[from] = 0x14000000U | static_cast<u32>(routine - from);
    expected[routine] = 0x52800049;     // movz w9, #2
    expected[routine + 1] = 0xB900A289; // str w9, [x20, #0xa0]
    expected[routine + 2] = 0x91030288; // add x8, x20, #0xc0
    expected[routine + 3] = 0x14000000U | (static_cast<u32>((from + 1) - (routine + 3)) & 0x3FFFFFFU);
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the cave routines together", "[core]") {
    Text text;
    Values values{};
    values[CameraFar] = 50;
    values[DebrisOff] = 1;
    values[ShadowCascades] = 1;
    values[ShadowDistance] = 30;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    REQUIRE(result[CameraFar] == State::Applied);
    REQUIRE(result[DebrisOff] == State::Applied);
    REQUIRE(result[ShadowCascades] == State::Applied);
    REQUIRE(result[ShadowDistance] == State::Applied);
}

TEST_CASE("Nx2DkQuality: the camera far, three places", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(CameraFar, 10));
    REQUIRE(result[CameraFar] == State::Applied);
    const std::array<size_t, 3> sites{FarA + 1, FarB + 2, FarC + 1};
    const std::array<u32, 3> originals{0x1E204121, 0x1E204121, 0x1E222821};
    for (size_t i = 0; i < 3; ++i) {
        const size_t routine = Cave + FarRoutine + i * FarRoutineWords;
        expected[sites[i]] = 0x14000000U | static_cast<u32>(routine - sites[i]);
        expected[routine] = originals[i];
        expected[routine + 1] = 0x1C000062; // ldr s2, #12
        expected[routine + 2] = 0x1E220821; // fmul s1, s1, s2
        expected[routine + 3] =
            0x14000000U | (static_cast<u32>((sites[i] + 1) - (routine + 3)) & 0x3FFFFFFU);
        expected[routine + 4] = 0x3DCCCCCD; // 0.1f
    }
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the camera far needs all three places", "[core]") {
    Text text;
    text.words[FarC + 1] = Filler;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(CameraFar, 10));
    REQUIRE(result[CameraFar] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: the bloom scale, all three places", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(BloomScale, 25));
    REQUIRE(result[BloomScale] == State::Applied);
    expected[BloomFirst] = 0x52A7D008;     // movz w8, #0x3e80, lsl #16
    expected[BloomFirst + 1] = 0x1E270101; // fmov s1, w8
    for (const size_t at : {BloomSecond, BloomThird}) {
        expected[at] = 0x52A7D00B;     // movz w11, #0x3e80, lsl #16
        expected[at + 1] = 0x1E270160; // fmov s0, w11
    }
    REQUIRE(text.words == expected);
}

TEST_CASE("Nx2DkQuality: the bloom scale needs all three places", "[core]") {
    Text text;
    text.words[BloomThird] = Filler;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(BloomScale, 25));
    REQUIRE(result[BloomScale] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: the camera far needs its free space", "[core]") {
    Text text;
    text.words[Cave + 7] = Filler;
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(CameraFar, 50));
    REQUIRE(result[CameraFar] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: a setting found twice is left alone", "[core]") {
    Text text;
    text.PutGetter(0x500, RadialBlurScaleParam, true);
    const std::vector<u32> before = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, Only(RadialBlurScale, 25));
    REQUIRE(result[RadialBlurScale] == State::Missed);
    REQUIRE(text.words == before);
}

TEST_CASE("Nx2DkQuality: values and variables", "[core]") {
    REQUIRE(Parse(nullptr, Knobs[ResBest]) == 0);
    REQUIRE(Parse("", Knobs[ResBest]) == 0);
    REQUIRE(Parse("60", Knobs[ResBest]) == 60);
    REQUIRE(Parse("24", Knobs[ResBest]) == -1);
    REQUIRE(Parse("101", Knobs[ResBest]) == -1);
    REQUIRE(Parse("60%", Knobs[ResBest]) == -1);
    REQUIRE(Parse("240", Knobs[CubeMapInterval]) == 240);
    REQUIRE(Sanitize(9, Knobs[CameraFar]) == 0);

    Values values{};
    values[ResBest] = 60;
    values[CameraFar] = 40;
    values[CubeMapInterval] = 999; // out of range: left out
    REQUIRE(Wanted(values, false).empty());
    const auto vars = Wanted(values, true);
    REQUIRE(vars.size() == 2);
    REQUIRE(vars[0].name == "EDEN_NX2_DKQ_RES_BEST");
    REQUIRE(vars[0].value == "60");
    REQUIRE(vars[1].name == "EDEN_NX2_DKQ_CAMERA_FAR");
    REQUIRE(vars[1].value == "40");
}

TEST_CASE("Nx2DkQuality: the presets", "[core]") {
    REQUIRE(PresetOf(Values{}) == Preset::Default);
    for (const Preset preset : {Preset::Balanced, Preset::Performance}) {
        const Values values = PresetValues(preset);
        REQUIRE(PresetOf(values) == preset);
        REQUIRE(values != Values{});
        for (size_t i = 0; i < Count; ++i) {
            REQUIRE(values[i] == Sanitize(values[i], Knobs[i]));
        }
        // The best resolution is not below the worst one
        REQUIRE(values[ResBest] >= values[ResWorst]);
    }
    Values changed = PresetValues(Preset::Balanced);
    changed[BloomScale] = 11;
    REQUIRE(PresetOf(changed) == Preset::Custom);
    REQUIRE(PresetOf(Only(ResBest, 60)) == Preset::Custom);
}

TEST_CASE("Nx2DkQuality: the border mesh and the shadow masks", "[core]") {
    Text text;
    Values values{};
    values[VoxelBorderMeshOff] = 1;
    values[ShadowMaskOff] = 1;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    REQUIRE(result[VoxelBorderMeshOff] == State::Applied);
    REQUIRE(result[ShadowMaskOff] == State::Applied);
    REQUIRE(text.words[ShadowMaskGetter] == 0x52800000); // movz w0, #0
    REQUIRE(text.words[ShadowMaskGetter + 1] == Ret);
    REQUIRE(text.words[BorderSite] == 0x52800028); // mov w8, #1
}

TEST_CASE("Nx2DkQuality: the features switched off in place", "[core]") {
    Text text;
    std::vector<u32> expected = text.words;
    Values values{};
    for (const auto& flag : FeatureFlags) {
        values[flag.first] = 1;
    }
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize, values);
    for (size_t i = 0; i < FeatureFlags.size(); ++i) {
        REQUIRE(result[FeatureFlags[i].first] == State::Applied);
        expected[FlagSite + i * 4 + 1] = 0x5280000B; // movz w11, #0
    }
    REQUIRE(text.words == expected);
}
