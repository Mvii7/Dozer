// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "common/logging.h"
#include "core/core.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_stages.h"
#include "video_core/nx2_uniform_prep.h"

namespace VideoCommon::Nx2UniformPrep {
namespace {

using Tegra::Engines::Maxwell3D;

// ---- Foreign writes (any thread writes, B reads) ----

/// A ring of the last writes. An entry's index is stored last: a reader seeing another index
/// (overwritten, or being written) counts the entry as reaching every range.
struct ForeignLog {
    static constexpr u64 SIZE = 4096;
    /// Entries read from this far before A's count: a write noted just before A read memory may
    /// land in it just after (a write around the caches is noted before it is made).
    static constexpr u64 SLACK = 16;
    struct Entry {
        std::atomic<u64> index{~u64{0}};
        std::atomic<u64> begin{};
        std::atomic<u64> end{};
    };
    std::array<Entry, SIZE> entries{};
    std::atomic<u64> count{};
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
};
ForeignLog g_foreign;

void NoteForeignRange(u64 begin, u64 end) {
    ForeignLog& log = g_foreign;
    while (log.lock.test_and_set(std::memory_order_acquire)) {
    }
    const u64 index = log.count.load(std::memory_order_relaxed);
    ForeignLog::Entry& entry = log.entries[index % ForeignLog::SIZE];
    entry.index.store(~u64{0}, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    entry.begin.store(begin, std::memory_order_relaxed);
    entry.end.store(end, std::memory_order_relaxed);
    entry.index.store(index, std::memory_order_release);
    log.count.store(index + 1, std::memory_order_release);
    log.lock.clear(std::memory_order_release);
}

bool ForeignSince(u64 since, u64 addr, u64 size) {
    ForeignLog& log = g_foreign;
    const u64 now = log.count.load(std::memory_order_acquire);
    since = since > ForeignLog::SLACK ? since - ForeignLog::SLACK : 0;
    if (now - since >= ForeignLog::SIZE - 64) {
        return true;
    }
    const u64 end = addr + size;
    for (u64 i = since; i < now; ++i) {
        const ForeignLog::Entry& entry = log.entries[i % ForeignLog::SIZE];
        if (entry.index.load(std::memory_order_acquire) != i) {
            return true;
        }
        const u64 begin = entry.begin.load(std::memory_order_relaxed);
        const u64 write_end = entry.end.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (entry.index.load(std::memory_order_relaxed) != i) {
            return true;
        }
        if (begin < end && addr < write_end) {
            return true;
        }
    }
    return false;
}

// ---- Constant buffer data: A posts, B applies (counted in words) ----

std::atomic<u64> g_applied{};

/// A's own: the words of constant buffer data it sent, by 4 KiB page of device memory, with the
/// count of words sent up to and including each (its sequence number).
class CbShadow {
public:
    void Write(Maxwell3D& primary, u64 address, const u32* data, u32 count) {
        Tegra::MemoryManager& memory_manager = primary.GetMemoryManager();
        const u64 first_seq = posted + 1;
        posted += count;
        if ((address & 3) != 0) {
            // Not word aligned (never seen): the range is left to B until applied.
            if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                poisons.push_back({*device, *device + u64{count} * 4, posted});
            } else {
                poison_all = posted;
            }
            return;
        }
        u32 done = 0;
        while (done < count) {
            const u64 word_address = address + u64{done} * 4;
            const u64 page_end = (word_address | 0xfff) + 1;
            const u32 words = static_cast<u32>(
                (std::min)(u64{count - done}, (page_end - word_address) / 4));
            const std::optional<u64> device = memory_manager.GpuToCpuAddress(word_address);
            if (!device) {
                // B's write goes nowhere either.
                done += words;
                continue;
            }
            for (u32 i = 0; i < words; ++i) {
                const u64 word_device = *device + u64{i} * 4;
                Block& block = BlockAt(word_device >> 12);
                const u32 word = static_cast<u32>((word_device & 0xfff) >> 2);
                block.data[word] = data[done + i];
                block.seq[word] = first_seq + done + i;
                block.max_seq = first_seq + done + i;
            }
            done += words;
        }
    }

    /// Lays the words B has not applied (sequence above `applied`) over `bytes`, read from device
    /// memory at `addr`. False when part of the range has a write the shadow cannot give.
    bool Overlay(u64 addr, u8* bytes, u32 size, u64 applied) {
        if (posted <= applied) {
            return true;
        }
        if (poison_all > applied) {
            return false;
        }
        const u64 end = addr + size;
        for (const Poison& poison : poisons) {
            if (poison.seq > applied && poison.begin < end && addr < poison.end) {
                return false;
            }
        }
        for (u64 page = addr >> 12; page <= (end - 1) >> 12; ++page) {
            const auto it = blocks.find(page);
            if (it == blocks.end() || it->second->max_seq <= applied) {
                continue;
            }
            const Block& block = *it->second;
            const u64 page_base = page << 12;
            const u64 begin = (std::max)(addr, page_base);
            const u64 stop = (std::min)(end, page_base + 0x1000);
            for (u64 word_addr = begin & ~u64{3}; word_addr < stop; word_addr += 4) {
                const u32 word = static_cast<u32>((word_addr - page_base) >> 2);
                if (block.seq[word] <= applied) {
                    continue;
                }
                u8 word_bytes[4];
                std::memcpy(word_bytes, &block.data[word], 4);
                for (u32 b = 0; b < 4; ++b) {
                    const u64 byte_addr = word_addr + b;
                    if (byte_addr >= addr && byte_addr < end) {
                        bytes[byte_addr - addr] = word_bytes[b];
                    }
                }
            }
        }
        return true;
    }

    /// Drops what B applied.
    void Trim(u64 applied) {
        if (blocks.size() > 64) {
            for (auto it = blocks.begin(); it != blocks.end();) {
                it = it->second->max_seq <= applied ? blocks.erase(it) : std::next(it);
            }
            last_page = ~u64{0};
            last_block = nullptr;
        }
        std::erase_if(poisons, [applied](const Poison& poison) { return poison.seq <= applied; });
    }

private:
    struct Block {
        std::array<u32, 1024> data{};
        std::array<u64, 1024> seq{};
        u64 max_seq{};
    };
    struct Poison {
        u64 begin;
        u64 end;
        u64 seq;
    };

    Block& BlockAt(u64 page) {
        if (page == last_page) {
            return *last_block;
        }
        auto& slot = blocks[page];
        if (!slot) {
            slot = std::make_unique<Block>();
        }
        last_page = page;
        last_block = slot.get();
        return *slot;
    }

    std::unordered_map<u64, std::unique_ptr<Block>> blocks;
    std::vector<Poison> poisons;
    u64 poison_all{};
    u64 posted{};
    u64 last_page{~u64{0}};
    Block* last_block{};
};
CbShadow g_shadow;

// ---- Which slots a draw binds (B publishes, A reads) ----

u64 ProgramKey(const Maxwell3D& maxwell3d) {
    const auto& regs = maxwell3d.regs;
    u64 key = regs.program_region.Address() * 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < Maxwell3D::Regs::MaxShaderProgram; ++i) {
        const auto& pipeline = regs.pipelines[i];
        const u64 word = (u64{pipeline.offset} << 8) |
                         (u64{static_cast<u32>(pipeline.program.Value())} << 1) |
                         (regs.IsShaderConfigEnabled(i) ? 1 : 0);
        key = (std::rotl(key, 13) ^ word) * 0xBF58476D1CE4E5B9ULL;
    }
    return key | 1; // 0: an empty slot
}

/// A seqlock per entry: B writes (version odd while it does), A reads.
struct Usage {
    std::atomic<u32> version{};
    std::atomic<u64> key{};
    std::atomic<u32> mask{};
    std::array<std::atomic<u32>, NumSlots> sizes{};
};
constexpr u32 USAGE_BITS = 10;
std::array<std::array<Usage, size_t{1} << USAGE_BITS>, NumStages> g_usage{};

Usage& UsageSlot(size_t stage, u64 key) {
    return g_usage[stage][(key * 0x94D049BB133111EBULL) >> (64 - USAGE_BITS)];
}

bool ReadUsage(size_t stage, u64 key, u32& mask, std::array<u32, NumSlots>& sizes) {
    const Usage& usage = UsageSlot(stage, key);
    const u32 version = usage.version.load(std::memory_order_acquire);
    if ((version & 1) != 0 || usage.key.load(std::memory_order_relaxed) != key) {
        return false;
    }
    mask = usage.mask.load(std::memory_order_relaxed);
    for (u32 i = 0; i < NumSlots; ++i) {
        sizes[i] = usage.sizes[i].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    return usage.version.load(std::memory_order_relaxed) == version;
}

// ---- The upload ring (A writes, B binds and reclaims) ----

constexpr u64 RING_ALIGN = 256;
/// A reuses a slice only from this far behind its head (the proof of reuse is in Allocate).
constexpr u64 REUSE_WINDOW_DIVISOR = 4;

std::span<u8> g_ring;
CurrentTickFn g_current_tick{};
IsFreeFn g_is_free{};
void* g_tick_context{};
/// The ring position below which A may write again (B, as ticks finish).
std::atomic<u64> g_reclaim{};
std::atomic<bool> g_ready{};

struct Slice {
    u64 device_addr{};
    u64 pos{};
    u32 size{};
    std::vector<u8> bytes;
};

/// A's own.
struct Uploader {
    static constexpr u32 SLICE_BITS = 12;
    std::array<Slice, size_t{1} << SLICE_BITS> slices{};
    std::array<std::array<Slice*, NumSlots>, NumStages> last{};
    std::array<u8, MaxSize> scratch{};
    u64 head{};
    // Counters for the log.
    u64 draws{};
    u64 prepared{};
    u64 reused{};
    u64 uploaded_bytes{};
    u64 full{};
    u64 unknown_stages{};

    u64 Window() const {
        return g_ring.size() / REUSE_WINDOW_DIVISOR;
    }

    bool Usable(const Slice* slice, const u8* bytes, u32 size) const {
        return slice != nullptr && slice->size >= size && head - slice->pos <= Window() &&
               std::memcmp(slice->bytes.data(), bytes, size) == 0;
    }

    /// A ring position for `size` bytes, or none while the ring is full. A slice at P is reused
    /// only while P >= head - W. Bytes at P are written again only by an allocation ending past
    /// P + R, allowed when end <= reclaim + R - W: then reclaim > P + W >= the head when it was
    /// reused, so a tick holding a draw prepared after that reuse is done, and with it every
    /// earlier one, the reusing draw's included.
    std::optional<u64> Allocate(u32 size) {
        const u64 ring_size = g_ring.size();
        const u64 aligned = (u64{size} + RING_ALIGN - 1) & ~(RING_ALIGN - 1);
        u64 pos = head;
        const u64 offset = pos % ring_size;
        if (offset + aligned > ring_size) {
            pos += ring_size - offset;
        }
        const u64 end = pos + aligned;
        if (end > g_reclaim.load(std::memory_order_acquire) + ring_size - Window()) {
            return std::nullopt;
        }
        head = end;
        return pos;
    }

    std::optional<u32> Upload(size_t stage, u32 slot, u64 device_addr, const u8* bytes, u32 size) {
        const u64 ring_size = g_ring.size();
        Slice*& last_slice = last[stage][slot];
        if (Usable(last_slice, bytes, size)) {
            ++reused;
            return static_cast<u32>(last_slice->pos % ring_size);
        }
        Slice& slice = slices[((device_addr >> 8) * 0x9E3779B97F4A7C15ULL) >> (64 - SLICE_BITS)];
        if (slice.device_addr == device_addr && Usable(&slice, bytes, size)) {
            ++reused;
            last_slice = &slice;
            return static_cast<u32>(slice.pos % ring_size);
        }
        const std::optional<u64> pos = Allocate(size);
        if (!pos) {
            ++full;
            return std::nullopt;
        }
        const u32 offset = static_cast<u32>(*pos % ring_size);
        std::memcpy(g_ring.data() + offset, bytes, size);
        if (slice.bytes.size() < MaxSize) {
            slice.bytes.resize(MaxSize);
        }
        std::memcpy(slice.bytes.data(), bytes, size);
        slice.device_addr = device_addr;
        slice.pos = *pos;
        slice.size = size;
        last_slice = &slice;
        uploaded_bytes += size;
        return offset;
    }
};
Uploader g_uploader;

// ---- B's own ----

struct Current {
    bool active{};
    u64 foreign{};
    u64 head{};
    u64 key{};
    u32 known{};
    std::array<std::array<s8, NumSlots>, NumStages> table{};
    std::array<Entry, NumStages * NumSlots> entries{};
    u32 count{};
};
Current g_current;

struct TickHead {
    u64 tick;
    u64 head;
};
std::deque<TickHead> g_ticks;

struct BindCounts {
    u64 binds{};
    u64 used{};
    u64 refused{};
    u64 mismatches{};
};
BindCounts g_binds;

constexpr u64 HEADER_WORDS = 3;

} // Anonymous namespace

void NoteForeign(u64 addr, u64 size) {
    if (Mode() == 0 || size == 0) {
        return;
    }
    NoteForeignRange(addr, addr + size);
}

void NoteForeignAll() {
    if (Mode() == 0) {
        return;
    }
    NoteForeignRange(0, ~u64{0});
}

void NotePostedCb(Maxwell3D& primary, u64 address, const u32* data, u32 count) {
    if (Mode() == 0) {
        return;
    }
    // Counted from the first word on, as B counts the words it applied.
    g_shadow.Write(primary, address, data, count);
}

void NoteAppliedCb(u32 count) {
    if (Mode() == 0) {
        return;
    }
    g_applied.fetch_add(count, std::memory_order_release);
}

void SetRing(std::span<u8> mapped, CurrentTickFn current_tick, IsFreeFn is_free, void* context) {
    g_ring = mapped;
    g_current_tick = current_tick;
    g_is_free = is_free;
    g_tick_context = context;
    g_ready.store(!mapped.empty(), std::memory_order_release);
    for (auto& stage : g_current.table) {
        stage.fill(-1);
    }
}

void Prepare(Core::System& system, Maxwell3D& primary, std::vector<u64>& out) {
    if (Mode() == 0 || !g_ready.load(std::memory_order_acquire) || Nx2Stages::Get().cut_c) {
        return;
    }
    Uploader& uploader = g_uploader;
    // Taken before any guest memory is read.
    const u64 foreign = g_foreign.count.load(std::memory_order_acquire);
    const u64 applied = g_applied.load(std::memory_order_acquire);
    g_shadow.Trim(applied);
    const u64 key = ProgramKey(primary);
    Tegra::MemoryManager& memory_manager = primary.GetMemoryManager();
    auto& device_memory = system.Host1x().MemoryManager();
    const size_t base = out.size();
    out.resize(base + HEADER_WORDS);
    u32 known = 0;
    u32 count = 0;
    std::array<u32, NumSlots> sizes{};
    for (u32 stage = 0; stage < NumStages; ++stage) {
        u32 mask = 0;
        if (!ReadUsage(stage, key, mask, sizes)) {
            ++uploader.unknown_stages;
            continue;
        }
        known |= 1u << stage;
        const auto& buffers = primary.state.shader_stages[stage].const_buffers;
        for (u32 bits = mask; bits != 0; bits &= bits - 1) {
            const u32 slot = static_cast<u32>(std::countr_zero(bits));
            // Bank 17 may stream the root tables instead (BindHostGraphicsUniformBuffer).
            if (slot >= NumSlots || slot == 17) {
                continue;
            }
            const auto& buffer = buffers[slot];
            if (!buffer.enabled) {
                continue;
            }
            const u32 size = (std::min)(buffer.size, sizes[slot]);
            if (size == 0 || size > MaxSize) {
                continue;
            }
            const std::optional<u64> device = memory_manager.GpuToCpuAddress(buffer.address);
            if (!device) {
                continue;
            }
            u8* const bytes = uploader.scratch.data();
            device_memory.ReadBlockUnsafe(*device, bytes, size);
            if (!g_shadow.Overlay(*device, bytes, size, applied)) {
                continue;
            }
            const std::optional<u32> offset = uploader.Upload(stage, slot, *device, bytes, size);
            if (!offset) {
                continue;
            }
            out.push_back(*device);
            out.push_back(u64{*offset} | (u64{size} << 32) | (u64{slot} << 48) |
                          (u64{stage} << 56));
            ++count;
        }
    }
    out[base] = u64{count} | (u64{known} << 32);
    out[base + 1] = foreign;
    out[base + 2] = uploader.head;
    uploader.prepared += count;
    if ((++uploader.draws & ((u64{1} << 20) - 1)) == 0) {
        LOG_INFO(HW_GPU,
                 "nx2 uniform prep (GPUBack): {} draws, {} slices prepared ({} reused), {} MiB "
                 "uploaded, {} with the ring full, {} stages not known yet",
                 uploader.draws, uploader.prepared, uploader.reused,
                 uploader.uploaded_bytes >> 20, uploader.full, uploader.unknown_stages);
    }
}

void BeginDraw(const Maxwell3D& replica, const u64* words, u32 count) {
    if (count < HEADER_WORDS) {
        return;
    }
    Current& current = g_current;
    const u32 entries = static_cast<u32>(words[0] & 0xffffffff);
    current.known = static_cast<u32>(words[0] >> 32);
    current.foreign = words[1];
    current.head = words[2];
    current.key = ProgramKey(replica);
    current.count = 0;
    for (u32 i = 0; i < entries && HEADER_WORDS + 2 * i + 1 < count; ++i) {
        const u64 device_addr = words[HEADER_WORDS + 2 * i];
        const u64 packed = words[HEADER_WORDS + 2 * i + 1];
        const u32 stage = static_cast<u32>(packed >> 56);
        const u32 slot = static_cast<u32>((packed >> 48) & 0xff);
        if (stage >= NumStages || slot >= NumSlots || current.count >= current.entries.size()) {
            continue;
        }
        current.entries[current.count] = Entry{
            .device_addr = device_addr,
            .offset = static_cast<u32>(packed & 0xffffffff),
            .size = static_cast<u32>((packed >> 32) & 0xffff),
        };
        current.table[stage][slot] = static_cast<s8>(current.count);
        ++current.count;
    }
    current.active = true;
}

void EndDraw() {
    Current& current = g_current;
    if (!current.active) {
        return;
    }
    current.active = false;
    for (auto& stage : current.table) {
        stage.fill(-1);
    }
    const u64 tick = g_current_tick(g_tick_context);
    if (!g_ticks.empty() && g_ticks.back().tick == tick) {
        g_ticks.back().head = (std::max)(g_ticks.back().head, current.head);
    } else {
        g_ticks.push_back({tick, current.head});
    }
    while (!g_ticks.empty() && g_is_free(g_tick_context, g_ticks.front().tick)) {
        g_reclaim.store(g_ticks.front().head, std::memory_order_release);
        g_ticks.pop_front();
    }
}

const Entry* Find(size_t stage, u32 slot) {
    const Current& current = g_current;
    if (!current.active || stage >= NumStages || slot >= NumSlots) {
        return nullptr;
    }
    const s8 index = current.table[stage][slot];
    return index < 0 ? nullptr : &current.entries[static_cast<size_t>(index)];
}

bool WrittenSinceRead(u64 addr, u64 size) {
    return ForeignSince(g_current.foreign, addr, size);
}

void PublishUsage(size_t stage, u32 mask, const u32* sizes) {
    const Current& current = g_current;
    if (!current.active || stage >= NumStages || ((current.known >> stage) & 1) != 0 ||
        sizes == nullptr) {
        return;
    }
    Usage& usage = UsageSlot(stage, current.key);
    if (usage.key.load(std::memory_order_relaxed) == current.key &&
        usage.mask.load(std::memory_order_relaxed) == mask) {
        bool same = true;
        for (u32 i = 0; i < NumSlots && same; ++i) {
            same = usage.sizes[i].load(std::memory_order_relaxed) == sizes[i];
        }
        if (same) {
            return;
        }
    }
    const u32 version = usage.version.load(std::memory_order_relaxed);
    usage.version.store(version + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    usage.key.store(current.key, std::memory_order_relaxed);
    usage.mask.store(mask, std::memory_order_relaxed);
    for (u32 i = 0; i < NumSlots; ++i) {
        usage.sizes[i].store(sizes[i], std::memory_order_relaxed);
    }
    usage.version.store(version + 2, std::memory_order_release);
}

std::span<const u8> RingBytes(u32 offset, u32 size) {
    if (u64{offset} + size > g_ring.size()) {
        return {};
    }
    return std::span<const u8>{g_ring.data() + offset, size};
}

void CountBind(bool used, bool mismatch) {
    BindCounts& counts = g_binds;
    ++counts.binds;
    counts.used += used ? 1 : 0;
    counts.refused += used ? 0 : 1;
    counts.mismatches += mismatch ? 1 : 0;
    if ((counts.binds & ((u64{1} << 22) - 1)) == 0) {
        LOG_INFO(HW_GPU,
                 "nx2 uniform prep (GPUMem): {} of {} prepared binds taken, {} refused (a write "
                 "since GPUBack read, or another range or size), {} different (=2)",
                 counts.used, counts.binds, counts.refused, counts.mismatches);
    }
}

} // namespace VideoCommon::Nx2UniformPrep
