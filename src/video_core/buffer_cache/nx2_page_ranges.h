// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/range_sets.h"

namespace VideoCommon {

/// A set of byte ranges of the 64 GiB device space, by 4 MiB region and 4 KiB page: a region is
/// empty, whole or has a slot per page, and a page is empty, whole or has a bit per byte. Adding
/// or removing a range costs its pages (its whole regions one step each), not a tree walk among
/// all the others. Ranges past the space go to a Common::RangeSet. Same answers as one.
class PageRangeSet {
public:
    static constexpr u32 PAGE_BITS = 12;
    static constexpr u32 REGION_BITS = 22;
    static constexpr u32 SPACE_BITS = 36;
    static constexpr u64 PAGE_BYTES = u64{1} << PAGE_BITS;
    static constexpr u64 REGION_BYTES = u64{1} << REGION_BITS;
    static constexpr u64 SPACE_BYTES = u64{1} << SPACE_BITS;
    static constexpr u32 PAGES_PER_REGION = 1u << (REGION_BITS - PAGE_BITS);
    static constexpr u32 NUM_REGIONS = 1u << (SPACE_BITS - REGION_BITS);
    static constexpr u32 WORDS = PAGE_BYTES / 64;

    PageRangeSet() = default;
    PageRangeSet(PageRangeSet&&) = default;
    PageRangeSet& operator=(PageRangeSet&&) = default;

    void Add(u64 addr, size_t size) {
        const u64 end = End(addr, size);
        if (end > SPACE_BYTES) {
            const u64 outside = (std::max)(addr, SPACE_BYTES);
            beyond.Add(outside, end - outside);
        }
        if (!regions) {
            regions = std::make_unique<Region[]>(NUM_REGIONS);
        }
        ForEachRegion(addr, end, [&](u32 region, u64 begin, u64 stop, bool whole) {
            if (whole) {
                MakeRegion(region, Full);
                return true;
            }
            Region& entry = regions[region];
            if (entry.state == Full) {
                return true;
            }
            if (entry.state == Unused) {
                MakeRegion(region, Mixed);
            }
            // The partial pages at the ends, and a plain loop over the whole ones between (a
            // storage binding added again each draw: its pages mostly whole already).
            const u64 first_whole = (begin + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
            const u64 last_whole = stop & ~(PAGE_BYTES - 1);
            if (first_whole >= last_whole) {
                ForEachPage(begin, stop, [&](u32 page, u32 lo, u32 hi) {
                    SetBits(entry, page, lo, hi);
                    return true;
                });
            } else {
                ForEachPage(begin, first_whole, [&](u32 page, u32 lo, u32 hi) {
                    SetBits(entry, page, lo, hi);
                    return true;
                });
                const u32 first = PageIndex(first_whole);
                const u32 last = PageIndex(last_whole - 1) + 1;
                u32* const slots = entry.slots.get();
                for (u32 page = first; page < last; ++page) {
                    if (slots[page] != FullSlot) {
                        SetBits(entry, page, 0, static_cast<u32>(PAGE_BYTES));
                    }
                }
                ForEachPage(last_whole, stop, [&](u32 page, u32 lo, u32 hi) {
                    SetBits(entry, page, lo, hi);
                    return true;
                });
            }
            if (entry.full == PAGES_PER_REGION) {
                MakeRegion(region, Full);
            }
            return true;
        });
    }

    void Subtract(u64 addr, size_t size) {
        const u64 end = End(addr, size);
        if (end > SPACE_BYTES) {
            const u64 outside = (std::max)(addr, SPACE_BYTES);
            beyond.Subtract(outside, end - outside);
        }
        if (!regions) {
            return;
        }
        ForEachRegion(addr, end, [&](u32 region, u64 begin, u64 stop, bool whole) {
            Region& entry = regions[region];
            if (entry.state == Unused) {
                return true;
            }
            if (whole) {
                MakeRegion(region, Unused);
                return true;
            }
            if (entry.state == Full) {
                MakeRegion(region, Mixed, true);
            }
            ForEachPage(begin, stop, [&](u32 page, u32 lo, u32 hi) {
                ClearBits(entry, page, lo, hi);
                return true;
            });
            if (entry.used == 0) {
                MakeRegion(region, Unused);
            }
            return true;
        });
    }

    [[nodiscard]] bool Intersects(u64 addr, size_t size) const {
        if (size == 0) {
            return false;
        }
        const u64 end = End(addr, size);
        if (end > SPACE_BYTES) {
            const u64 outside = (std::max)(addr, SPACE_BYTES);
            if (beyond.Intersects(outside, end - outside)) {
                return true;
            }
        }
        if (!regions || used_regions == 0) {
            return false;
        }
        bool found = false;
        ForEachRegion(addr, end, [&](u32 region, u64 begin, u64 stop, bool) {
            const Region& entry = regions[region];
            if (entry.state == Unused) {
                return true;
            }
            if (entry.state == Full) {
                found = true;
                return false;
            }
            ForEachPage(begin, stop, [&](u32 page, u32 lo, u32 hi) {
                const u32 slot = entry.slots[page];
                if (slot == EmptySlot) {
                    return true;
                }
                // A bitmap is freed once its last bit is cleared: a whole page query needs no scan.
                if (slot == FullSlot || (lo == 0 && hi == PAGE_BYTES) ||
                    AnyBits(bitmaps[slot - FirstBitmap], lo, hi)) {
                    found = true;
                    return false;
                }
                return true;
            });
            return !found;
        });
        return found;
    }

    /// Calls `func(begin, end)` for each range in [addr, addr + size), clipped to it, ascending.
    template <typename Func>
    void ForEachInRange(u64 addr, size_t size, Func&& func) const {
        if (size == 0) {
            return;
        }
        const u64 end = End(addr, size);
        Runs<Func> runs{func};
        if (regions && used_regions != 0) {
            ForEachRegion(addr, end, [&](u32 region, u64 begin, u64 stop, bool) {
                const Region& entry = regions[region];
                if (entry.state == Full) {
                    runs.Put(begin, stop);
                } else if (entry.state == Mixed) {
                    const u64 base = u64{region} << REGION_BITS;
                    ForEachPage(begin, stop, [&](u32 page, u32 lo, u32 hi) {
                        const u32 slot = entry.slots[page];
                        const u64 page_base = base + (u64{page} << PAGE_BITS);
                        if (slot == FullSlot) {
                            runs.Put(page_base + lo, page_base + hi);
                        } else if (slot != EmptySlot) {
                            ForEachBitRun(bitmaps[slot - FirstBitmap], lo, hi,
                                          [&](u32 run_lo, u32 run_hi) {
                                              runs.Put(page_base + run_lo, page_base + run_hi);
                                          });
                        }
                        return true;
                    });
                }
                return true;
            });
        }
        if (end > SPACE_BYTES) {
            const u64 outside = (std::max)(addr, SPACE_BYTES);
            beyond.ForEachInRange(outside, end - outside,
                                  [&](u64 begin, u64 stop) { runs.Put(begin, stop); });
        }
        runs.Flush();
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        ForEachInRange(0, ~size_t{0}, std::forward<Func>(func));
    }

    void Clear() {
        regions.reset();
        bitmaps.clear();
        free_bitmaps.clear();
        used_regions = 0;
        beyond.Clear();
    }

    [[nodiscard]] bool Empty() const {
        return used_regions == 0 && beyond.Empty();
    }

private:
    enum State : u8 { Unused, Full, Mixed };
    static constexpr u32 EmptySlot = 0;
    static constexpr u32 FullSlot = 1;
    static constexpr u32 FirstBitmap = 2;

    using Bitmap = std::array<u64, WORDS>;

    struct Region {
        State state = Unused;
        u32 used = 0; ///< Mixed: slots not empty
        u32 full = 0; ///< Mixed: slots whole
        std::unique_ptr<u32[]> slots;
    };

    template <typename Func>
    struct Runs {
        Func& func;
        u64 begin = 0;
        u64 end = 0;
        bool any = false;

        void Put(u64 run_begin, u64 run_end) {
            if (any && run_begin == end) {
                end = run_end;
                return;
            }
            Flush();
            begin = run_begin;
            end = run_end;
            any = true;
        }
        void Flush() {
            if (any) {
                func(begin, end);
                any = false;
            }
        }
    };

    static u32 PageIndex(u64 addr) noexcept {
        return static_cast<u32>((addr >> PAGE_BITS) & (PAGES_PER_REGION - 1));
    }

    static u64 End(u64 addr, size_t size) noexcept {
        const u64 end = addr + size;
        return end < addr ? ~u64{0} : end;
    }

    /// Calls `func(region, begin, end, whole)` for each region [addr, end) reaches inside the
    /// space, until it returns false.
    template <typename Func>
    static void ForEachRegion(u64 addr, u64 end, Func&& func) {
        end = (std::min)(end, SPACE_BYTES);
        while (addr < end) {
            const u64 region_end = (addr | (REGION_BYTES - 1)) + 1;
            const u64 stop = (std::min)(end, region_end);
            const bool whole = (addr & (REGION_BYTES - 1)) == 0 && stop == region_end;
            if (!func(static_cast<u32>(addr >> REGION_BITS), addr, stop, whole)) {
                return;
            }
            addr = stop;
        }
    }

    /// Calls `func(page, lo, hi)` for each page of one region [addr, end) reaches, with the byte
    /// offsets in it, until it returns false.
    template <typename Func>
    static void ForEachPage(u64 addr, u64 end, Func&& func) {
        while (addr < end) {
            const u64 page_end = (addr | (PAGE_BYTES - 1)) + 1;
            const u64 stop = (std::min)(end, page_end);
            const u32 page = static_cast<u32>((addr >> PAGE_BITS) & (PAGES_PER_REGION - 1));
            const u32 lo = static_cast<u32>(addr & (PAGE_BYTES - 1));
            const u32 hi = static_cast<u32>(stop - (page_end - PAGE_BYTES));
            if (!func(page, lo, hi)) {
                return;
            }
            addr = stop;
        }
    }

    /// Makes `region` Unused, Full or Mixed (its slots all full when `full_slots`).
    void MakeRegion(u32 region, State state, bool full_slots = false) {
        Region& entry = regions[region];
        if (entry.state == Mixed) {
            for (u32 page = 0; page < PAGES_PER_REGION; ++page) {
                FreeSlot(entry.slots[page]);
            }
        }
        if (entry.state == Unused && state != Unused) {
            ++used_regions;
        } else if (entry.state != Unused && state == Unused) {
            --used_regions;
        }
        entry.state = state;
        entry.used = 0;
        entry.full = 0;
        if (state == Mixed) {
            if (!entry.slots) {
                entry.slots = std::make_unique<u32[]>(PAGES_PER_REGION);
            }
            std::fill_n(entry.slots.get(), PAGES_PER_REGION, full_slots ? FullSlot : EmptySlot);
            entry.used = full_slots ? PAGES_PER_REGION : 0;
            entry.full = entry.used;
        }
    }

    void FreeSlot(u32& slot) {
        if (slot >= FirstBitmap) {
            free_bitmaps.push_back(slot - FirstBitmap);
        }
        slot = EmptySlot;
    }

    u32 NewBitmap(bool ones) {
        u32 index;
        if (free_bitmaps.empty()) {
            index = static_cast<u32>(bitmaps.size());
            bitmaps.emplace_back();
        } else {
            index = free_bitmaps.back();
            free_bitmaps.pop_back();
        }
        bitmaps[index].fill(ones ? ~u64{0} : 0);
        return index + FirstBitmap;
    }

    void SetBits(Region& entry, u32 page, u32 lo, u32 hi) {
        u32& slot = entry.slots[page];
        if (slot == FullSlot) {
            return;
        }
        if (lo == 0 && hi == PAGE_BYTES) {
            if (slot == EmptySlot) {
                ++entry.used;
            }
            FreeSlot(slot);
            slot = FullSlot;
            ++entry.full;
            return;
        }
        if (slot == EmptySlot) {
            slot = NewBitmap(false);
            ++entry.used;
        }
        ForEachWord(lo, hi, [&](u32 word, u64 mask) { bitmaps[slot - FirstBitmap][word] |= mask; });
    }

    void ClearBits(Region& entry, u32 page, u32 lo, u32 hi) {
        u32& slot = entry.slots[page];
        if (slot == EmptySlot) {
            return;
        }
        if (slot == FullSlot) {
            --entry.full;
        }
        if (lo == 0 && hi == PAGE_BYTES) {
            FreeSlot(slot);
            --entry.used;
            return;
        }
        const bool was_full = slot == FullSlot;
        if (was_full) {
            slot = NewBitmap(true);
        }
        Bitmap& bits = bitmaps[slot - FirstBitmap];
        ForEachWord(lo, hi, [&](u32 word, u64 mask) { bits[word] &= ~mask; });
        if (was_full) {
            return; // Part of the page was cleared
        }
        for (const u64 word : bits) {
            if (word != 0) {
                return;
            }
        }
        FreeSlot(slot);
        --entry.used;
    }

    /// Calls `func(word, mask)` for the bits [lo, hi) of a page's bitmap.
    template <typename Func>
    static void ForEachWord(u32 lo, u32 hi, Func&& func) {
        while (lo < hi) {
            const u32 word = lo / 64;
            const u32 stop = (std::min)(hi, (word + 1) * 64);
            const u32 count = stop - lo;
            const u64 mask = (count == 64 ? ~u64{0} : ((u64{1} << count) - 1)) << (lo % 64);
            func(word, mask);
            lo = stop;
        }
    }

    static bool AnyBits(const Bitmap& bits, u32 lo, u32 hi) {
        bool any = false;
        ForEachWord(lo, hi, [&](u32 word, u64 mask) { any = any || (bits[word] & mask) != 0; });
        return any;
    }

    /// Calls `func(lo, hi)` for each run of set bits in [lo, hi), clipped to it.
    template <typename Func>
    static void ForEachBitRun(const Bitmap& bits, u32 lo, u32 hi, Func&& func) {
        u32 pos = lo;
        while (pos < hi) {
            // The next set bit at or after pos
            u32 word = pos / 64;
            u64 value = bits[word] & (~u64{0} << (pos % 64));
            while (value == 0 && ++word < WORDS && word * 64 < hi) {
                value = bits[word];
            }
            if (value == 0) {
                return;
            }
            const u32 start = word * 64 + static_cast<u32>(std::countr_zero(value));
            if (start >= hi) {
                return;
            }
            // The next clear bit after start
            word = start / 64;
            value = ~bits[word] & (~u64{0} << (start % 64));
            while (value == 0 && ++word < WORDS) {
                value = ~bits[word];
            }
            const u32 stop = value == 0 ? static_cast<u32>(PAGE_BYTES)
                                        : word * 64 + static_cast<u32>(std::countr_zero(value));
            func(start, (std::min)(stop, hi));
            pos = stop;
        }
    }

    std::unique_ptr<Region[]> regions;
    std::vector<Bitmap> bitmaps;
    std::vector<u32> free_bitmaps;
    u32 used_regions = 0;
    Common::RangeSet<u64> beyond;
};

} // namespace VideoCommon
