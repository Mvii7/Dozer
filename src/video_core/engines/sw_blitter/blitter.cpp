// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "common/div_ceil.h"
#include "common/logging.h"
#include "common/scratch_buffer.h"
#include "video_core/engines/sw_blitter/blitter.h"
#include "video_core/engines/sw_blitter/converter.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/surface.h"
#include "video_core/textures/decoders.h"

namespace Tegra {
class MemoryManager;
}

using VideoCore::Surface::BytesPerBlock;
using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

namespace Tegra::Engines::Blitter {

using namespace Texture;

namespace {

constexpr size_t ir_components = 4;

void NearestNeighbor(std::span<const u8> input, std::span<u8> output, u32 src_width, u32 src_height,
                     u32 dst_width, u32 dst_height, size_t bpp) {
    const size_t dx_du = std::llround((static_cast<f64>(src_width) / dst_width) * (1ULL << 32));
    const size_t dy_dv = std::llround((static_cast<f64>(src_height) / dst_height) * (1ULL << 32));
    size_t src_y = 0;
    for (u32 y = 0; y < dst_height; y++) {
        size_t src_x = 0;
        for (u32 x = 0; x < dst_width; x++) {
            const size_t read_from = ((src_y * src_width + src_x) >> 32) * bpp;
            const size_t write_to = (y * dst_width + x) * bpp;

            std::memcpy(&output[write_to], &input[read_from], bpp);
            src_x += dx_du;
        }
        src_y += dy_dv;
    }
}

void NearestNeighborFast(std::span<const f32> input, std::span<f32> output, u32 src_width,
                         u32 src_height, u32 dst_width, u32 dst_height) {
    const size_t dx_du = std::llround((static_cast<f64>(src_width) / dst_width) * (1ULL << 32));
    const size_t dy_dv = std::llround((static_cast<f64>(src_height) / dst_height) * (1ULL << 32));
    size_t src_y = 0;
    for (u32 y = 0; y < dst_height; y++) {
        size_t src_x = 0;
        for (u32 x = 0; x < dst_width; x++) {
            const size_t read_from = ((src_y * src_width + src_x) >> 32) * ir_components;
            const size_t write_to = (y * dst_width + x) * ir_components;

            std::memcpy(&output[write_to], &input[read_from], sizeof(f32) * ir_components);
            src_x += dx_du;
        }
        src_y += dy_dv;
    }
}

void Bilinear(std::span<const f32> input, std::span<f32> output, size_t src_width,
              size_t src_height, size_t dst_width, size_t dst_height) {
    const auto bilinear_sample = [](std::span<const f32> x0_y0, std::span<const f32> x1_y0,
                                    std::span<const f32> x0_y1, std::span<const f32> x1_y1,
                                    f32 weight_x, f32 weight_y) {
        std::array<f32, ir_components> result{};
        for (size_t i = 0; i < ir_components; i++) {
            const f32 a = std::lerp(x0_y0[i], x1_y0[i], weight_x);
            const f32 b = std::lerp(x0_y1[i], x1_y1[i], weight_x);
            result[i] = std::lerp(a, b, weight_y);
        }
        return result;
    };
    const f32 dx_du =
        dst_width > 1 ? static_cast<f32>(src_width - 1) / static_cast<f32>(dst_width - 1) : 0.f;
    const f32 dy_dv =
        dst_height > 1 ? static_cast<f32>(src_height - 1) / static_cast<f32>(dst_height - 1) : 0.f;
    for (u32 y = 0; y < dst_height; y++) {
        for (u32 x = 0; x < dst_width; x++) {
            const f32 x_low = std::floor(static_cast<f32>(x) * dx_du);
            const f32 y_low = std::floor(static_cast<f32>(y) * dy_dv);
            const f32 x_high = std::ceil(static_cast<f32>(x) * dx_du);
            const f32 y_high = std::ceil(static_cast<f32>(y) * dy_dv);
            const f32 weight_x = (static_cast<f32>(x) * dx_du) - x_low;
            const f32 weight_y = (static_cast<f32>(y) * dy_dv) - y_low;

            const auto read_src = [&](f32 in_x, f32 in_y) {
                const size_t read_from =
                    ((static_cast<size_t>(in_x) * src_width + static_cast<size_t>(in_y)) >> 32) *
                    ir_components;
                return std::span<const f32>(&input[read_from], ir_components);
            };

            auto x0_y0 = read_src(x_low, y_low);
            auto x1_y0 = read_src(x_high, y_low);
            auto x0_y1 = read_src(x_low, y_high);
            auto x1_y1 = read_src(x_high, y_high);

            const auto result = bilinear_sample(x0_y0, x1_y0, x0_y1, x1_y1, weight_x, weight_y);

            const size_t write_to = (y * dst_width + x) * ir_components;

            std::memcpy(&output[write_to], &result, sizeof(f32) * ir_components);
        }
    }
}

template <bool unpack>
void ProcessPitchLinear(std::span<const u8> input, std::span<u8> output, size_t extent_x,
                        size_t extent_y, u32 pitch, u32 x0, u32 y0, size_t bpp) {
    const size_t base_offset = x0 * bpp;
    const size_t copy_size = extent_x * bpp;
    for (size_t y = 0; y < extent_y; y++) {
        const size_t first_offset = (y + y0) * pitch + base_offset;
        const size_t second_offset = y * extent_x * bpp;
        u8* write_to = unpack ? &output[first_offset] : &output[second_offset];
        const u8* read_from = unpack ? &input[second_offset] : &input[first_offset];
        std::memcpy(write_to, read_from, copy_size);
    }
}

// The part of a surface a blit touches: rows [y0, y1) rounded out to whole blocks, so the
// swizzle offsets inside the window match those of the full surface.
struct SurfaceWindow {
    GPUVAddr address;
    size_t size;
    u32 height;
    u32 y0;
};

SurfaceWindow GetSurfaceWindow(const Fermi2D::Surface& surface, u32 bytes_per_pixel, s32 y0,
                               s32 y1) {
    const bool block_linear = surface.linear == Fermi2D::MemoryLayout::BlockLinear;
    const size_t full_size =
        block_linear ? CalculateSize(true, bytes_per_pixel, surface.width, surface.height,
                                     surface.depth, surface.block_height, surface.block_depth)
                     : static_cast<size_t>(surface.pitch) * surface.height;
    const SurfaceWindow full{surface.Address(), full_size, surface.height, static_cast<u32>(y0)};
    if (y0 < 0 || y0 >= y1 || static_cast<u32>(y0) >= surface.height ||
        (block_linear && surface.depth != 1)) {
        return full;
    }
    u32 rows_per_block = 1;
    size_t block_row_size = surface.pitch;
    if (block_linear) {
        const u32 gobs_in_x = Common::DivCeil(surface.width * bytes_per_pixel, GOB_SIZE_X);
        rows_per_block = GOB_SIZE_Y << surface.block_height;
        block_row_size = size_t{gobs_in_x}
                         << (GOB_SIZE_SHIFT + surface.block_height + surface.block_depth);
    }
    const u32 y_end = (std::min)(static_cast<u32>(y1), surface.height);
    const u32 first = static_cast<u32>(y0) / rows_per_block;
    const u32 last = Common::DivCeil(y_end, rows_per_block);
    const size_t offset = first * block_row_size;
    if (offset >= full_size) {
        return full;
    }
    return SurfaceWindow{
        .address = surface.Address() + offset,
        .size = (std::min)(full_size - offset, (last - first) * block_row_size),
        .height = (std::min)(last * rows_per_block, surface.height) - first * rows_per_block,
        .y0 = static_cast<u32>(y0) - first * rows_per_block,
    };
}

// True when a blit copies a whole surface into one of the same format, size and layout, so the
// guest bytes can be copied as they are.
bool IsRawSurfaceCopy(const Fermi2D::Surface& src, const Fermi2D::Surface& dst,
                      const Fermi2D::Config& config) {
    if (src.format != dst.format || src.linear != dst.linear || src.width != dst.width ||
        src.height != dst.height || config.src_x0 != 0 || config.src_y0 != 0 ||
        config.dst_x0 != 0 || config.dst_y0 != 0 ||
        config.src_x1 != static_cast<s32>(src.width) ||
        config.src_y1 != static_cast<s32>(src.height) ||
        config.dst_x1 != static_cast<s32>(dst.width) ||
        config.dst_y1 != static_cast<s32>(dst.height)) {
        return false;
    }
    if (src.linear == Fermi2D::MemoryLayout::BlockLinear) {
        return src.depth == 1 && dst.depth == 1 && src.block_height == dst.block_height &&
               src.block_depth == dst.block_depth;
    }
    return src.pitch == dst.pitch;
}

// EDEN_NX2_RT_TRACE: log the first CPU-fallback blits that take 2 ms or more.
void TraceSlowBlit(const Fermi2D::Surface& src, const Fermi2D::Surface& dst,
                   const Fermi2D::Config& config, std::chrono::steady_clock::time_point start,
                   const char* path) {
    static const bool trace = std::getenv("EDEN_NX2_RT_TRACE") != nullptr;
    if (!trace) {
        return;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start);
    static u32 traced{};
    if (elapsed.count() < 2.0 || traced >= 64) {
        return;
    }
    ++traced;
    const auto describe = [](const Fermi2D::Surface& surface) {
        return fmt::format("{:#x} {}x{} fmt {:#x} {} bh {} pitch {}", surface.Address(),
                           surface.width, surface.height, static_cast<u32>(surface.format),
                           surface.linear == Fermi2D::MemoryLayout::BlockLinear ? "block"
                                                                               : "pitch",
                           surface.block_height.Value(), surface.pitch);
    };
    LOG_INFO(HW_GPU, "nx2 sw blit {:.1f} ms ({}): {} rect {},{}-{},{} -> {} rect {},{}-{},{}",
             elapsed.count(), path, describe(src), config.src_x0, config.src_y0, config.src_x1,
             config.src_y1, describe(dst), config.dst_x0, config.dst_y0, config.dst_x1,
             config.dst_y1);
}

} // namespace

struct SoftwareBlitEngine::BlitEngineImpl {
    Common::ScratchBuffer<u8> tmp_buffer;
    Common::ScratchBuffer<u8> src_buffer;
    Common::ScratchBuffer<u8> dst_buffer;
    Common::ScratchBuffer<f32> intermediate_src;
    Common::ScratchBuffer<f32> intermediate_dst;
    ConverterFactory converter_factory;
};

SoftwareBlitEngine::SoftwareBlitEngine(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_} {
    impl = std::make_unique<BlitEngineImpl>();
}

SoftwareBlitEngine::~SoftwareBlitEngine() = default;

bool SoftwareBlitEngine::Blit(Fermi2D::Surface& src, Fermi2D::Surface& dst,
                              Fermi2D::Config& config) {
    const auto start = std::chrono::steady_clock::now();
    const u32 src_extent_x = config.src_x1 - config.src_x0;
    const u32 src_extent_y = config.src_y1 - config.src_y0;

    const u32 dst_extent_x = config.dst_x1 - config.dst_x0;
    const u32 dst_extent_y = config.dst_y1 - config.dst_y0;
    const auto src_bytes_per_pixel = BytesPerBlock(PixelFormatFromRenderTargetFormat(src.format));
    const auto dst_bytes_per_pixel = BytesPerBlock(PixelFormatFromRenderTargetFormat(dst.format));
    const SurfaceWindow src_window =
        GetSurfaceWindow(src, src_bytes_per_pixel, config.src_y0, config.src_y1);

    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::SafeRead> tmp_buffer(
        memory_manager, src_window.address, src_window.size, &impl->tmp_buffer);

    if (IsRawSurfaceCopy(src, dst, config)) {
        if (src.Address() != dst.Address()) {
            memory_manager.WriteBlock(dst.Address(), tmp_buffer.data(), src_window.size);
        }
        TraceSlowBlit(src, dst, config, start, "raw");
        return true;
    }

    const size_t src_copy_size = src_extent_x * src_extent_y * src_bytes_per_pixel;
    const size_t dst_copy_size = dst_extent_x * dst_extent_y * dst_bytes_per_pixel;

    impl->src_buffer.resize_destructive(src_copy_size);

    const bool no_passthrough =
        src.format != dst.format || src_extent_x != dst_extent_x || src_extent_y != dst_extent_y;

    const auto conversion_phase_same_format = [&]() {
        NearestNeighbor(impl->src_buffer, impl->dst_buffer, src_extent_x, src_extent_y,
                        dst_extent_x, dst_extent_y, dst_bytes_per_pixel);
    };

    const auto conversion_phase_ir = [&]() {
        auto* input_converter = impl->converter_factory.GetFormatConverter(src.format);
        impl->intermediate_src.resize_destructive((src_copy_size / src_bytes_per_pixel) *
                                                  ir_components);
        impl->intermediate_dst.resize_destructive((dst_copy_size / dst_bytes_per_pixel) *
                                                  ir_components);
        input_converter->ConvertTo(impl->src_buffer, impl->intermediate_src);

        if (config.filter != Fermi2D::Filter::Bilinear) {
            NearestNeighborFast(impl->intermediate_src, impl->intermediate_dst, src_extent_x,
                                src_extent_y, dst_extent_x, dst_extent_y);
        } else {
            Bilinear(impl->intermediate_src, impl->intermediate_dst, src_extent_x, src_extent_y,
                     dst_extent_x, dst_extent_y);
        }

        auto* output_converter = impl->converter_factory.GetFormatConverter(dst.format);
        output_converter->ConvertFrom(impl->intermediate_dst, impl->dst_buffer);
    };

    // Switch 2 titles use a different GOB order; EDEN_NX2_BLIT_NATIVE_TILING=0 restores the old one.
    const auto UseNativeTiling = [this] {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_BLIT_NATIVE_TILING");
            return value == nullptr || value[0] != '0';
        }();
        return enabled && memory_manager.UsesNativeTiling();
    };

    // Do actual Blit

    impl->dst_buffer.resize_destructive(dst_copy_size);
    if (src.linear == Fermi2D::MemoryLayout::BlockLinear) {
        UnswizzleSubrect(impl->src_buffer, tmp_buffer, src_bytes_per_pixel, src.width,
                         src_window.height, src.depth, config.src_x0, src_window.y0,
                         src_extent_x, src_extent_y,
                         src.block_height, src.block_depth, src_extent_x * src_bytes_per_pixel,
                         UseNativeTiling());
    } else {
        ProcessPitchLinear<false>(tmp_buffer, impl->src_buffer, src_extent_x, src_extent_y,
                                  src.pitch, config.src_x0, src_window.y0, src_bytes_per_pixel);
    }

    // Conversion Phase
    if (no_passthrough) {
        if (src.format != dst.format || config.filter == Fermi2D::Filter::Bilinear) {
            conversion_phase_ir();
        } else {
            conversion_phase_same_format();
        }
    } else {
        impl->dst_buffer.swap(impl->src_buffer);
    }

    const SurfaceWindow dst_window =
        GetSurfaceWindow(dst, dst_bytes_per_pixel, config.dst_y0, config.dst_y1);
    const auto write_destination = [&](auto& tmp_buffer2) {
        if (dst.linear == Fermi2D::MemoryLayout::BlockLinear) {
            SwizzleSubrect(tmp_buffer2, impl->dst_buffer, dst_bytes_per_pixel, dst.width,
                           dst_window.height, dst.depth, config.dst_x0, dst_window.y0,
                           dst_extent_x, dst_extent_y, dst.block_height, dst.block_depth,
                           dst_extent_x * dst_bytes_per_pixel, UseNativeTiling());
        } else {
            ProcessPitchLinear<true>(impl->dst_buffer, tmp_buffer2, dst_extent_x, dst_extent_y,
                                     dst.pitch, config.dst_x0, dst_window.y0,
                                     static_cast<size_t>(dst_bytes_per_pixel));
        }
    };
    // A blit that writes every byte of the window (no padding in it) needs no read of the old
    // destination.
    const bool covers_window = config.dst_x0 == 0 && dst_extent_x == dst.width &&
                               dst_window.y0 == 0 && dst_extent_y == dst_window.height &&
                               dst_window.size == dst_copy_size;
    if (covers_window) {
        Tegra::Memory::GpuGuestMemoryScoped<u8, Tegra::Memory::GuestMemoryFlags::SafeWrite>
            tmp_buffer2(memory_manager, dst_window.address, dst_window.size, &impl->tmp_buffer);
        write_destination(tmp_buffer2);
    } else {
        Tegra::Memory::GpuGuestMemoryScoped<u8, Tegra::Memory::GuestMemoryFlags::SafeReadWrite>
            tmp_buffer2(memory_manager, dst_window.address, dst_window.size, &impl->tmp_buffer);
        write_destination(tmp_buffer2);
    }
    TraceSlowBlit(src, dst, config, start, covers_window ? "whole" : "part");
    return true;
}

} // namespace Tegra::Engines::Blitter
