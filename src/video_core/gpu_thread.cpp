// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>

#include "common/assert.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_busy_clock.h"
#include "video_core/nx2_front_diag_hooks.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_sync_diag.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GPUThread {

namespace {

// Tells a CPU thread waiting in PushCommand that its command ran.
void SignalCommand(SynchState& state, u64 fence, bool block) {
    state.signaled_fence.store(fence);
    if (block) {
        // We have to lock the write_lock to ensure that the condition_variable wait not get a
        // race between the check and the lock itself.
        std::scoped_lock lk{state.write_lock};
        state.cv.notify_all();
    }
}

} // Anonymous namespace

ThreadManager::ThreadManager(Core::System& system_)
    : system{system_}
{}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPU");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        system.RegisterHostThread();

        auto current_context = context.Acquire();
        CommandDataContainer next;
        // When a pending channel last moved or a command arrived (see the waits below).
        auto last_progress = std::chrono::steady_clock::now();
        // For this long after that the thread waits in hardware, then it blocks in the OS. In
        // hardware it looks at the words the channels wait on every WordCheckPeriod and polls the
        // channels in full every PendingPollPeriod.
        constexpr std::chrono::nanoseconds PendingPoll = std::chrono::microseconds{500};
        constexpr std::chrono::nanoseconds PendingPollPeriod = std::chrono::microseconds{20};
        constexpr std::chrono::nanoseconds WordCheckPeriod = std::chrono::microseconds{2};
        const bool pending_wake = Tegra::Nx2PendingWake::Enabled();
        auto& wake = Tegra::Nx2PendingWake::Get();
        auto& waiters = Tegra::Nx2SemaphoreWaiters::Get();
        namespace Profile = VideoCommon::Nx2Profile;
        auto& split = VideoCommon::GpuSplit::Get();
        const bool busy_clock = VideoCommon::Nx2BusyClock::Tracked();
        const bool sync_diag = VideoCommon::Nx2SyncDiag::On();
        // A GPU mapping change went to the back (RunCommand::front_waits): before the front parses
        // another command list, which may live in or read from the mapped memory, the back runs it.
        // Once for a burst of maps.
        bool map_pending = false;
        const auto wait_for_maps = [&] {
            if (!map_pending) {
                return;
            }
            map_pending = false;
            if (split.Active()) {
                VideoCommon::Nx2FrontDiag::CountDrain(
                    split, VideoCommon::Nx2Profile::DrainSite::MappingChange);
            }
        };
        while (!stop_token.stop_requested()) {
            Profile::Report();
            if (VideoCommon::Nx2FrontDiag::On()) [[unlikely]] {
                VideoCommon::Nx2FrontDiag::Report();
            }
            if (sync_diag) {
                VideoCommon::Nx2SyncDiag::GetFront().Report();
            }
            split.MaybeStart(system);
            if (scheduler.HasPendingChannels() && pending_wake) {
                // Suspended channels wait on semaphores: a new command or a GPU release wakes the
                // wait below at once (Nx2PendingWake), so arm it before looking.
                wake.Arm();
                bool resumed = false;
                {
                    Profile::Scope scope{Profile::Zone::ResumePending};
                    wait_for_maps();
                    resumed = scheduler.ResumePending(system.GPU());
                }
                if (sync_diag) {
                    VideoCommon::Nx2SyncDiag::GetFront().NoteResumePass(resumed);
                }
                const bool popped = state.queue.TryPop(next);
                if (resumed || popped) {
                    last_progress = std::chrono::steady_clock::now();
                }
                if (!popped) {
                    if (!scheduler.HasPendingChannels()) {
                        continue;
                    }
                    // The CPU usually releases the semaphore soon after, with a store that sends
                    // no wake: for half a millisecond wait in hardware, watching the semaphore
                    // words, then block in the OS for 500 us at a time. A full poll every 20 us
                    // catches what sends no wake and changes no word.
                    Profile::Scope scope{Profile::Zone::PendingSleep};
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_progress < PendingPoll) {
                        const auto poll_end =
                            (std::min)(now + PendingPollPeriod, last_progress + PendingPoll);
                        auto until = now;
                        do {
                            until = (std::min)(std::chrono::steady_clock::now() + WordCheckPeriod,
                                               poll_end);
                            if (wake.Poll(until) || waiters.WordStored()) {
                                break;
                            }
                        } while (until < poll_end);
                    } else if (sync_diag) {
                        const s64 start = VideoCommon::Nx2SyncDiag::NowNs();
                        const bool notified = wake.Sleep(std::chrono::microseconds{500});
                        VideoCommon::Nx2SyncDiag::GetFront().NoteSleep(
                            static_cast<u64>(VideoCommon::Nx2SyncDiag::NowNs() - start), notified);
                    } else {
                        wake.Sleep(std::chrono::microseconds{500});
                    }
                    continue;
                }
            } else if (scheduler.HasPendingChannels()) {
                // EDEN_NX2_PENDING_WAKE=0: suspended channels wait on semaphores the CPU
                // releases: poll them meanwhile.
                {
                    Profile::Scope scope{Profile::Zone::ResumePending};
                    wait_for_maps();
                    scheduler.ResumePending(system.GPU());
                }
                if (!state.queue.TryPop(next)) {
                    // The CPU usually releases the semaphore soon after: poll for half a
                    // millisecond before sleeping, or every wait also waits out a host sleep
                    // (title 1 waits ~25 times a second).
                    Profile::Scope scope{Profile::Zone::PendingSleep};
                    if (std::chrono::steady_clock::now() - last_progress <
                        std::chrono::microseconds{500}) {
                        std::this_thread::yield();
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    continue;
                }
            } else {
                Profile::Scope scope{Profile::Zone::Idle};
                if (busy_clock) {
                    // EDEN_NX2_GPU_BUSY_CLOCK: idle from here if the back caught up too.
                    VideoCommon::Nx2BusyClock::FrontWaits(true);
                    state.queue.PopWait(next, stop_token);
                    VideoCommon::Nx2BusyClock::FrontWaits(false);
                } else {
                    state.queue.PopWait(next, stop_token);
                }
            }
            if (stop_token.stop_requested()) {
                break;
            }
            last_progress = std::chrono::steady_clock::now();
            if (split.Active()) {
                // Only command lists are parsed here; everything else runs on the back thread
                // in order, and so does the signal that the command ran.
                auto* const rasterizer_ptr = renderer.ReadRasterizer();
                namespace Diag = VideoCommon::Nx2FrontDiag;
                const bool diag = Diag::Tracking();
                if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                    Profile::Scope scope{Profile::Zone::Push};
                    // Held-back guest CPU writes first (see the single-thread path below).
                    split.Defer([gpu = &system.GPU()] { gpu->InvalidateGPUCache(); });
                    wait_for_maps();
                    scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
                    // EDEN_NX2_PROBE_FRONT_US: a slower front, for how much its time costs.
                    if (const u32 probe_us = Diag::ProbeFrontUs(); probe_us != 0) [[unlikely]] {
                        Diag::SpinFor(probe_us);
                    }
                } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                    split.Defer([gpu = &system.GPU()] { gpu->TickWork(); });
                    if (diag) [[unlikely]] {
                        Diag::NoteWork(Diag::WorkKind::Tick);
                    }
                } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                    // Unknown work: on A with the back stages parked (nx2_stages.h).
                    map_pending |= run->front_waits;
                    split.DeferFunctionTo<VideoCommon::DeferTarget::Join>(std::move(run->func));
                    if (diag) [[unlikely]] {
                        Diag::NoteWork(Diag::WorkKind::Run);
                    }
                } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = flush->addr, size = flush->size] {
                        rasterizer_ptr->FlushRegion(addr, size);
                    });
                    if (diag) [[unlikely]] {
                        Diag::NoteDeviceWrite(Diag::WriteKind::Download, flush->addr, flush->size);
                    }
                } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = invalidate->addr, size = invalidate->size] {
                        rasterizer_ptr->OnCacheInvalidation(addr, size);
                    });
                } else {
                    ASSERT(false);
                }
                split.Defer([state_ptr = &state, fence = next.fence, block = next.block] {
                    SignalCommand(*state_ptr, fence, block);
                });
                split.Publish();
                continue;
            }
            if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Push};
                // Guest CPU writes to GPU-written memory are held back until the GPU thread
                // applies them, by default at the next fence. The guest made them before it
                // submitted this list, which may read them: apply them first. Pokemon Pokopia
                // keeps constants in pages its native storage bindings mark GPU-written, and some
                // frames drew stale ones (flickering shadows, stretched geometry).
                system.GPU().InvalidateGPUCache();
                scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
            } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                Profile::Scope scope{Profile::Zone::Tick};
                system.GPU().TickWork();
            } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Run};
                run->func();
            } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
            } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr, invalidate->size);
            } else {
                ASSERT(false);
            }
            SignalCommand(state, next.fence, next.block);
        }
    });
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    PushCommand(GPUTickCommand(), false, is_async);
}

void ThreadManager::Run(std::function<void()>&& func, bool is_async, bool front_waits) {
    PushCommand(RunCommand{std::move(func), front_waits}, false, is_async);
}

void ThreadManager::StartSplit() {
    const auto& split = VideoCommon::GpuSplit::Get();
    if (split.Active() || !split.CanStart()) {
        return;
    }
    // On the GPU thread, in the order of its queue; run by the back thread instead when the split
    // started meanwhile.
    PushCommand(RunCommand{[this] {
                    auto& gpu_split = VideoCommon::GpuSplit::Get();
                    if (!gpu_split.Active()) {
                        gpu_split.MaybeStart(system);
                    }
                }},
                true, true);
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    if (Settings::IsGPULevelHigh()) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            gpu.WaitForSyncOperation(fence);
        }
    }
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
    state.queue.EmplaceWait(std::move(command_data), fence, block);
    Tegra::Nx2PendingWake::Get().Notify();

    if (block) {
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

void ThreadManager::NotifyShutdown() {
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
    VideoCommon::GpuSplit::Get().Stop();
}

} // namespace VideoCommon::GPUThread
