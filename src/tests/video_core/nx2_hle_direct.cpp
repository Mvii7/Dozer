// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/macro_turing.h"
#include "video_core/nx2_hle_direct.h"

// EDEN_NX2_HLE_DIRECT: the C++ macros (nx2_hle_direct.h's Run) against the interpreter running MME
// programs written here by hand from the instruction fields to do what each macro does (no game
// code), on random parameters and data RAM.
namespace {

using Word = std::uint32_t;
using VideoCommon::Nx2HleDirect::DataRam;
using VideoCommon::Nx2HleDirect::Kind;

enum Op : Word {
    Add = 0, Sub = 2, Merge = 16, Loop = 23, Ble = 27, DRead = 30, DWrite = 31,
};
constexpr Word Zero = 24, ImmS = 25, Imm32 = 27, Load0 = 28;
constexpr Word OutAlu0 = 1, OutAlu1 = 2, OutLoad0 = 3, OutLoad1 = 4, OutImm0 = 5, OutImm1 = 6;

struct LaneFields {
    Word op = Add, dst = Zero, a = Zero, b = Zero, imm = 0;
};

struct Ins {
    bool end = false;
    std::array<LaneFields, 2> lane{};
    std::array<Word, 2> method{}, emit{};

    Ins& End() { end = true; return *this; }
    Ins& Alu(Word l, Word op, Word dst, Word a, Word b, Word imm = 0) {
        lane[l] = {op, dst, a, b, imm};
        return *this;
    }
    Ins& Imm(Word l, Word imm) { lane[l].imm = imm; return *this; }
    Ins& Method(Word l, Word selector) { method[l] = selector; return *this; }
    Ins& Emit(Word l, Word selector) { emit[l] = selector; return *this; }
};

constexpr Word M(Word method, Word increment) {
    return (increment << 12) | method;
}

/// A branch immediate: taken when the condition is true; -4096 ends the macro.
constexpr Word BranchImm(int offset) {
    return 0x8000U | (static_cast<Word>(offset) & 0x1fffU);
}

std::array<Word, 3> Encode(const Ins& ins) {
    std::array<Word, 3> bits{};
    const auto field = [&](Word offset, Word count, Word value) {
        for (Word bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1U) << ((offset + bit) % 32);
        }
    };
    field(0, 1, ins.end ? 1U : 0U);
    for (Word l = 0; l < 2; ++l) {
        const Word base = 10 + 36 * l;
        field(base, 5, ins.lane[l].op);
        field(base + 5, 5, ins.lane[l].dst);
        field(base + 10, 5, ins.lane[l].a);
        field(base + 15, 5, ins.lane[l].b);
        field(base + 20, 16, ins.lane[l].imm);
    }
    field(82, 3, ins.method[0]);
    field(85, 4, ins.emit[0]);
    field(89, 3, ins.method[1]);
    field(92, 4, ins.emit[1]);
    return {bits[2], bits[1], bits[0]};
}

std::vector<Word> Assemble(const std::vector<Ins>& code) {
    std::vector<Word> words;
    for (const Ins& ins : code) {
        const auto encoded = Encode(ins);
        words.insert(words.end(), encoded.begin(), encoded.end());
    }
    return words;
}

// {topology, address high, address low, format, count}: draw begin, the index buffer, first 0,
// the count, draw end.
std::vector<Ins> IndexedDraw() {
    return {
        Ins{}.Imm(0, M(0x586, 0)).Method(0, OutImm0).Emit(0, OutLoad0),
        Ins{}.Imm(0, M(0x5f2, 1)).Method(0, OutImm0).Emit(0, OutLoad0),
        Ins{}.Imm(0, M(0x5f3, 3)).Method(0, OutImm0).Emit(0, OutLoad0).Emit(1, OutLoad1),
        Ins{}.Imm(0, M(0x5f7, 1)).Method(0, OutImm0).Emit(0, OutAlu0).Emit(1, OutLoad0).End(),
        Ins{}.Imm(0, M(0x585, 0)).Method(0, OutImm0).Emit(0, OutAlu0),
    };
}

// {pairs, formats...}: two formats a pair from 0x458, then 0x3a400040 for the pairs the last
// call wrote past these (data RAM 0x54c).
std::vector<Ins> VertexFormats() {
    return {
        Ins{}.Alu(0, Add, 1, Load0, Zero).Alu(1, DRead, 2, ImmS, Zero, 0x54c),
        Ins{}.Alu(0, Add, 3, Imm32, Zero, 0x3a40).Imm(1, 0x0040),
        Ins{}.Alu(0, Loop, Zero, 1, Zero, 2).Alu(1, Sub, 2, 2, 1, M(0x458, 1)).Method(1, OutImm1),
        Ins{}.Emit(0, OutLoad0).Emit(1, OutLoad1),
        Ins{}.Alu(0, Ble, Zero, 2, Zero, BranchImm(-4096)).Alu(1, DWrite, Zero, ImmS, 1, 0x54c),
        Ins{}.Alu(0, Loop, Zero, 2, Zero, 2),
        Ins{}.Alu(0, Add, Zero, 3, Zero).Alu(1, Add, Zero, 3, Zero).Emit(0, OutAlu0).Emit(1, OutAlu1),
        Ins{}.End(),
        Ins{},
    };
}

// {pairs, strides..., pairs, enable bits, divisors...}: stream words from 0x700 every 4, zeros
// past them (data RAM 0x54d); divisors from 0x703 every 4; two enable bits a pair from 0x620,
// zeros past them (data RAM 0x54e).
std::vector<Ins> VertexStreams() {
    return {
        Ins{}.Alu(0, Add, 1, Load0, Zero).Alu(1, DRead, 2, ImmS, Zero, 0x54d),
        Ins{}.Alu(0, Loop, Zero, 1, Zero, 2).Imm(1, M(0x700, 4)).Method(1, OutImm1),
        Ins{}.Emit(0, OutLoad0).Emit(1, OutLoad1),
        Ins{}.Alu(0, Sub, 2, 2, 1).Alu(1, DWrite, Zero, ImmS, 1, 0x54d),
        Ins{}.Alu(0, Ble, Zero, 2, Zero, BranchImm(3)),
        Ins{}.Alu(0, Loop, Zero, 2, Zero, 2),
        Ins{}.Emit(0, OutImm0).Emit(1, OutImm0),
        Ins{}.Alu(0, Add, 1, Load0, Zero).Alu(1, DRead, 2, ImmS, Zero, 0x54e),
        Ins{}.Alu(0, Add, 4, Load0, Zero).Imm(1, M(0x703, 4)).Method(1, OutImm1),
        Ins{}.Alu(0, Loop, Zero, 1, Zero, 2),
        Ins{}.Emit(0, OutLoad0).Emit(1, OutLoad1),
        Ins{}.Alu(0, Loop, Zero, 1, Zero, 3).Alu(1, Sub, 2, 2, 1, M(0x620, 1)).Method(1, OutImm1),
        Ins{}.Alu(0, Merge, Zero, Zero, 4, 0x20).Alu(1, Merge, Zero, Zero, 4, 0x21)
            .Emit(0, OutAlu0).Emit(1, OutAlu1),
        Ins{}.Alu(0, Merge, 4, Zero, 4, 0x3c2),
        Ins{}.Alu(0, Ble, Zero, 2, Zero, BranchImm(-4096)).Alu(1, DWrite, Zero, ImmS, 1, 0x54e),
        Ins{}.Alu(0, Loop, Zero, 2, Zero, 2),
        Ins{}.Emit(0, OutImm0).Emit(1, OutImm0),
        Ins{}.End(),
        Ins{},
    };
}

struct Outcome {
    bool success{};
    std::size_t consumed{};
    std::vector<std::pair<Word, Word>> writes;
    DataRam ram{};
};

Outcome RunCpp(Kind kind, const std::vector<Word>& parameters, const DataRam& ram) {
    Outcome out;
    out.ram = ram;
    const auto result = VideoCommon::Nx2HleDirect::Run(
        kind, parameters, out.ram, [&](Word method, Word value) { out.writes.emplace_back(method, value); });
    out.success = result.success;
    out.consumed = result.consumed;
    return out;
}

Outcome RunMacro(const std::vector<Word>& code, const std::vector<Word>& parameters,
                 const DataRam& ram) {
    Outcome out;
    out.ram = ram;
    Tegra::TuringMacroInterpreter interpreter;
    interpreter.active_dram = &out.ram;
    std::vector<Word> fifo = parameters;
    const auto result = interpreter.Execute(
        code, 0, fifo, [](Word) { return Word{0}; },
        [&](Word method, Word value) {
            out.writes.emplace_back(method, value);
            return method < 0xe00;
        });
    out.success = result.success;
    out.consumed = result.consumed;
    return out;
}

/// Random calls of `kind`: counts mostly in range (and the last call's counts in data RAM), now
/// and then past it or with too few parameters, which the C++ version must decline.
void Compare(Kind kind, const std::vector<Ins>& program, Word max_pairs) {
    const std::vector<Word> code = Assemble(program);
    std::mt19937 rng{0x5eed};
    int checked = 0;
    int declined = 0;
    for (int iteration = 0; iteration < 20000; ++iteration) {
        DataRam ram{};
        for (const Word address : {0x54cU, 0x54dU, 0x54eU}) {
            ram[address] = rng() % 16 == 0 ? rng() : rng() % (max_pairs + 2);
        }
        const auto count = [&] { return rng() % 16 == 0 ? rng() % 64 : rng() % (max_pairs + 1); };
        std::vector<Word> parameters;
        const Word pairs = count();
        parameters.push_back(pairs);
        for (Word i = 0; i < pairs * 2 && parameters.size() < 64; ++i) {
            parameters.push_back(rng());
        }
        if (kind == Kind::VertexStreams) {
            const Word instanced = count();
            parameters.push_back(instanced);
            parameters.push_back(rng());
            for (Word i = 0; i < instanced * 2 && parameters.size() < 96; ++i) {
                parameters.push_back(rng());
            }
        }
        if (kind == Kind::IndexedDraw) {
            parameters.resize(rng() % 8 == 0 ? rng() % 5 : 5);
            for (Word& word : parameters) {
                word = rng();
            }
        }
        if (rng() % 16 == 0 && !parameters.empty()) {
            parameters.resize(rng() % parameters.size());
        }
        const Outcome cpp = RunCpp(kind, parameters, ram);
        if (!cpp.success) {
            // Nothing written before it declines.
            REQUIRE(cpp.writes.empty());
            REQUIRE(cpp.ram == ram);
            ++declined;
            continue;
        }
        const Outcome macro = RunMacro(code, parameters, ram);
        REQUIRE(macro.success);
        REQUIRE(cpp.consumed == macro.consumed);
        REQUIRE(cpp.writes == macro.writes);
        REQUIRE(cpp.ram == macro.ram);
        ++checked;
    }
    REQUIRE(checked > 10000);
    REQUIRE(declined > 0);
}

} // namespace

TEST_CASE("HLE direct indexed draw matches the macro", "[video_core][nx2_hle_direct]") {
    Compare(Kind::IndexedDraw, IndexedDraw(), 8);
}

TEST_CASE("HLE direct vertex attribute formats match the macro", "[video_core][nx2_hle_direct]") {
    Compare(Kind::VertexFormats, VertexFormats(), 16);
}

TEST_CASE("HLE direct vertex streams match the macro", "[video_core][nx2_hle_direct]") {
    Compare(Kind::VertexStreams, VertexStreams(), 8);
}
