// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "video_core/textures/texture.h"

namespace Tegra::Texture {

constexpr u32 GOB_SIZE_X = 64;
constexpr u32 GOB_SIZE_Y = 8;
constexpr u32 GOB_SIZE_Z = 1;
constexpr u32 GOB_SIZE = GOB_SIZE_X * GOB_SIZE_Y * GOB_SIZE_Z;

constexpr u32 GOB_SIZE_X_SHIFT = 6;
constexpr u32 GOB_SIZE_Y_SHIFT = 3;
constexpr u32 GOB_SIZE_Z_SHIFT = 0;
constexpr u32 GOB_SIZE_SHIFT = GOB_SIZE_X_SHIFT + GOB_SIZE_Y_SHIFT + GOB_SIZE_Z_SHIFT;

constexpr u32 SWIZZLE_X_BITS = 0b100101111;
constexpr u32 SWIZZLE_Y_BITS = 0b011010000;

/// Unswizzles a block linear texture into linear memory.
void UnswizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment = 1, bool native_tiling = false);

/// Swizzles linear memory into a block linear texture.
void SwizzleTexture(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 block_height, u32 block_depth,
                    u32 stride_alignment = 1, bool native_tiling = false);

/// This function calculates the correct size of a texture depending if it's tiled or not.
std::size_t CalculateSize(bool tiled, u32 bytes_per_pixel, u32 width, u32 height, u32 depth,
                          u32 block_height, u32 block_depth);

/// Copies an untiled subrectangle into a tiled surface.
void SwizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel, u32 width,
                    u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x, u32 extent_y,
                    u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling = false);

/// Copies a tiled subrectangle into a linear surface.
void UnswizzleSubrect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 origin_x, u32 origin_y, u32 extent_x,
                      u32 extent_y, u32 block_height, u32 block_depth, u32 pitch_linear, bool native_tiling = false);

/// Copies a rectangle between two tiled surfaces of the same power-of-two bytes per pixel (at
/// most 16), depth 1 and block depth 0, whose origins are on GOB boundaries. GOBs the rectangle
/// covers whole are copied as they are, so the surfaces may have different block heights.
void CopyTiledRect(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                   u32 dst_width, u32 dst_block_height, u32 dst_x, u32 dst_y, u32 src_width,
                   u32 src_block_height, u32 src_x, u32 src_y, u32 extent_x, u32 extent_y,
                   bool native_tiling = false);

/// CopyTiledRect into a destination held in separate 4 KiB host pages: `pages[i]` points to the
/// destination's first byte in page i, and the destination starts `first_page_offset` bytes into
/// page 0. Its start must be GOB aligned, so no GOB straddles two pages.
void CopyTiledRectToPages(std::span<u8* const> pages, u32 first_page_offset,
                          std::size_t output_size, std::span<const u8> input, u32 bytes_per_pixel,
                          u32 dst_width, u32 dst_block_height, u32 dst_x, u32 dst_y,
                          u32 src_width, u32 src_block_height, u32 src_x, u32 src_y,
                          u32 extent_x, u32 extent_y, bool native_tiling = false);

/// Obtains the offset of the gob for positions 'dst_x' & 'dst_y'
u64 GetGOBOffset(u32 width, u32 height, u32 dst_x, u32 dst_y, u32 block_height,
                 u32 bytes_per_pixel);

} // namespace Tegra::Texture
