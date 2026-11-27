// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/macro_turing.h"

// EDEN_NX2_MACRO_DUMP=<n> (Maxwell3D's split front, FrontRunMacro): for the macros called most,
// log their code once and <n> of their calls (default 16, spread over the run), each run again
// on a copy of the data RAM: parameters, data RAM reads and writes, register reads and the writes
// it makes, marked where the value differs from the front's register. Every 64th call of those
// macros is also counted that way, and a summary of how many writes change a register is logged
// per macro. Logging only: the real call runs as without it. For writing hand-made versions of
// these macros (EDEN_NX2_HLE_DIRECT).
namespace VideoCommon::Nx2MacroDump {

inline u32 CallsToLog() {
    static const u32 calls = [] {
        const char* value = std::getenv("EDEN_NX2_MACRO_DUMP");
        if (value == nullptr || *value == '\0') {
            return 0U;
        }
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        return parsed == 0 ? 0U : static_cast<u32>(std::min<unsigned long>(parsed, 256));
    }();
    return calls;
}

inline bool On() {
    return CallsToLog() != 0;
}

class Dumper {
public:
    using Word = u32;

    /// Before the real call of `start` with `parameters` runs (front thread).
    template <typename Registers>
    void Note(std::span<const Word> code, u64 generation, Word start,
              std::span<const Word> parameters, std::span<const Word, 0xc00> data_ram,
              const Registers& regs) {
        if (generation != code_generation) {
            code_generation = generation;
            for (auto& [_, entry] : entries) {
                entry.code_logged = false;
            }
        }
        Entry& entry = entries[start];
        ++entry.calls;
        if (entry.calls < HotCalls) {
            return;
        }
        if (!entry.code_logged) {
            if (hot_starts >= MaxStarts && !entry.hot) {
                return;
            }
            if (!entry.hot) {
                entry.hot = true;
                ++hot_starts;
            }
            entry.code_logged = true;
            LogCode(code, start);
        }
        const bool log_call =
            entry.logged < CallsToLog() && (entry.calls - HotCalls) % LogEvery == 0;
        if (!log_call && (entry.calls & 63) != 0) {
            return;
        }
        Run(code, start, parameters, data_ram, regs);
        entry.sampled += 1;
        entry.writes += run_writes.size();
        for (const auto& write : run_writes) {
            if (write.changed) {
                ++entry.changed;
                ++entry.changed_by_method[write.method];
            }
            ++entry.by_method[write.method];
        }
        entry.failed += run_error.empty() ? 0 : 1;
        if (log_call) {
            ++entry.logged;
            LogCall(start, entry, parameters);
        }
        if ((entry.sampled & 16383) == 0) {
            LogSummary(start, entry);
        }
    }

private:
    static constexpr u64 HotCalls = 20000;
    static constexpr u64 LogEvery = 40000;
    static constexpr u32 MaxStarts = 8;

    struct Write {
        Word method;
        Word value;
        Word before;
        bool changed;
    };

    struct Entry {
        u64 calls{};
        u64 sampled{};
        u64 writes{};
        u64 changed{};
        u64 failed{};
        u32 logged{};
        bool hot{};
        bool code_logged{};
        std::unordered_map<Word, u64> by_method;
        std::unordered_map<Word, u64> changed_by_method;
    };

    template <typename Registers>
    void Run(std::span<const Word> code, Word start, std::span<const Word> parameters,
             std::span<const Word, 0xc00> data_ram, const Registers& regs) {
        std::copy(data_ram.begin(), data_ram.end(), dram_copy.begin());
        interpreter.active_dram = &dram_copy;
        fifo.assign(parameters.begin(), parameters.end());
        run_writes.clear();
        register_reads.clear();
        written.clear();
        const auto value_of = [&](Word method) {
            const auto it = std::ranges::find(written, method, &std::pair<Word, Word>::first);
            if (it != written.end()) {
                return it->second;
            }
            return method < regs.size() ? regs[method] : 0U;
        };
        const auto read = [&](Word address) {
            const Word value = value_of(address);
            register_reads.emplace_back(address, value);
            return value;
        };
        const auto emit = [&](Word method, Word value) {
            if (method >= 0xe00) {
                return false;
            }
            const Word before = value_of(method);
            run_writes.push_back({method, value, before, before != value});
            const auto it = std::ranges::find(written, method, &std::pair<Word, Word>::first);
            if (it != written.end()) {
                it->second = value;
            } else {
                written.emplace_back(method, value);
            }
            // MME DMA reads land only in the real call: what follows may differ from it.
            return method != 0x157 && method != 0x158;
        };
        const auto result = interpreter.Execute(code, start, fifo, read, emit);
        run_error = result.success ? std::string{} : std::string{result.error};
        run_consumed = result.consumed;
    }

    static void LogCode(std::span<const Word> code, Word start) {
        // The instructions the macro can reach: fall-through, branch, jump and loop targets,
        // up to the one after an end (its delay slot).
        std::vector<bool> reached(code.size() / 3);
        std::vector<Word> pending{start};
        Word last = start;
        while (!pending.empty()) {
            const Word pc = pending.back();
            pending.pop_back();
            if (pc >= reached.size() || reached[pc]) {
                continue;
            }
            reached[pc] = true;
            last = std::max(last, pc);
            const std::array<Word, 3> bits{code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]};
            const auto d = Tegra::TuringMacroInterpreter::Decode(bits);
            bool cancels_end = false;
            for (Word lane = 0; lane < 2; ++lane) {
                const Word imm = d.imm[lane];
                switch (d.op[lane]) {
                case 23:
                    pending.push_back(pc + imm);
                    break;
                case 24:
                    pending.push_back(pc + (imm & 0x7fff));
                    break;
                case 25: case 26: case 27: case 28: case 29: {
                    const auto offset = static_cast<s32>(imm << 19) >> 19;
                    if (offset != -4096) {
                        pending.push_back(static_cast<Word>(static_cast<s32>(pc) + offset));
                        cancels_end |= (imm & 0x4000) != 0;
                    }
                    break;
                }
                default:
                    break;
                }
            }
            if (d.end && !cancels_end) {
                if (pc + 1 < reached.size()) {
                    reached[pc + 1] = true;
                    last = std::max(last, pc + 1);
                }
            } else {
                pending.push_back(pc + 1);
            }
        }
        std::string text;
        for (Word pc = start; pc <= last && pc < reached.size(); ++pc) {
            fmt::format_to(std::back_inserter(text), "{}{:x}:{:08x}{:08x}{:08x}",
                           text.empty() ? "" : " ", pc, code[pc * 3 + 2], code[pc * 3 + 1],
                           code[pc * 3]);
        }
        LOG_INFO(HW_GPU,
                 "Macro dump: code of {:#x} ({} instructions, high word first, "
                 "pc:bits95-64 bits63-32 bits31-0): {}",
                 start, last - start + 1, text);
    }

    void LogCall(Word start, const Entry& entry, std::span<const Word> parameters) const {
        std::string reads;
        for (const auto& [address, value] : interpreter.data_ram_reads) {
            fmt::format_to(std::back_inserter(reads), "{}{:x}={:x}", reads.empty() ? "" : ",",
                           address, value);
        }
        std::string dram_writes;
        for (const auto& [address, value] : interpreter.data_ram_writes) {
            fmt::format_to(std::back_inserter(dram_writes), "{}{:x}={:x}",
                           dram_writes.empty() ? "" : ",", address, value);
        }
        std::string reg_reads;
        for (const auto& [address, value] : register_reads) {
            fmt::format_to(std::back_inserter(reg_reads), "{}{:x}={:x}",
                           reg_reads.empty() ? "" : ",", address, value);
        }
        std::string params;
        for (const Word parameter : parameters) {
            fmt::format_to(std::back_inserter(params), "{}{:x}", params.empty() ? "" : ",",
                           parameter);
        }
        std::string writes;
        for (const auto& write : run_writes) {
            fmt::format_to(std::back_inserter(writes), "{}{:x}={:x}{}", writes.empty() ? "" : ",",
                           write.method, write.value, write.changed ? "*" : "");
        }
        LOG_INFO(HW_GPU,
                 "Macro dump: call {} of {:#x}: parameters {} ({}) used {}{} | data RAM read "
                 "[{}] written [{}] | registers read [{}] | writes ({}, * changes the register) "
                 "[{}]",
                 entry.calls, start, parameters.size(), params, run_consumed,
                 run_error.empty() ? std::string{} : " error " + run_error, reads, dram_writes, reg_reads,
                 run_writes.size(), writes);
    }

    static void LogSummary(Word start, const Entry& entry) {
        std::vector<std::pair<Word, u64>> methods(entry.by_method.begin(), entry.by_method.end());
        std::ranges::sort(methods, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text;
        for (size_t i = 0; i < methods.size() && i < 40; ++i) {
            const auto it = entry.changed_by_method.find(methods[i].first);
            const u64 changed = it != entry.changed_by_method.end() ? it->second : 0;
            fmt::format_to(std::back_inserter(text), "{}{:x} {}/{}", text.empty() ? "" : ", ",
                           methods[i].first, changed, methods[i].second);
        }
        const double sampled = static_cast<double>(entry.sampled);
        LOG_INFO(HW_GPU,
                 "Macro dump: {:#x} after {} calls, {} counted: writes per call {:.2f}, changing "
                 "the register {:.2f}, failed {} | by method (changed/all): {}",
                 start, entry.calls, entry.sampled, static_cast<double>(entry.writes) / sampled,
                 static_cast<double>(entry.changed) / sampled, entry.failed, text);
    }

    Tegra::TuringMacroInterpreter interpreter;
    Tegra::TuringMacroInterpreter::DataRam dram_copy{};
    std::vector<Word> fifo;
    std::vector<Write> run_writes;
    std::vector<std::pair<Word, Word>> register_reads;
    std::vector<std::pair<Word, Word>> written;
    std::string run_error;
    size_t run_consumed{};
    std::unordered_map<Word, Entry> entries;
    u64 code_generation{~0ULL};
    u32 hot_starts{};
};

/// The front thread's dumper (one front runs the macros).
inline Dumper& Get() {
    static Dumper dumper;
    return dumper;
}

} // namespace VideoCommon::Nx2MacroDump
