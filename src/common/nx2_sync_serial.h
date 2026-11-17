// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Common::Nx2SyncSerial {

/// EDEN_NX2_UBO_KEEP (BufferCache::BindHostGraphicsUniformBuffer): bumped wherever guest memory
/// may have changed without a buffer cache write notice reaching the memory stage: a command list
/// submission and every puller method (semaphores and syncpoints: the guest CPU may have written
/// memory before either, and expects the commands after it to see that), a write around the
/// caches (Tegra::Nx2MarkUntrackedWrite: semaphore and query results, copy backs, downloads, DMA)
/// and a device memory map or unmap. A uniform buffer read after the serial was S still holds
/// guest memory's bytes while the serial is S and no write notice touched its range: every write
/// a later draw may rely on is behind a bump.
inline std::atomic<u64> g_serial{1};

inline void Bump() {
    g_serial.fetch_add(1, std::memory_order_seq_cst);
}

[[nodiscard]] inline u64 Get() {
    return g_serial.load(std::memory_order_seq_cst);
}

} // namespace Common::Nx2SyncSerial
