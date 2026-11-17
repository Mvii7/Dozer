// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class Maxwell3D;
}

// EDEN_NX2_UBO_PREP (back stages, cut a): A ("GPUBack") reads each draw's uniform buffers from
// guest memory, compares them with what it uploaded before and copies new ones into an upload
// ring of its own, and the draw record carries the result; B ("GPUMem") binds those slices
// instead of reading, comparing and copying the bytes itself. 0 off (default), 1 on, 2 verify (B
// also reads guest memory and logs any slice that differs, then binds its own).
// - A runs ahead of B. Guest memory there lacks the constant buffer data A sent B and B has not
//   written yet: A keeps those words (a shadow, by 4 KiB page) and lays the ones B has not applied
//   over what it read (B counts the words it applied). Any other write B or another thread makes
//   (inline data, DMA, copies back, query results, CPU writes the caches are told of) is noted in
//   a log of foreign writes; B binds A's slice only when none since A read reached the range.
// - Which slots a draw binds and their sizes come from B's pipeline: B publishes them by the
//   draw's shader programs (ProgramKey), A prepares a stage once it knows them.
// - B decides the fast path as before (GPU-written or larger uniform buffers keep B's way) and
//   checks A's device address and size against its own binding.
// - The ring is reused once the scheduler ticks of the draws that used it are done: B notes, for
//   each tick, A's ring position after the last draw it recorded in it.
namespace VideoCommon::Nx2UniformPrep {

[[nodiscard]] inline int Mode() {
    static const int mode = [] {
        const char* const value = std::getenv("EDEN_NX2_UBO_PREP");
        if (value == nullptr || value[0] == '\0' || value[0] == '0') {
            return 0;
        }
        return value[0] == '2' ? 2 : 1;
    }();
    return mode;
}

inline constexpr u32 NumStages = 5;
inline constexpr u32 NumSlots = 18;
inline constexpr u32 MaxSize = 4096;

/// Any thread: a write the shadow does not hold reached [addr, addr + size) of device memory.
void NoteForeign(u64 addr, u64 size);
/// Any thread: a write whose device range is unknown (every prepared slice is refused after it).
void NoteForeignAll();

/// A: ProcessCBMultiData's write at `address` of `primary`'s address space, sent to B.
void NotePostedCb(Tegra::Engines::Maxwell3D& primary, u64 address, const u32* data, u32 count);
/// B: a CbData record's `count` words written (or dropped) by B.
void NoteAppliedCb(u32 count);

using CurrentTickFn = u64 (*)(void* context);
using IsFreeFn = bool (*)(void* context, u64 tick);
/// The Vulkan buffer runtime, before the stages start: the upload ring and B's scheduler.
void SetRing(std::span<u8> mapped, CurrentTickFn current_tick, IsFreeFn is_free, void* context);

/// A, a draw-like record: the words to send with it (none when nothing is prepared).
void Prepare(Core::System& system, Tegra::Engines::Maxwell3D& primary, std::vector<u64>& out);

/// B: a prepared slice of the current draw.
struct Entry {
    u64 device_addr;
    u32 offset;
    u32 size;
};

/// B, around a draw record carrying Prepare's words.
void BeginDraw(const Tegra::Engines::Maxwell3D& replica, const u64* words, u32 count);
void EndDraw();

/// B: the slice A prepared for the current draw's `stage` and `slot`, or null.
[[nodiscard]] const Entry* Find(size_t stage, u32 slot);
/// B: whether a foreign write since A read the current draw's slices may have reached the range.
[[nodiscard]] bool WrittenSinceRead(u64 addr, u64 size);
/// B: the slots and sizes the pipeline binds for `stage` (from BindHostGraphicsUniformBuffers),
/// published for A when A did not know them for this draw.
void PublishUsage(size_t stage, u32 mask, const u32* sizes);
/// B, =2: the bytes of a prepared slice.
[[nodiscard]] std::span<const u8> RingBytes(u32 offset, u32 size);
/// B: a bind took A's slice, took its own way with one prepared, or (=2) found it different.
void CountBind(bool used, bool mismatch);

} // namespace VideoCommon::Nx2UniformPrep
