// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <tuple>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include "common/fs/path_util.h"
#include "common/nx2_reactive.h"
#include "common/nx2_slow_frame.h"
#include "common/settings.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/core_timing.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"

namespace Core {

namespace {
// EDEN_NX2_SLOW_FRAME_MS: the JIT's compiles for the slow frame line. The switch is read at the
// first present, not here: the menu program sets it when the game starts.
const bool slow_frame_jit_hook = [] {
    Common::Nx2SlowFrame::g_take_jit = [] {
        auto& stats = Dynarmic::A64::compile_stats;
        stats.track.store(true, std::memory_order_relaxed);
        return Common::Nx2SlowFrame::JitCounts{
            stats.blocks.exchange(0, std::memory_order_relaxed),
            stats.ns.exchange(0, std::memory_order_relaxed),
            stats.clears.exchange(0, std::memory_order_relaxed),
            stats.again.exchange(0, std::memory_order_relaxed),
            stats.other_jit.exchange(0, std::memory_order_relaxed),
            stats.ranges.exchange(0, std::memory_order_relaxed)};
    };
    return true;
}();

// EDEN_NX2_JIT_BLOCK_LIST=1: the blocks the JIT compiles are saved per title
// (<cache>/nx2_jit/<title id>.bin, every 30 s and when emulation stops), as offsets from the
// process's code address (the loader moves it every boot). When the first core starts, every
// core's JIT compiles the saved blocks in executable memory, in parallel, and each core waits for
// its own before it runs, so code reached in an earlier session is not compiled in the frame that
// first runs it, once by each core.
namespace JitBlockList {

constexpr u64 Magic = 0x32304C424A32584EULL; // "NX2JBL02"
constexpr size_t MaxBlocks = 4'000'000;
constexpr u64 PcMask = (u64{1} << 56) - 1;
constexpr u64 Invalid = ~u64{0};

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_BLOCK_LIST");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// Tests: EDEN_NX2_JIT_BLOCK_LIST_THREADS=0 has each core compile its list on its own thread when
/// it first runs; EDEN_NX2_JIT_BLOCK_LIST_MAX=<n> compiles only the first n saved blocks, after
/// the first EDEN_NX2_JIT_BLOCK_LIST_SKIP=<n>.
bool Threads() {
    static const bool threads = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_BLOCK_LIST_THREADS");
        return value == nullptr || value[0] != '0';
    }();
    return threads;
}

size_t SkipFirst() {
    static const size_t skip = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_BLOCK_LIST_SKIP");
        return value == nullptr ? size_t{0} : static_cast<size_t>(std::strtoull(value, nullptr, 10));
    }();
    return skip;
}

size_t MaxCompiled() {
    static const size_t max = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_BLOCK_LIST_MAX");
        return value == nullptr || value[0] == '\0' ? MaxBlocks
                                                    : static_cast<size_t>(std::strtoull(value, nullptr, 10));
    }();
    return max;
}

struct State {
    std::mutex mutex;
    const Kernel::KProcess* process = nullptr;
    u64 base = 0; ///< The process's code address
    std::filesystem::path path;
    std::unordered_set<u64> known; ///< Saved and compiled blocks, relative
    std::vector<u64> saved_list;   ///< Relative
    size_t saved_count = 0;
    bool ready_done = false;
    std::vector<u64> ready; ///< The saved blocks in executable memory, absolute
    // Diagnostic: the first 64 bytes of each ready block's code when it was compiled, checked
    // against memory once, 30 s after the list was loaded.
    Core::Memory::Memory* memory = nullptr;
    std::vector<std::pair<u64, u32>> ready_code; ///< (hash, first instruction)
    bool code_checked = false;
    // Diagnostics: the saved blocks compiled at boot and those skipped (not executable then),
    // relative, and the game's compiles of each kind and of new blocks.
    std::unordered_set<u64> ready_set;
    std::unordered_set<u64> skipped;
    u64 compiled_ready = 0;
    u64 compiled_skipped = 0;
    u64 compiled_new = 0;
    bool started = false;
    std::vector<std::jthread> workers;
    std::jthread saver; // last: stopped (and the list saved) before the rest goes
};

State& Get() {
    static State state;
    return state;
}

thread_local bool precompiling = false;

u64 ToRelative(u64 descriptor, u64 base) {
    const u64 pc = descriptor & PcMask;
    return pc < base ? Invalid : (descriptor & ~PcMask) | (pc - base);
}

u64 ToAbsolute(u64 relative, u64 base) {
    return (relative & ~PcMask) | (((relative & PcMask) + base) & PcMask);
}

void Record(u64 descriptor) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    const u64 relative = ToRelative(descriptor, state.base);
    if (relative == Invalid) {
        return;
    }
    if (!precompiling) {
        if (state.ready_set.contains(relative)) {
            ++state.compiled_ready;
        } else if (state.skipped.contains(relative)) {
            ++state.compiled_skipped;
        } else if (!state.known.contains(relative)) {
            ++state.compiled_new;
        }
    }
    if (state.known.size() < MaxBlocks) {
        state.known.insert(relative);
    }
}

std::pair<u64, u32> CodeAt(Core::Memory::Memory& memory, u64 pc) {
    std::array<u32, 16> words{};
    memory.ReadBlockUnsafe(pc, words.data(), sizeof(words));
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u32 word : words) {
        hash = (hash ^ word) * 0x100000001b3ULL;
    }
    return {hash, words[0]};
}

/// Logs the ready blocks whose code changed in memory since they were compiled.
void CheckCode() {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    if (state.code_checked || state.memory == nullptr || state.ready_code.size() != state.ready.size()) {
        return;
    }
    state.code_checked = true;
    size_t changed = 0;
    for (size_t i = 0; i < state.ready.size(); ++i) {
        const u64 pc = state.ready[i] & PcMask;
        const auto now = CodeAt(*state.memory, pc);
        if (now.first == state.ready_code[i].first) {
            continue;
        }
        if (++changed <= 40) {
            LOG_INFO(Core_ARM,
                     "nx2 jit block list: code changed since boot at {:#x} (code+{:#x}): first "
                     "instruction {:08x} -> {:08x}",
                     pc, pc - state.base, state.ready_code[i].second, now.second);
        }
    }
    LOG_INFO(Core_ARM, "nx2 jit block list: {} of {} blocks compiled at boot have changed code",
             changed, state.ready.size());
}

void LogGameCompiles() {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    LOG_INFO(Core_ARM,
             "nx2 jit block list: compiled in game since boot: {} new, {} saved but skipped at "
             "boot, {} compiled at boot already (by any core)",
             state.compiled_new, state.compiled_skipped, state.compiled_ready);
}

void Save() {
    auto& state = Get();
    std::vector<u64> list;
    std::filesystem::path path;
    {
        std::scoped_lock lock{state.mutex};
        // Test runs (EDEN_NX2_JIT_BLOCK_LIST_SKIP, _MAX) keep the saved list as it was.
        if (state.process == nullptr || state.known.size() == state.saved_count ||
            SkipFirst() != 0 || MaxCompiled() != MaxBlocks) {
            return;
        }
        list.assign(state.known.begin(), state.known.end());
        state.saved_count = list.size();
        path = state.path;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        const u64 header[2]{Magic, list.size()};
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(reinterpret_cast<const char*>(list.data()),
                   static_cast<std::streamsize>(list.size() * sizeof(u64)));
        if (!file) {
            return;
        }
    }
    std::filesystem::rename(temporary, path, error);
}

/// Loads the list of the process's title once per process and starts recording; true when
/// enabled.
bool Load(const Kernel::KProcess& process) {
    if (!Enabled()) {
        return false;
    }
    auto& state = Get();
    {
        std::scoped_lock lock{state.mutex};
        if (state.process == &process) {
            return true;
        }
    }
    Save(); // a previous game's
    auto& workers = state.workers;
    workers.clear(); // joins them
    std::scoped_lock lock{state.mutex};
    state.process = &process;
    state.base = GetInteger(process.GetEntryPoint());
    state.path = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "nx2_jit" /
                 fmt::format("{:016X}.bin", process.GetProgramId());
    state.known.clear();
    state.saved_list.clear();
    state.ready.clear();
    state.ready_set.clear();
    state.skipped.clear();
    state.ready_done = false;
    state.started = false;
    state.compiled_ready = state.compiled_skipped = state.compiled_new = 0;
    std::ifstream file{state.path, std::ios::binary};
    u64 header[2]{};
    if (file.read(reinterpret_cast<char*>(header), sizeof(header)) && header[0] == Magic &&
        header[1] <= MaxBlocks) {
        state.saved_list.resize(header[1]);
        if (!file.read(reinterpret_cast<char*>(state.saved_list.data()),
                       static_cast<std::streamsize>(header[1] * sizeof(u64)))) {
            state.saved_list.clear();
        }
    }
    state.known.insert(state.saved_list.begin(), state.saved_list.end());
    state.saved_count = state.known.size();
    LOG_INFO(Core_ARM, "nx2 jit block list: {} blocks saved for {:016X}, code at {:#x} ({})",
             state.saved_list.size(), process.GetProgramId(), state.base,
             Common::FS::PathToUTF8String(state.path));
    Dynarmic::A64::on_block_compiled = Record;
    if (!state.saver.joinable()) {
        state.saver = std::jthread([](std::stop_token stop) {
            std::mutex wait_mutex;
            std::condition_variable_any wake;
            while (!stop.stop_requested()) {
                std::unique_lock wait_lock{wait_mutex};
                wake.wait_for(wait_lock, stop, std::chrono::seconds{30}, [] { return false; });
                if (!stop.stop_requested()) {
                    LogGameCompiles();
                    CheckCode();
                }
                Save();
            }
        });
    }
    return true;
}

/// The saved blocks whose code is in executable memory of the process, absolute.
const std::vector<u64>& Ready(Kernel::KProcess& process) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    if (state.ready_done) {
        return state.ready;
    }
    state.ready_done = true;
    state.memory = &process.GetMemory();
    state.ready_code.clear();
    state.code_checked = false;
    std::vector<u64> list;
    list.reserve(state.saved_list.size());
    for (size_t i = SkipFirst(); i < state.saved_list.size() && list.size() < MaxCompiled(); ++i) {
        list.push_back(ToAbsolute(state.saved_list[i], state.base));
    }
    std::sort(list.begin(), list.end(),
              [](u64 a, u64 b) { return (a & PcMask) < (b & PcMask); });
    u64 region_start = 0;
    u64 region_end = 0;
    bool executable = false;
    // Skipped blocks by memory region: (start, end, state, permission) -> blocks.
    std::map<std::tuple<u64, u64, u32, u32>, size_t> skipped_regions;
    std::tuple<u64, u64, u32, u32> region_key{};
    for (const u64 descriptor : list) {
        const u64 pc = descriptor & PcMask;
        if (pc < region_start || pc >= region_end) {
            Kernel::KMemoryInfo info{};
            Kernel::Svc::PageInfo page_info{};
            if (process.GetPageTable().QueryInfo(&info, &page_info, pc) != ResultSuccess) {
                region_start = 0;
                region_end = 0;
                executable = false;
                region_key = {pc, pc, ~0U, ~0U};
            } else {
                region_start = info.GetAddress();
                region_end = info.GetEndAddress();
                executable = True(info.GetPermission() & Kernel::KMemoryPermission::UserExecute);
                region_key = {region_start, region_end, static_cast<u32>(info.GetState()),
                              static_cast<u32>(info.GetPermission())};
            }
        }
        if (executable && pc + sizeof(u32) <= region_end) {
            state.ready.push_back(descriptor);
            state.ready_code.push_back(CodeAt(process.GetMemory(), pc));
            state.ready_set.insert(ToRelative(descriptor, state.base));
        } else {
            state.skipped.insert(ToRelative(descriptor, state.base));
            ++skipped_regions[region_key];
        }
    }
    std::vector<std::pair<size_t, std::tuple<u64, u64, u32, u32>>> by_count;
    for (const auto& [key, count] : skipped_regions) {
        by_count.emplace_back(count, key);
    }
    std::sort(by_count.begin(), by_count.end(), std::greater{});
    LOG_INFO(Core_ARM,
             "nx2 jit block list: {} saved blocks skipped (not executable now) in {} regions",
             state.skipped.size(), by_count.size());
    for (size_t i = 0; i < by_count.size() && i < 12; ++i) {
        const auto& [count, key] = by_count[i];
        const auto& [start, end, memory_state, permission] = key;
        LOG_INFO(Core_ARM,
                 "nx2 jit block list: skipped {} in [{:#x}, {:#x}) state {:#x} permission {:#x}",
                 count, start, end, memory_state, permission);
    }
    return state.ready;
}

} // namespace JitBlockList
} // namespace

using namespace Common::Literals;

DynarmicCallbacks64::DynarmicCallbacks64(ArmDynarmic64& parent, Kernel::KProcess* process)
    : m_parent{parent}, m_memory(process->GetMemory())
    , m_process(process), m_debugger_enabled{parent.m_system.DebuggerEnabled()}
    , m_check_memory_access{m_debugger_enabled || !Settings::values.cpuopt_ignore_memory_aborts.GetValue()}
{}

u64 DynarmicCallbacks64::MemoryRead(u64 vaddr, size_t size) {
    CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Read);
    u64 value{};
    switch (size) {
    case sizeof(u64): value = m_memory.Read64(vaddr); break;
    case sizeof(u32): value = m_memory.Read32(vaddr); break;
    case sizeof(u16): value = m_memory.Read16(vaddr); break;
    case sizeof(u8): value = m_memory.Read8(vaddr); break;
    default: UNREACHABLE();
    }
    // With memory aborts ignored the guest keeps running on zeros: say where it read from, once
    // for each of the first reading instructions, with the code around it to disassemble.
    if (auto& unmapped = Core::Memory::UnmappedReadFlag(); unmapped != 0) {
        unmapped = 0;
        const u64 pc = m_parent.m_jit->GetPC();
        static std::mutex seen_mutex;
        static std::array<u64, 32> seen{};
        static size_t seen_count = 0;
        bool first = false;
        {
            std::scoped_lock lock{seen_mutex};
            if (seen_count < seen.size() &&
                std::find(seen.begin(), seen.begin() + seen_count, pc) == seen.begin() + seen_count) {
                seen[seen_count++] = pc;
                first = true;
            }
        }
        if (first) {
            constexpr u64 Before = 16, After = 4;
            std::string code;
            const u64 start = pc - Before * 4;
            if (m_memory.IsValidVirtualAddressRange(start, (Before + After + 1) * 4)) {
                for (u64 i = 0; i <= Before + After; ++i) {
                    const bool at_pc = i == Before;
                    code += fmt::format(" {}{:08x}{}", at_pc ? "[" : "",
                                        m_memory.Read32(start + i * 4), at_pc ? "]" : "");
                }
            }
            LOG_ERROR(Core_ARM,
                      "Unmapped read {:#x} ({} bytes) at pc {:#x} lr {:#x} sp {:#x}; code from "
                      "{:#x}:{}",
                      vaddr, size, pc, m_parent.m_jit->GetRegister(30), m_parent.m_jit->GetSP(),
                      start, code);
        }
    }
    return value;
}
Dynarmic::A64::Vector DynarmicCallbacks64::MemoryRead128(u64 vaddr) {
    CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Read);
    return {m_memory.Read64(vaddr), m_memory.Read64(vaddr + 8)};
}

std::optional<u32> DynarmicCallbacks64::MemoryReadCode(u64 vaddr) {
    if (!m_memory.IsValidVirtualAddressRange(vaddr, sizeof(u32)))
        return std::nullopt;
    auto const aligned_vaddr = vaddr & ~Core::Memory::YUZU_PAGEMASK;
    if (last_code_addr != aligned_vaddr) {
        m_memory.ReadBlock(aligned_vaddr, &cached_code_page, sizeof(cached_code_page));
        last_code_addr = aligned_vaddr;
    }
    return cached_code_page.inst[(vaddr & Core::Memory::YUZU_PAGEMASK) / sizeof(u32)];
}

void DynarmicCallbacks64::MemoryWrite(Dynarmic::A64::VAddr vaddr, u64 value, std::size_t size) {
    if (CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Write)) {
        switch (size) {
        case sizeof(u64): return m_memory.Write64(vaddr, u64(value));
        case sizeof(u32): return m_memory.Write32(vaddr, u32(value));
        case sizeof(u16): return m_memory.Write16(vaddr, u16(value));
        case sizeof(u8): return m_memory.Write8(vaddr, u8(value));
        default: UNREACHABLE();
        }
    }
}
void DynarmicCallbacks64::MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value) {
    if (CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write)) {
        m_memory.Write64(vaddr, value[0]);
        m_memory.Write64(vaddr + 8, value[1]);
    }
}

bool DynarmicCallbacks64::MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) {
    return CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive8(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) {
    return CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive16(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) {
    return CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive32(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) {
    return CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive64(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive128(u64 vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) {
    return CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive128(vaddr, value, expected);
}

void DynarmicCallbacks64::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) {
    last_code_addr = u64(-1); //invalidate cached page
    switch (op) {
    case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU: {
        static constexpr u64 ICACHE_LINE_SIZE = 64;
        const u64 cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
        m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
        break;
    }
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
        m_parent.ClearInstructionCache();
        break;
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
    default:
        LOG_DEBUG(Core_ARM, "Unprocesseed instruction cache operation: {}", op);
        break;
    }
    m_parent.m_jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
}

void DynarmicCallbacks64::ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) {
    switch (exception) {
    case Dynarmic::A64::Exception::WaitForInterrupt:
    case Dynarmic::A64::Exception::WaitForEvent:
    case Dynarmic::A64::Exception::SendEvent:
    case Dynarmic::A64::Exception::SendEventLocal:
    case Dynarmic::A64::Exception::Yield:
        LOG_TRACE(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X}, cached = {:08X})", std::size_t(exception), pc, m_memory.Read32(pc), MemoryReadCode(pc).value_or(0));
        return;
    case Dynarmic::A64::Exception::NoExecuteFault:
        LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", pc);
        ReturnException(pc, PrefetchAbort);
        return;
    default:
        if (m_debugger_enabled) {
            ReturnException(pc, InstructionBreakpoint);
        } else {
            m_parent.LogBacktrace(m_process);
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})", static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
        }
    }
}

void DynarmicCallbacks64::CallSVC(u32 svc) {
    m_parent.m_svc = svc;
    m_parent.m_jit->HaltExecution(SupervisorCall);
}

void DynarmicCallbacks64::AddTicks(u64 ticks) {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
    // rough approximation of the amount of executed ticks in the system, it may be thrown off
    // if not all cores are doing a similar amount of work. Instead of doing this, we should
    // device a way so that timing is consistent across all cores without increasing the ticks 4
    // times.
    u64 amortized_ticks = ticks / Core::Hardware::NUM_CPU_CORES;
    // Always execute at least one tick.
    amortized_ticks = std::max<u64>(amortized_ticks, 1);
    m_parent.m_system.CoreTiming().AddTicks(amortized_ticks);
}

u64 DynarmicCallbacks64::GetTicksRemaining() {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    return std::max<s64>(m_parent.m_system.CoreTiming().downcount, 0);
}

u64 DynarmicCallbacks64::GetCNTPCT() {
    const u64 ticks = m_parent.m_system.CoreTiming().GetClockTicks();
    // Guest code that waits by polling the counter (title 1's main thread reads it ~17 million
    // times a second while waiting for the GPU) keeps a host core busy at full speed, and the
    // GPU thread is often on the other half of that SMT core. Two reads within ~1.7 us of each
    // other are such a loop: pause, which hands the core's resources to the sibling thread for a
    // moment. The guest sees the same counter. EDEN_NX2_CNTPCT_PAUSE=0 turns it off.
    static const bool pause_spins = [] {
        const char* value = std::getenv("EDEN_NX2_CNTPCT_PAUSE");
        return value == nullptr || value[0] != '0';
    }();
    if (pause_spins && ticks - m_last_cntpct < m_spin_ticks) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
    m_last_cntpct = ticks;
    return ticks;
}

bool DynarmicCallbacks64::CheckMemoryAccess(u64 addr, u64 size, Kernel::DebugWatchpointType type) {
    if (!m_check_memory_access) {
        return true;
    }

    if (!m_memory.IsValidVirtualAddressRange(addr, size)) {
        LOG_CRITICAL(Core_ARM, "Stopping execution due to unmapped memory access at {:#x}",
                        addr);
        m_parent.m_jit->HaltExecution(PrefetchAbort);
        return false;
    }

    if (!m_debugger_enabled) {
        return true;
    }

    const auto match{m_parent.MatchingWatchpoint(addr, size, type)};
    if (match) {
        m_parent.m_halted_watchpoint = match;
        m_parent.m_jit->HaltExecution(DataAbort);
        return false;
    }

    return true;
}

void DynarmicCallbacks64::ReturnException(u64 pc, Dynarmic::HaltReason hr) {
    m_parent.GetContext(m_parent.m_breakpoint_context);
    m_parent.m_breakpoint_context.pc = pc;
    m_parent.m_jit->HaltExecution(hr);
}

void ArmDynarmic64::MakeJit(Common::PageTable* page_table, std::size_t address_space_bits) {
    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = std::addressof(*m_cb);

    // Memory
    if (page_table) {
        // Dynarmic will not write to the page table, const_cast is safe here
        config.page_table = reinterpret_cast<void**>(
            const_cast<Common::PageTable::PageEntryData*>(page_table->entries.data()));
        config.page_table_address_space_bits = std::uint32_t(address_space_bits);
        config.page_table_pointer_mask = Common::PageTable::ATTRIBUTE_MASK;
        config.page_table_marked_bit = uint8_t(0);
        config.silently_mirror_page_table = false;
        config.absolute_offset_page_table = true;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;

        config.fastmem_pointer = page_table->fastmem_arena ?
            std::optional<uintptr_t>{reinterpret_cast<uintptr_t>(page_table->fastmem_arena)} :
            std::nullopt;
        config.fastmem_address_space_bits = std::uint32_t(address_space_bits);
        config.silently_mirror_fastmem = false;

        config.fastmem_exclusive_access = config.fastmem_pointer != std::nullopt;
        config.recompile_on_exclusive_fastmem_failure = true;

        if (reinterpret_cast<u64>(m_system.DeviceMemory().buffer.BackingBasePointer() +
            Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize()) < (1ULL << 39)) {
            // Systems like FreeBSD allocate memory really low by default, and since we pack our page table entries,
            // we have to manually sign extend when our actual pointer is negative.
            config.page_table_sign_extension = std::uint8_t(Common::PageTable::SIGN_BIT);
        }
    }

    // Multi-process state
    config.processor_id = std::uint8_t(m_core_index);
    config.global_monitor = &m_exclusive_monitor.monitor;

    // System registers
    config.tpidrro_el0 = &m_cb->m_tpidrro_el0;
    config.tpidr_el0 = &m_cb->m_tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    // 31.25 MHz for Switch 2 titles (Common::WallClock::SetGuestCNTFRQ, set at boot).
    config.cntfrq_el0 = static_cast<u32>(Common::g_wall_clock.GetGuestCNTFRQ());
    m_cb->m_spin_ticks = config.cntfrq_el0 / 600'000;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Timing
    config.wall_clock_cntpct = m_uses_wall_clock;
    config.enable_cycle_counting = !m_uses_wall_clock;

    // Code cache size
#if defined(ARCHITECTURE_arm64) || defined(__sun__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
    config.code_cache_size = std::uint32_t(128_MiB);
#else
    config.code_cache_size = std::uint32_t(512_MiB);
#endif

    // Allow memory fault handling to work
    if (m_system.DebuggerEnabled()) {
        config.check_halt_on_memory_access = true;
    }

    // null_jit
    if (!page_table) {
        // Don't waste too much memory on null_jit
        config.code_cache_size = std::uint32_t(8_MiB);
    }

    switch (Settings::values.cpu_accuracy.GetValue()) {
    // Debug mode
    case Settings::CpuAccuracy::Debugging:
        if (!Settings::values.cpuopt_page_tables) {
            config.page_table = nullptr;
        }
        if (!Settings::values.cpuopt_block_linking) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!Settings::values.cpuopt_return_stack_buffer) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        if (!Settings::values.cpuopt_fast_dispatcher) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        if (!Settings::values.cpuopt_context_elimination) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
        }
        if (!Settings::values.cpuopt_const_prop) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
        }
        if (!Settings::values.cpuopt_misc_ir) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
        }
        if (!Settings::values.cpuopt_reduce_misalign_checks) {
            config.only_detect_misalignment_via_page_table_on_page_boundary = false;
        }
        if (!Settings::values.cpuopt_fastmem) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_fastmem_exclusives) {
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_recompile_exclusives) {
            config.recompile_on_exclusive_fastmem_failure = false;
        }
        if (!Settings::values.cpuopt_ignore_memory_aborts) {
            config.check_halt_on_memory_access = true;
        }
        break;
    // Unsafe optimizations
    case Settings::CpuAccuracy::Unsafe:
        config.unsafe_optimizations = true;
        if (!Settings::values.cpuopt_unsafe_host_mmu) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (Settings::values.cpuopt_unsafe_unfuse_fma) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        }
        if (Settings::values.cpuopt_unsafe_reduce_fp_error) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
        }
        if (Settings::values.cpuopt_unsafe_inaccurate_nan) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
        if (Settings::values.cpuopt_unsafe_fastmem_check) {
            config.fastmem_address_space_bits = 64;
        }
        if (Settings::values.cpuopt_unsafe_ignore_global_monitor) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        break;
    // Safe optimisations
    case Settings::CpuAccuracy::Auto:
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        config.fastmem_address_space_bits = 64;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        break;
    // Paranoia mode for debugging optimizations
    case Settings::CpuAccuracy::Paranoid:
        config.unsafe_optimizations = false;
        config.optimizations = Dynarmic::no_optimizations;
        break;
    case Settings::CpuAccuracy::Accurate:
    default:
        break;
    }
    if (!Settings::IsFastmemEnabled()) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }
    // Read traps (Memory::RasterizerArmReadTraps) protect a page only until its next read, so
    // that read should not take the instruction off fastmem for good.
    const char* read_traps = Common::Nx2Reactive::GetEnv("EDEN_NX2_READ_TRAPS");
    config.fastmem_read_faults_transient =
        config.fastmem_pointer.has_value() && (read_traps == nullptr || read_traps[0] != '0') &&
        !Settings::values.use_reactive_flushing.GetValue() &&
        Common::Nx2Reactive::TrapFaultsKeepFastmem();
#if defined(ARCHITECTURE_x86_64)
    // x86-64 host code generation options; Paranoid keeps dynarmic's defaults.
    if (Settings::values.cpu_accuracy.GetValue() != Settings::CpuAccuracy::Paranoid) {
        // The JIT spilled ~9% of its instructions to stack slots in Ocarina of Time's hot code
        // while callee-saved GPRs sat empty: evictions go to a free callee-saved GPR first, and an
        // operand that is overwritten but still needed is copied to a free register instead.
        config.spill_to_free_gpr = true;
        config.scratch_copy_to_free_reg = true;
        // W-register writes and zero-extends of values a 32-bit instruction produced skip the
        // second `mov r32, r32`.
        config.elide_redundant_zext = true;
        // ldr/str [xN, #imm]: the immediate goes into the fastmem addressing mode instead of a
        // separate LEA (and often a register copy).
        config.fold_address_offsets = true;
        // The per-block %rbp frame is three instructions per block that nothing reads.
        config.omit_block_frame = true;
        // Fast dispatch table of 2^16 entries, 4-way set associative: 1 MiB per core instead of
        // the 16 MiB direct-mapped 2^20 table.
        config.fast_dispatch_table_bits = 16;
        config.fast_dispatch_set_associative = true;
        // A 32-entry return stack buffer: returns from deeper call chains skip the dispatcher.
        config.rsb_size = 32;
        // Unlinked exits jump to one shared stub per target: 5/6-byte patch sites instead of
        // 22/23 bytes plus an inline exit stub.
        config.compact_block_exits = true;
        // Fault/abort paths, atomic fallbacks and exit stubs go to a cold region at the end of
        // the code cache, so hot blocks sit densely.
        config.far_code = true;
        // Branches, CSEL and CBZ/TBZ read the host flags left by the instruction before them
        // instead of reloading NZCV from the guest state.
        config.reuse_host_flags = !config.enable_cycle_counting;
        // Halt checks on links to lower PCs and on indirect branches instead of every linked
        // branch (multicore only: dynarmic counts no cycles there).
        config.halt_check_on_back_edges = !config.enable_cycle_counting;
        // BL/BLR as host calls and RET as a host return checked against the guest return
        // address: the CPU's return predictor predicts guest returns instead of one shared
        // indirect jump through the return stack buffer (multicore only, no cycle counter).
        config.host_call_return = !config.enable_cycle_counting;
    }
#endif
    m_jit.emplace(config);
}

HaltReason ArmDynarmic64::RunThread(Kernel::KThread* thread) {
    if (!m_block_list_done) [[unlikely]] {
        m_block_list_done = true;
        StartBlockList();
    }
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Run());
}

HaltReason ArmDynarmic64::StepThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

u32 ArmDynarmic64::GetSvcNumber() const {
    return m_svc;
}

void ArmDynarmic64::GetSvcArguments(std::span<uint64_t, 8> args) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        args[i] = j.GetRegister(i);
}

void ArmDynarmic64::SetSvcArguments(std::span<const uint64_t, 8> args) {
    Dynarmic::A64::Jit& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        j.SetRegister(i, args[i]);
}

const Kernel::DebugWatchpoint* ArmDynarmic64::HaltedWatchpoint() const {
    return m_halted_watchpoint;
}

void ArmDynarmic64::RewindBreakpointInstruction() {
    this->SetContext(m_breakpoint_context);
}

ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process, DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor}
    , m_cb(std::make_optional<DynarmicCallbacks64>(*this, process))
    , m_core_index{core_index}
{
    auto& page_table = process->GetPageTable().GetBasePageTable();
    auto& page_table_impl = page_table.GetImpl();
    MakeJit(&page_table_impl, page_table.GetAddressSpaceWidth());
}

ArmDynarmic64::~ArmDynarmic64() {
    for (u32 state = m_block_list_state.load(); state == 1; state = m_block_list_state.load()) {
        m_block_list_state.wait(state);
    }
    if (m_block_list_done && JitBlockList::Enabled()) {
        {
            auto& state = JitBlockList::Get();
            std::scoped_lock lock{state.mutex};
            state.memory = nullptr;
        }
        JitBlockList::Save();
    }
}

void ArmDynarmic64::StartBlockList() {
    Kernel::KProcess* const process = m_cb->m_process;
    if (process == nullptr || !process->IsApplication() || !JitBlockList::Load(*process)) {
        return;
    }
    auto& state = JitBlockList::Get();
    bool start = false;
    {
        std::scoped_lock lock{state.mutex};
        start = !state.started;
        state.started = true;
    }
    if (start) {
        // Every core's JIT compiles the list on a thread of its own; a core not created yet
        // compiles it when it first runs. (No RTTI: an AArch64 interface is this class unless
        // NCE may run the process.)
#ifndef HAS_NCE
        const auto& ready = JitBlockList::Ready(*process);
        for (size_t core = 0; JitBlockList::Threads() && core < Hardware::NUM_CPU_CORES; ++core) {
            ArmInterface* const core_interface = process->GetArmInterface(core);
            if (core_interface == nullptr ||
                core_interface->GetArchitecture() != Architecture::AArch64) {
                continue;
            }
            auto* const arm = static_cast<ArmDynarmic64*>(core_interface);
            if (!arm->ClaimBlockList()) {
                continue;
            }
            state.workers.emplace_back([arm, &ready] {
                JitBlockList::precompiling = true;
                arm->CompileBlockList(ready);
            });
        }
#endif
    }
    if (ClaimBlockList()) {
        JitBlockList::precompiling = true;
        CompileBlockList(JitBlockList::Ready(*process));
        JitBlockList::precompiling = false;
    }
    for (u32 list_state = m_block_list_state.load(); list_state == 1;
         list_state = m_block_list_state.load()) {
        m_block_list_state.wait(list_state);
    }
}

bool ArmDynarmic64::ClaimBlockList() {
    u32 expected = 0;
    return m_block_list_state.compare_exchange_strong(expected, 1);
}

void ArmDynarmic64::CompileBlockList(const std::vector<u64>& ready) {
    const auto start = std::chrono::steady_clock::now();
    const size_t compiled = m_jit->Precompile(ready.data(), ready.size());
    LOG_INFO(Core_ARM, "nx2 jit block list: core {} compiled {} of {} saved blocks in {} ms",
             m_core_index, compiled, ready.size(),
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());
    m_block_list_state.store(2);
    m_block_list_state.notify_all();
}

void ArmDynarmic64::SetTpidrroEl0(u64 value) {
    m_cb->m_tpidrro_el0 = value;
}

void ArmDynarmic64::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    auto gpr = j.GetRegisters();
    auto fpr = j.GetVectors();
    // TODO: this is inconvenient
    for (size_t i = 0; i < 29; i++)
        ctx.r[i] = gpr[i];
    ctx.fp = gpr[29];
    ctx.lr = gpr[30];
    ctx.sp = j.GetSP();
    ctx.pc = j.GetPC();
    ctx.pstate = j.GetPstate();
    ctx.v = fpr;
    ctx.fpcr = j.GetFpcr();
    ctx.fpsr = j.GetFpsr();
    ctx.tpidr = m_cb->m_tpidr_el0;
}

void ArmDynarmic64::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    Dynarmic::A64::Jit& j = *m_jit;
    // TODO: this is inconvenient
    std::array<u64, 31> gpr;
    for (size_t i = 0; i < 29; i++)
        gpr[i] = ctx.r[i];
    gpr[29] = ctx.fp;
    gpr[30] = ctx.lr;
    j.SetRegisters(gpr);
    j.SetSP(ctx.sp);
    j.SetPC(ctx.pc);
    j.SetPstate(ctx.pstate);
    j.SetVectors(ctx.v);
    j.SetFpcr(ctx.fpcr);
    j.SetFpsr(ctx.fpsr);
    m_cb->m_tpidr_el0 = ctx.tpidr;
}

void ArmDynarmic64::SignalInterrupt(Kernel::KThread* thread) {
    m_jit->HaltExecution(BreakLoop);
}

void ArmDynarmic64::ClearInstructionCache() {
    m_cb->last_code_addr = u64(-1);
    m_jit->ClearCache();
}

void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {
    m_jit->InvalidateCacheRange(addr, size);
}

} // namespace Core
