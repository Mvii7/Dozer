// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <vector>

#include "core/hle/kernel/k_thread.h"
#include "core/loader/loader.h"

namespace Core {

std::optional<std::string> GetThreadName(const Kernel::KThread* thread);
std::string_view GetThreadWaitReason(const Kernel::KThread* thread);
std::string GetThreadState(const Kernel::KThread* thread);

Loader::AppLoader::Modules FindModules(Kernel::KProcess* process);
Kernel::KProcessAddress GetModuleEnd(const Kernel::KProcess* process, Kernel::KProcessAddress base);
Kernel::KProcessAddress FindMainModuleEntrypoint(Kernel::KProcess* process);

void InvalidateInstructionCacheRange(const Kernel::KProcess* process, u64 address, u64 size);

// The sampler names guest code. One ArmDynarmic64 per core keeps its process here.
void RetainGuestProcess(Kernel::KProcess* process);
void ReleaseGuestProcess(Kernel::KProcess* process);

// "main+0x1c0 symbol" for a guest PC, or empty when no loaded module holds it. The PC is a
// block's first instruction.
std::string NameGuestPc(u64 guest_pc);

// Host code [host, host_end) compiled for guest_pc.
struct JitGuestBlock {
    u64 host;
    u64 host_end;
    u64 guest_pc;
};
void CopyJitGuestBlocks(std::vector<JitGuestBlock>& out);

struct BacktraceEntry {
    std::string module;
    u64 address;
    u64 original_address;
    u64 offset;
    std::string name;
};

std::vector<BacktraceEntry> GetBacktraceFromContext(Kernel::KProcess* process,
                                                    const Kernel::Svc::ThreadContext& ctx);
std::vector<BacktraceEntry> GetBacktrace(const Kernel::KThread* thread);

} // namespace Core
