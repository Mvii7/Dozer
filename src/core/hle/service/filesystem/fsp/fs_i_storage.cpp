// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "common/nx2_slow_frame.h"
#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    namespace Slow = Common::Nx2SlowFrame;
    Slow::Timer slow_timer{Slow::Counter::FsReadNs};
    Slow::Add(Slow::Counter::FsReads, 1);
    Slow::Add(Slow::Counter::FsReadBytes, static_cast<u64>(length));
    // EDEN_NX2_FS_READ_MBPS: the read rate held (40 MB/s unless set, =0 no limit).
    static const s64 bytes_per_second = [] {
        const char* value = std::getenv("EDEN_NX2_FS_READ_MBPS");
        if (value == nullptr || value[0] == '\0') {
            return ReadBytesPerSecond;
        }
        return static_cast<s64>(std::strtoll(value, nullptr, 10)) * 1'000'000;
    }();
    if (bytes_per_second > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_read_tick) {
            Slow::Timer throttle_timer{Slow::Counter::FsThrottleNs};
            std::this_thread::sleep_for(next_read_tick - now);
        }
        next_read_tick = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                                  1'000'000'000 / bytes_per_second};
    }

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
