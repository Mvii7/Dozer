// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/macro_turing.h"

// EDEN_NX2_HLE_DIRECT (Maxwell3D's split front, FrontRunMacro): NVN's direct draw macros in C++
// instead of the macro JIT. Donkey Kong Bananza calls them ~1,000,000 times a second; most of
// their writes hold the register's value already and are dropped (EDEN_NX2_JIT_SAME), so the
// JIT's run around them is most of their cost. A macro is taken only when its code hashes to
// one of the versions written here (EDEN_NX2_MACRO_DUMP logged them), whatever its position.
// =1 on, =2 verify (every call runs on the JIT; one in 8 also runs in C++ on a copy first and is
// compared with the interpreter's writes and data RAM), 0 or unset off.
namespace VideoCommon::Nx2HleDirect {

using Word = u32;
using DataRam = Tegra::TuringMacroInterpreter::DataRam;
using Result = Tegra::TuringMacroInterpreter::Result;

enum class Kind : u8 {
    Unknown,       ///< Not looked at for this code generation
    None,          ///< Not one of these
    IndexedDraw,   ///< Index buffer, then the draw: {topology, address high, low, format, count}
    VertexFormats, ///< {pairs, formats...}: vertex attribute formats, defaults after the last
    VertexStreams, ///< {pairs, strides..., pairs, enables, divisors...}: vertex streams
};

inline u32 Mode() {
    static const u32 mode = [] {
        const char* value = std::getenv("EDEN_NX2_HLE_DIRECT");
        if (value == nullptr || (value[0] != '1' && value[0] != '2')) {
            return 0U;
        }
        return static_cast<u32>(value[0] - '0');
    }();
    return mode;
}

inline const char* Name(Kind kind) {
    switch (kind) {
    case Kind::IndexedDraw:
        return "indexed draw";
    case Kind::VertexFormats:
        return "vertex attribute formats";
    case Kind::VertexStreams:
        return "vertex streams";
    default:
        return "none";
    }
}

/// Which of these the macro at `start` is (its instructions up to its end's delay slot).
inline Kind Identify(std::span<const Word> code, Word start) {
    struct Known {
        Kind kind;
        u32 instructions;
        u64 hash;
    };
    static constexpr std::array<Known, 3> known{{
        {Kind::IndexedDraw, 4, 0xb7ca2881b1bc4ddbULL},
        {Kind::VertexFormats, 9, 0xb34db254aae47de2ULL},
        {Kind::VertexStreams, 18, 0x9dde30d53c3897acULL},
    }};
    for (const Known& entry : known) {
        const size_t begin = size_t{start} * 3;
        const size_t words = size_t{entry.instructions} * 3;
        if (begin + words > code.size()) {
            continue;
        }
        const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(code.data() + begin),
                                            words * sizeof(Word));
        if (hash == entry.hash) {
            return entry.kind;
        }
    }
    return Kind::None;
}

/// Runs the macro as its code does, `emit(method, value)` for each write in its order. Returns
/// {false, ...} before any write or data RAM change when the call is one the C++ version does
/// not cover (too few parameters, writes past the register arrays): the JIT runs it then.
template <typename Emit>
Result Run(Kind kind, std::span<const Word> p, DataRam& dram, Emit&& emit) {
    const auto fail = [](std::string_view why) { return Result{false, 0, 0, why}; };
    const auto positive = [](Word value) { return static_cast<s32>(value) > 0; };
    switch (kind) {
    case Kind::IndexedDraw: {
        if (p.size() < 5) {
            return fail("too few parameters");
        }
        emit(0x586, p[0]);
        emit(0x5f2, p[1]);
        emit(0x5f3, p[2]);
        emit(0x5f6, p[3]);
        emit(0x5f7, 0);
        emit(0x5f8, p[4]);
        emit(0x585, 0);
        return {true, 0, 5, {}};
    }
    case Kind::VertexFormats: {
        // 0x458 on, two formats a pair; then 0x3a400040 for the pairs the last call wrote past
        // these. Data RAM 0x54c keeps the count.
        if (p.empty()) {
            return fail("too few parameters");
        }
        const Word pairs = p[0];
        const Word extra = dram[0x54c] - pairs;
        const u64 fill = positive(extra) ? extra : 0;
        if (pairs > 16 || u64{pairs} + fill > 16) {
            return fail("past the attribute formats");
        }
        if (p.size() < 1 + size_t{pairs} * 2) {
            return fail("too few parameters");
        }
        Word method = 0x458;
        for (Word i = 0; i < pairs * 2; ++i) {
            emit(method++, p[1 + i]);
        }
        dram[0x54c] = pairs;
        for (u64 i = 0; i < fill * 2; ++i) {
            emit(method++, 0x3a400040);
        }
        return {true, 0, 1 + size_t{pairs} * 2, {}};
    }
    case Kind::VertexStreams: {
        // Stream words 0x700 on, a stride every 4, two streams a pair, 0 for the pairs the last
        // call wrote past these (data RAM 0x54d). Then pairs of divisors at 0x703 on, every 4,
        // and two enable bits a pair from one word at 0x620 on, 0 past them (data RAM 0x54e).
        if (p.empty()) {
            return fail("too few parameters");
        }
        const Word pairs = p[0];
        const Word extra = dram[0x54d] - pairs;
        const u64 fill = positive(extra) ? extra : 0;
        if (pairs > 8 || u64{pairs} + fill > 8) {
            return fail("past the vertex streams");
        }
        const size_t second = 1 + size_t{pairs} * 2;
        if (p.size() < second + 2) {
            return fail("too few parameters");
        }
        const Word instanced = p[second];
        const Word extra2 = dram[0x54e] - instanced;
        const u64 fill2 = positive(extra2) ? extra2 : 0;
        if (instanced > 8 || u64{instanced} + fill2 > 8) {
            return fail("past the vertex streams");
        }
        const size_t consumed = second + 2 + size_t{instanced} * 2;
        if (p.size() < consumed) {
            return fail("too few parameters");
        }
        Word method = 0x700;
        for (Word i = 0; i < pairs * 2; ++i, method += 4) {
            emit(method, p[1 + i]);
        }
        dram[0x54d] = pairs;
        for (u64 i = 0; i < fill * 2; ++i, method += 4) {
            emit(method, 0);
        }
        Word bits = p[second + 1];
        method = 0x703;
        for (Word i = 0; i < instanced * 2; ++i, method += 4) {
            emit(method, p[second + 2 + i]);
        }
        method = 0x620;
        for (Word i = 0; i < instanced; ++i) {
            emit(method++, bits & 1);
            emit(method++, (bits >> 1) & 1);
            bits = (bits >> 2) & 0x3fffffff;
        }
        dram[0x54e] = instanced;
        for (u64 i = 0; i < fill2 * 2; ++i) {
            emit(method++, 0);
        }
        return {true, 0, consumed, {}};
    }
    default:
        return fail("not a known macro");
    }
}

/// EDEN_NX2_HLE_DIRECT=2: the C++ version against the interpreter, both on copies of the data RAM
/// (front thread, before the real call). Logs the first mismatches and returns whether they
/// agreed.
class Verifier {
public:
    bool Check(Kind kind, std::span<const Word> code, Word start, std::span<const Word> parameters,
               const DataRam& data_ram) {
        // One call in 8: the interpreter costs about as much as the front's other work.
        if ((++seen & 7) != 0) {
            return true;
        }
        ++calls;
        native_ram = data_ram;
        native_writes.clear();
        const Result native = Run(kind, parameters, native_ram,
                                  [&](Word method, Word value) {
                                      native_writes.emplace_back(method, value);
                                  });
        interpreter_ram = data_ram;
        interpreter.active_dram = &interpreter_ram;
        fifo.assign(parameters.begin(), parameters.end());
        interpreter_writes.clear();
        const Result reference = interpreter.Execute(
            code, start, fifo, [](Word) { return Word{0}; },
            [&](Word method, Word value) {
                interpreter_writes.emplace_back(method, value);
                return method < 0xe00;
            });
        if (!native.success) {
            ++declined;
            return true;
        }
        const bool same = reference.success && native.consumed == reference.consumed &&
                          native_writes == interpreter_writes && native_ram == interpreter_ram;
        if (!same) {
            ++mismatches;
            if (mismatches <= 16 || (mismatches & (mismatches - 1)) == 0) {
                LOG_ERROR(HW_GPU,
                          "HLE direct (EDEN_NX2_HLE_DIRECT=2): {} at {:#x} differs from the "
                          "macro ({} of {} calls): parameters [{}] | C++ consumed {} writes [{}] "
                          "| macro {} consumed {} writes [{}] | data RAM {}",
                          Name(kind), start, mismatches, calls, Join(parameters), native.consumed,
                          Join(native_writes), reference.success ? "ok" : reference.error,
                          reference.consumed, Join(interpreter_writes),
                          native_ram == interpreter_ram ? "same" : "differs");
            }
        }
        if ((calls & ((u64{1} << 20) - 1)) == 0) {
            LOG_INFO(HW_GPU,
                     "HLE direct (EDEN_NX2_HLE_DIRECT=2): {} calls checked, {} mismatches, {} "
                     "left to the JIT",
                     calls, mismatches, declined);
        }
        return same;
    }

private:
    static std::string Join(std::span<const Word> words) {
        std::string text;
        for (const Word word : words) {
            fmt::format_to(std::back_inserter(text), "{}{:x}", text.empty() ? "" : ",", word);
        }
        return text;
    }
    static std::string Join(const std::vector<std::pair<Word, Word>>& writes) {
        std::string text;
        for (const auto& [method, value] : writes) {
            fmt::format_to(std::back_inserter(text), "{}{:x}={:x}", text.empty() ? "" : ",",
                           method, value);
        }
        return text;
    }

    Tegra::TuringMacroInterpreter interpreter;
    DataRam native_ram{};
    DataRam interpreter_ram{};
    std::vector<Word> fifo;
    std::vector<std::pair<Word, Word>> native_writes;
    std::vector<std::pair<Word, Word>> interpreter_writes;
    u64 seen{};
    u64 calls{};
    u64 mismatches{};
    u64 declined{};
};

} // namespace VideoCommon::Nx2HleDirect
