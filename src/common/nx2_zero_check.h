// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstring>

#include "common/common_types.h"

#ifdef ARCHITECTURE_x86_64
#include <emmintrin.h>
#endif

namespace Common {

/// True when all `size` bytes at `data` are zero. Checks 4 KiB at a time (memory bound).
[[nodiscard]] inline bool IsAllZero(const u8* data, std::size_t size) {
#ifdef ARCHITECTURE_x86_64
    for (; size >= 4096; data += 4096, size -= 4096) {
        __m128i acc0 = _mm_setzero_si128();
        __m128i acc1 = _mm_setzero_si128();
        __m128i acc2 = _mm_setzero_si128();
        __m128i acc3 = _mm_setzero_si128();
        for (std::size_t i = 0; i < 4096; i += 64) {
            const auto* p = reinterpret_cast<const __m128i*>(data + i);
            acc0 = _mm_or_si128(acc0, _mm_loadu_si128(p));
            acc1 = _mm_or_si128(acc1, _mm_loadu_si128(p + 1));
            acc2 = _mm_or_si128(acc2, _mm_loadu_si128(p + 2));
            acc3 = _mm_or_si128(acc3, _mm_loadu_si128(p + 3));
        }
        const __m128i acc = _mm_or_si128(_mm_or_si128(acc0, acc1), _mm_or_si128(acc2, acc3));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(acc, _mm_setzero_si128())) != 0xFFFF) {
            return false;
        }
    }
#endif
    for (; size >= 8; data += 8, size -= 8) {
        u64 word;
        std::memcpy(&word, data, sizeof(word));
        if (word != 0) {
            return false;
        }
    }
    for (; size > 0; ++data, --size) {
        if (*data != 0) {
            return false;
        }
    }
    return true;
}

} // namespace Common
