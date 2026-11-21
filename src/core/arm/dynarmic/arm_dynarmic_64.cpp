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
#include <deque>
#include <filesystem>
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
// EDEN_NX2_JIT_BLOCK_LIST (on; =0 off): the blocks the JIT compiles are saved per title
// (<cache>/nx2_jit/<title id>.bin, every 30 s and when emulation stops), as offsets from the
// process's code address (the loader moves it every boot). After the game is loaded and before it
// runs, every core's JIT compiles the saved blocks in executable memory, in parallel, so code
// reached in an earlier session is not compiled in the frame that first runs it, once by each core.
namespace JitBlockList {

constexpr u64 Magic = 0x33304C424A32584EULL; // "NX2JBL03"
constexpr size_t MaxBlocks = 4'000'000;
constexpr u64 PcMask = (u64{1} << 56) - 1;
constexpr u64 Invalid = ~u64{0};

using BuildId = std::array<u8, 0x20>;

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_BLOCK_LIST");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// EDEN_NX2_JIT_IDLE_COMPILE (on with the block list; =0 off): a block one core compiles is
// compiled by the other cores while they idle, instead of in the frame each one first runs it.
bool IdleCompileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_IDLE_COMPILE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

constexpr size_t MaxPending = 1 << 18; ///< Per core

struct State {
    std::mutex mutex;
    const Kernel::KProcess* process = nullptr; ///< Recorded, until it ends
    u64 base = 0;                              ///< Its code address
    BuildId build_id{};                        ///< Its main module's; the list is for that build
    std::filesystem::path path;
    std::unordered_set<u64> known; ///< Saved and compiled blocks, relative
    std::vector<u64> order;        ///< The same, oldest first
    size_t first_kept = 0;         ///< order's first saved block (older ones did not fit)
    size_t saved_count = 0;        ///< order.size() at the last save
    /// The process's JIT on each core, while it lives. Its core_mutex is held to use it.
    std::array<ArmDynarmic64*, Hardware::NUM_CPU_CORES> cores{};
    std::array<std::mutex, Hardware::NUM_CPU_CORES> core_mutex;
    /// Absolute descriptors of new blocks other cores compiled, for this core's idle time
    std::array<std::deque<u64>, Hardware::NUM_CPU_CORES> pending;
    std::array<std::atomic<size_t>, Hardware::NUM_CPU_CORES> pending_size{};
    std::atomic<u64> idle_compiled = 0;
    std::atomic<u64> pending_dropped = 0;
    std::jthread saver; // last: stopped (and the list saved) before the rest goes
};

State& Get() {
    static State state;
    return state;
}

u64 ToRelative(u64 descriptor, u64 base) {
    const u64 pc = descriptor & PcMask;
    return pc < base ? Invalid : (descriptor & ~PcMask) | (pc - base);
}

u64 ToAbsolute(u64 relative, u64 base) {
    return (relative & ~PcMask) | (((relative & PcMask) + base) & PcMask);
}

void Record(const Kernel::KProcess* process, size_t core, u64 descriptor) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    if (process != state.process) {
        return;
    }
    const u64 relative = ToRelative(descriptor, state.base);
    if (relative == Invalid || state.known.size() >= MaxBlocks ||
        !state.known.insert(relative).second) {
        return;
    }
    state.order.push_back(relative);
    if (!IdleCompileEnabled()) {
        return;
    }
    for (size_t other = 0; other < state.cores.size(); ++other) {
        if (other == core || state.cores[other] == nullptr) {
            continue;
        }
        if (state.pending[other].size() >= MaxPending) {
            ++state.pending_dropped;
            continue;
        }
        state.pending[other].push_back(descriptor);
        state.pending_size[other].store(state.pending[other].size(), std::memory_order_relaxed);
    }
}

void ClearPending(State& state, size_t core) {
    state.pending[core].clear();
    state.pending_size[core].store(0, std::memory_order_relaxed);
}

/// The process's JIT on this core (nullptr: none). Takes the core's mutex, then the state's.
void SetCore(size_t core, ArmDynarmic64* arm, const ArmDynarmic64* replaced) {
    auto& state = Get();
    std::scoped_lock lock{state.core_mutex[core], state.mutex};
    if (replaced == nullptr || state.cores[core] == replaced) {
        state.cores[core] = arm;
        ClearPending(state, core);
    }
}

/// Writes the blocks from first_kept on; the header holds the build they are for.
void Save() {
    auto& state = Get();
    std::vector<u64> list;
    std::filesystem::path path;
    BuildId build_id{};
    size_t count = 0;
    {
        std::scoped_lock lock{state.mutex};
        count = state.order.size();
        if (state.process == nullptr || count == state.saved_count) {
            return;
        }
        list.assign(state.order.begin() + static_cast<std::ptrdiff_t>(state.first_kept),
                    state.order.end());
        path = state.path;
        build_id = state.build_id;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += ".tmp";
    bool written = false;
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        const u64 header[2]{Magic, list.size()};
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(reinterpret_cast<const char*>(build_id.data()), build_id.size());
        file.write(reinterpret_cast<const char*>(list.data()),
                   static_cast<std::streamsize>(list.size() * sizeof(u64)));
        written = static_cast<bool>(file);
    }
    if (written) {
        std::filesystem::rename(temporary, path, error);
        written = !error;
    }
    if (!written) {
        std::filesystem::remove(temporary, error);
        return;
    }
    std::scoped_lock lock{state.mutex};
    if (state.path == path) {
        state.saved_count = (std::max)(state.saved_count, count);
    }
}

/// Loads the process's list (dropped when it is for another build of the game) and records the
/// blocks its JITs compile from now on.
void Load(const Kernel::KProcess& process, const BuildId& build_id) {
    auto& state = Get();
    Save(); // a previous game's
    std::scoped_lock lock{state.mutex};
    state.process = &process;
    state.base = GetInteger(process.GetEntryPoint());
    state.build_id = build_id;
    state.path = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "nx2_jit" /
                 fmt::format("{:016X}.bin", process.GetProgramId());
    state.known.clear();
    state.order.clear();
    state.first_kept = 0;
    std::ifstream file{state.path, std::ios::binary};
    u64 header[2]{};
    BuildId saved_build{};
    if (file.read(reinterpret_cast<char*>(header), sizeof(header)) && header[0] == Magic &&
        header[1] <= MaxBlocks &&
        file.read(reinterpret_cast<char*>(saved_build.data()), saved_build.size()) &&
        saved_build == build_id) {
        state.order.resize(header[1]);
        if (!file.read(reinterpret_cast<char*>(state.order.data()),
                       static_cast<std::streamsize>(header[1] * sizeof(u64)))) {
            state.order.clear();
        }
    }
    state.known.insert(state.order.begin(), state.order.end());
    state.saved_count = state.order.size();
    if (!state.saver.joinable()) {
        state.saver = std::jthread([](std::stop_token stop) {
            std::mutex wait_mutex;
            std::condition_variable_any wake;
            while (!stop.stop_requested()) {
                std::unique_lock wait_lock{wait_mutex};
                wake.wait_for(wait_lock, stop, std::chrono::seconds{30}, [] { return false; });
                Save();
                const u64 compiled = Get().idle_compiled.exchange(0);
                const u64 dropped = Get().pending_dropped.exchange(0);
                if (compiled != 0 || dropped != 0) {
                    LOG_INFO(Core_ARM,
                             "nx2 jit idle compile: {} blocks compiled by idle cores in 30 s, {} "
                             "not queued (queue full)",
                             compiled, dropped);
                }
            }
        });
    }
}

/// Saves the list and stops recording when the process ends.
void End(const Kernel::KProcess* process) {
    auto& state = Get();
    {
        std::scoped_lock lock{state.mutex};
        if (process == nullptr || process != state.process) {
            return;
        }
    }
    Save();
    std::scoped_lock lock{state.mutex};
    if (state.process == process) {
        state.process = nullptr;
    }
}

struct ReadyBlocks {
    std::vector<u64> descriptors; ///< Absolute, newest first
    std::vector<size_t> order_at; ///< Each one's index in State::order
};

/// The saved blocks whose code is in executable memory of the process.
ReadyBlocks Ready(Kernel::KProcess& process) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    // By address for the memory queries; the result in the list's order.
    std::vector<std::pair<u64, size_t>> by_pc;
    by_pc.reserve(state.order.size());
    for (size_t i = 0; i < state.order.size(); ++i) {
        by_pc.emplace_back(ToAbsolute(state.order[i], state.base) & PcMask, i);
    }
    std::sort(by_pc.begin(), by_pc.end());
    std::vector<bool> executable_at(state.order.size());
    u64 region_start = 0;
    u64 region_end = 0;
    bool executable = false;
    for (const auto& [pc, index] : by_pc) {
        if (pc < region_start || pc >= region_end) {
            Kernel::KMemoryInfo info{};
            Kernel::Svc::PageInfo page_info{};
            if (process.GetPageTable().QueryInfo(&info, &page_info, pc) != ResultSuccess) {
                region_start = region_end = 0;
                executable = false;
            } else {
                region_start = info.GetAddress();
                region_end = info.GetEndAddress();
                executable = True(info.GetPermission() & Kernel::KMemoryPermission::UserExecute);
            }
        }
        executable_at[index] = executable && pc + sizeof(u32) <= region_end;
    }
    ReadyBlocks ready;
    ready.descriptors.reserve(state.order.size());
    ready.order_at.reserve(state.order.size());
    for (size_t i = state.order.size(); i-- > 0;) {
        if (executable_at[i]) {
            ready.descriptors.push_back(ToAbsolute(state.order[i], state.base));
            ready.order_at.push_back(i);
        }
    }
    return ready;
}

/// The cores compiled only the newest `fitted` ready blocks before their code caches were too
/// full: the older ones are no longer saved.
void Fit(const ReadyBlocks& ready, size_t fitted) {
    if (fitted >= ready.descriptors.size()) {
        return;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    state.first_kept = ready.order_at[fitted] + 1;
    state.saved_count = 0;
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

void DynarmicCallbacks64::BlockCompiled(u64 descriptor) {
    if (JitBlockList::Enabled()) {
        JitBlockList::Record(m_process, m_parent.m_core_index, descriptor);
    }
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
    if (JitBlockList::Enabled()) {
        if (m_core_index < Hardware::NUM_CPU_CORES) {
            JitBlockList::SetCore(m_core_index, nullptr, this);
        }
        JitBlockList::End(m_cb->m_process);
    }
}

void ArmDynarmic64::CompileForOtherCores(std::size_t core, const std::atomic<bool>& interrupted) {
    if (!JitBlockList::Enabled() || !JitBlockList::IdleCompileEnabled() ||
        core >= Hardware::NUM_CPU_CORES) {
        return;
    }
    auto& state = JitBlockList::Get();
    while (state.pending_size[core].load(std::memory_order_relaxed) != 0 && !interrupted) {
        std::scoped_lock core_lock{state.core_mutex[core]};
        ArmDynarmic64* arm = nullptr;
        u64 descriptor = 0;
        {
            std::scoped_lock lock{state.mutex};
            arm = state.cores[core];
            if (arm == nullptr || state.pending[core].empty()) {
                JitBlockList::ClearPending(state, core);
                return;
            }
            descriptor = state.pending[core].front();
            state.pending[core].pop_front();
            state.pending_size[core].store(state.pending[core].size(), std::memory_order_relaxed);
        }
        // This core's thread is idle, so its JIT is not running. When the code cache is too full
        // the rest is left to the run.
        if (!arm->m_jit->PrecompileOne(descriptor)) {
            std::scoped_lock lock{state.mutex};
            JitBlockList::ClearPending(state, core);
            return;
        }
        ++state.idle_compiled;
    }
}

void ArmDynarmic64::PrecompileBlockList(System& system, Kernel::KProcess& process) {
#ifdef HAS_NCE
    if (Settings::IsNceEnabled()) {
        return;
    }
#endif
    if (!JitBlockList::Enabled() || !process.IsApplication() || !process.Is64Bit()) {
        return;
    }
    JitBlockList::Load(process, system.GetApplicationProcessBuildID());
    const auto start = std::chrono::steady_clock::now();
    const auto ready = JitBlockList::Ready(process);
    std::atomic<size_t> fitted{ready.descriptors.size()};
    {
        // Before the process runs. (No RTTI: a 64-bit process's interfaces are this class unless
        // NCE runs it.)
        std::vector<std::jthread> workers;
        for (size_t core = 0; core < Hardware::NUM_CPU_CORES; ++core) {
            ArmInterface* const core_interface = process.GetArmInterface(core);
            if (core_interface != nullptr &&
                core_interface->GetArchitecture() == Architecture::AArch64) {
                auto* const arm = static_cast<ArmDynarmic64*>(core_interface);
                JitBlockList::SetCore(core, arm, nullptr);
                workers.emplace_back([arm, &ready, &fitted] {
                    const size_t done = arm->m_jit->Precompile(ready.descriptors.data(),
                                                               ready.descriptors.size());
                    for (size_t seen = fitted.load(); done < seen &&
                                                      !fitted.compare_exchange_weak(seen, done);) {
                    }
                });
            }
        }
    }
    JitBlockList::Fit(ready, fitted.load());
    LOG_INFO(Core_ARM, "nx2 jit block list: {} of {} saved blocks compiled on every core in {} ms",
             fitted.load(), ready.descriptors.size(),
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());
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
