// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#if defined(ARCHITECTURE_x86_64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#include "common/common_types.h"

// Where the thread that prepares draws (the GPU back thread with the split, the GPU thread
// without it) spends its time, and what it waits for (docs/back-thread-split-plan.md, phase 0).
// EDEN_BACK_THREAD_STATS=1 logs it every 4 s; =2 also serializes the stamps (lfence before rdtsc),
// to check the buckets a decision rests on. Off, every probe is one load and a branch.
//
// Ownership: DrawThread is touched only by the thread that prepares draws (one at a time: the
// split hands it over with a join or a drain). Everything other threads write is in Shared, as
// relaxed atomics: each value is atomic by itself and a report a chunk late does not matter.
namespace VideoCommon::BackStats {

/// 0 off, 1 on, 2 on with serialized stamps.
extern const int mode;

[[nodiscard]] inline bool Enabled() {
    return mode != 0;
}

/// A time stamp in ticks (the TSC on x86-64, steady_clock elsewhere); the report calibrates them.
/// Unserialized rdtsc may move by tens of cycles: meaningful for buckets summed over thousands of
/// draws, not for a single short span.
[[nodiscard]] inline u64 Stamp() {
#if defined(ARCHITECTURE_x86_64)
    if (mode == 2) {
        _mm_lfence();
    }
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

/// Consecutive spans of RasterizerVulkan::PrepareDraw, each attributed by Mark to one bucket.
enum Draw : u32 {
    FlushWork,     ///< FlushWork (dispatching to the worker, flushing every 4096 draws)
    FlushCaching,  ///< gpu_memory->FlushCaching (accumulated invalidations)
    Pipeline,      ///< pipeline_cache.CurrentGraphicsPipeline
    LockWait,      ///< Acquiring buffer_cache.mutex + texture_cache.mutex
    Setup,         ///< ConfigureImpl: VPRS, SynchronizeDescriptors, uniform buffer state
    StorageBind,   ///< ConfigureImpl: binding storage buffers
    Handles,       ///< ConfigureImpl: texture handles, compact tables
    Resolve,       ///< ConfigureImpl: view and sampler resolution, banks
    BindStage,     ///< ConfigureImpl: texture and image buffers
    Buffers,       ///< UpdateGraphicsBuffers, BindHostGeometryBuffers, descriptor queue Acquire
    StageBuffers,  ///< BindHostStageBuffers
    PushImages,    ///< PushImageDescriptors
    PostConfigure, ///< PostCopyBarrier, UpdateRenderTargets, CheckFeedbackLoop
    DescCompare,   ///< ConfigureDraw: comparing the payload with the last one
    DescAllocate,  ///< DescriptorBufferRing::Allocate
    DescWrite,     ///< WriteDescriptorBuffer
    DescAssign,    ///< Keeping the payload (last_descriptor_payload.assign)
    DescRecord,    ///< Rest of ConfigureDraw: render pass, pipeline and state updates, Record
    DynamicStates, ///< UpdateDynamicStates
    Queries,       ///< Query segment, transform feedback, counters
    DrawFunc,      ///< Recording the draw
    FlushStorage,  ///< FlushStorageWrites
    Tail,          ///< Early exits, failed configures, up to releasing the locks
    Unlock,        ///< Releasing the cache locks (wakes a waiting guest thread, if any)
    TickWork,      ///< gpu.TickWork at the end
    DrawCount,
};

/// The draw-preparing thread's own counters (see the ownership note above).
struct DrawThread {
    u64 last{};
    std::array<u64, DrawCount> draw{};
    u64 draws{};
    u64 prepare{};   ///< Whole PrepareDraw calls
    u64 lock_hold{}; ///< From acquiring the cache locks to releasing them
    u64 back_busy{}; ///< GpuSplit::BackLoop executing entries
    u64 back_idle{}; ///< ... spinning or sleeping
    bool back_loop{}; ///< BackLoop ran in this window
    u64 back_idle_episodes{}; ///< Times BackLoop found the ring empty
    u64 back_slept{};         ///< ... and went to sleep after spinning
    u64 back_wake{};          ///< From the front's wake-up call to the back running again
    u64 back_wake_count{};
    // Descriptors of graphics draws.
    std::array<u64, 4> paths{}; ///< Descriptor buffer, push descriptor, descriptor set, none
    u64 buffer_reused{};
    u64 set_reused{};           ///< Descriptor set draws that kept the last set (same payload)
    u64 set_rebound{};          ///< ... that wrote a new set because the pipeline was bound again
    u64 set_rebound_same{};     ///< ... whose entries equal the pipeline's last set anyway
    u64 buffer_allocated{};
    u64 buffer_entries{};       ///< Entries written into new allocations (one vkGetDescriptorEXT each)
    u64 copied_bytes{};         ///< Cached compact-table entries copied (AddEntries)
    u64 compared_bytes{};
    u64 assigned_bytes{};
    u64 ring_exhausted{};       ///< Descriptor buffer frames that ran out (a Finish each)
    u64 ring_frame_wait{};      ///< Waiting for a reused descriptor buffer frame
    u32 report_check{};
    // Kept across reports.
    u64 draw_serial{};          ///< Draws prepared so far
    // GpuSplit::BackLoop by ring entry: method, macro write, multi-word method, call, other.
    std::array<u64, 5> ring_ticks{};
    std::array<u64, 5> ring_count{};
    u64 sink_ticks{};           ///< Engine::ConsumeSink (inside the method entries)
    u64 last_compute_serial{};  ///< draw_serial at the last compute dispatch
    // Handle tables a compute shader wrote (buffer-backed texture arrays, downloaded per frame).
    u64 table_downloads{};
    u64 table_download_ticks{}; ///< Inside buffer_cache.DownloadMemory (a Finish when GPU-written)
    u64 table_draws_since_compute{}; ///< Draws recorded since the last dispatch, at each download
    u64 table_same{};           ///< The downloaded bytes equal the last download at that address
    u64 table_changed{};
    // CompactTextureTable::Build: elements whose (TIC, TSC, excluded) changed since the last build.
    u64 builds{};
    u64 build_elements{};
    u64 build_changed{};
    u64 build_resized{};        ///< Builds with another element count (not compared)
};
extern DrawThread draw_thread;

enum DescriptorPath : u32 {
    PathBuffer,
    PathPush,
    PathSet,
    PathNone,
};

/// A scheduler wait (Finish, WaitWorker, Wait for a tick).
struct WaitCounters {
    std::atomic<u64> count{};
    std::atomic<u64> ticks{};
    std::atomic<u64> ticks_in_draw_lock{}; ///< While the draw thread held the cache locks
    std::atomic<u64> queue_sum{};          ///< Chunks queued for the worker when the wait began
    std::atomic<u64> queue_max{};
    std::atomic<u64> retained{};           ///< The producer's chunk still held commands
};

struct LockCounters {
    std::atomic<u64> count{};
    std::atomic<u64> ticks{};
};

/// A batch of ranges applied to the caches: how many ranges, how many remain once overlapping
/// and adjacent ones are joined (an exact union, which every cache would treat alike), and how
/// many distinct 4 KiB pages they touch.
struct InvalidationCounters {
    std::atomic<u64> batches{};
    std::atomic<u64> ranges{};
    std::atomic<u64> merged{};
    std::atomic<u64> pages{};
};

/// Written by other threads (worker, guest CPU threads, the split's front).
struct Shared {
    std::atomic<u64> frame{}; ///< TickFrame count of the draw thread
    std::atomic<u64> composite{}; ///< RendererVulkan::Composite (may run on a syncpoint action)
    std::atomic<u64> clear{};
    std::atomic<u64> compute{};
    std::atomic<u64> dma{};
    std::atomic<u64> inline_upload{};
    // Vulkan worker.
    std::atomic<u64> worker_busy{};
    std::atomic<u64> worker_idle{};
    std::atomic<u64> worker_chunks{};
    std::atomic<u64> worker_age_max{}; ///< Frames since the oldest command of a chunk was recorded
    std::atomic<u64> submits{};
    std::atomic<u64> submit_latency{}; ///< From recording a submission to running it
    std::atomic<u64> submit_latency_max{};
    std::atomic<u64> dispatches{};
    std::atomic<u64> dispatch_queue_sum{};
    std::atomic<u64> dispatch_queue_max{};
    WaitCounters finish;
    WaitCounters wait_worker;
    WaitCounters wait_tick;       ///< On the draw thread
    WaitCounters wait_tick_other; ///< On other threads (fence release, present, ...)
    std::array<LockCounters, 9> drain_sites{}; ///< GpuSplit::Drain by caller (DrainSite)
    // Native semaphore acquires after their drain: ready at once, ready after the front released
    // fences (it waited for the GPU), or not ready (the channel is suspended).
    std::atomic<u64> acquire_ready{};
    std::atomic<u64> acquire_ready_after_fences{};
    std::atomic<u64> acquire_suspended{};
    std::atomic<u64> acquire_fence_ticks{}; ///< The front in ReleaseFences(true) for an acquire
    // The semaphore word read from guest memory before the drain (statistics only).
    std::atomic<u64> acquire_ready_before{}; ///< Already satisfied before draining
    std::atomic<u64> acquire_changed{};      ///< The drain changed the word
    std::array<std::atomic<u64>, 3> acquire_ops{}; ///< Equal, greater or equal, mask
    /// Acquires whose word the drain changed, by the source of the last release the back executed
    /// at that address (ReleaseSource).
    std::array<std::atomic<u64>, 6> acquire_release_source{};
    // Cache locks taken outside draws: by other threads, and by the draw thread itself.
    LockCounters cpu_write;
    LockCounters cached_writes;
    LockCounters other_locks;
    LockCounters draw_thread_locks;
    // The split's front.
    std::atomic<u64> drains{};
    std::atomic<u64> front_slept{};      ///< Drains in which the front went to sleep after spinning
    std::atomic<u64> front_wake{};       ///< From the back's wake-up call to the front running again
    std::atomic<u64> front_wake_count{};
    std::atomic<u64> drain_to_publish{}; ///< From a drain's end to the front's next publish
    std::atomic<u64> drain_to_publish_count{};
    std::atomic<u64> front_guest_wait{};   ///< The GPU thread (front) waiting for guest submissions
    std::atomic<u64> front_guest_waits{};
    std::atomic<u64> front_pending_sleep{}; ///< ... sleeping 1 ms with a native channel suspended
    // Guest CPU writes to GPU-cached pages (core/memory.cpp HandleRasterizerWrite) and what the
    // buffer cache answered (BufferCache::OnCPUWrite).
    std::atomic<u64> cpu_write_unregistered{}; ///< No buffer there (the texture cache is told)
    std::atomic<u64> cpu_write_held{};         ///< GPU-modified: held until the next submission
    std::atomic<u64> cpu_write_written{};      ///< Marked CPU-modified (the texture cache is told)
    std::atomic<u64> cpu_write_would_hit{};    ///< Held, on one of the core's last 16 held pages
    std::atomic<u64> cpu_write_skipped{};      ///< Held without asking (EDEN_CPU_WRITE_PAGES)
    // BufferCache::IsRegionGpuModified (under the buffer cache lock, any thread).
    // Calls the front queues around command lists (any thread runs them without the split).
    std::atomic<u64> list_start{};   ///< GPU::InvalidateGPUCache: applying held guest writes
    std::atomic<u64> list_end{};     ///< FlushCommandsBatched + OnCommandListEnd
    // FenceManager::SignalFenceCoalesced (native QMD release adds): run now or at a fence.
    std::atomic<u64> coalesced_now{};
    std::atomic<u64> coalesced_delayed{};
    std::atomic<u64> fences_signaled{};
    // ReadNativeSemaphoreWord: words read back from a GPU-modified buffer (FlushRegion).
    std::atomic<u64> semaphore_readbacks{};
    std::atomic<u64> semaphore_readback_ticks{};
    InvalidationCounters held_writes;   ///< RasterizerVulkan::OnCachedWrites (guest writes)
    InvalidationCounters flush_caching; ///< RasterizerVulkan::InnerInvalidation (engine writes)
    std::atomic<u64> gpu_modified_queries{};
    std::atomic<u64> gpu_modified_true{};
    std::atomic<u64> gpu_modified_one_page{}; ///< Queries within one 4 KiB page
    std::atomic<u64> drain_ticks{};
    std::atomic<u64> space_ticks{};
};
extern Shared shared;

/// Callers of GpuSplit::Drain (the front waits for the back and the back then runs dry).
enum DrainSite : u32 {
    DrainOther,
    DrainBind,             ///< Puller::ProcessBindMethod
    DrainNativeAcquire,    ///< Puller::CanResumeNativeAcquire
    DrainSemaphoreAcquire, ///< Puller semaphore acquire (ProcessSemaphoreTriggerMethod)
    DrainAcquire,          ///< Puller::ProcessSemaphoreAcquire
    DrainMacroMemory,      ///< A front macro reads memory (FIFO DMA)
    DrainFront3D,          ///< Maxwell3D takes its macros to the front
    DrainFrontCompute,     ///< KeplerCompute takes its macros to the front
    DrainSignalSync,       ///< DmaPusher: a command list asks for a sync
};

/// Who released a semaphore (statistics only).
enum ReleaseSource : u32 {
    ReleaseUnknown,  ///< No release seen at the address (or forgotten by a collision)
    ReleasePuller,   ///< Puller semaphore release or WriteLong trigger
    ReleaseDma,      ///< MaxwellDMA semaphore
    ReleaseQmd,      ///< Native compute QMD release (written when the back runs it)
    ReleaseQmdAdd,   ///< Native compute QMD release that adds at a fence
    Release3D,       ///< Maxwell3D report semaphore
};

/// The last release the back executed at each address (draw thread, which runs the engines; the
/// front reads it only after a drain, while the back is idle).
struct ReleaseNote {
    u64 address{};
    ReleaseSource source{};
};
extern std::array<ReleaseNote, 1024> release_notes;

inline void NoteRelease(u64 gpu_address, ReleaseSource source) {
    if (Enabled()) {
        release_notes[(gpu_address >> 2) & (release_notes.size() - 1)] = {gpu_address, source};
    }
}

[[nodiscard]] inline ReleaseSource LastRelease(u64 gpu_address) {
    const ReleaseNote& note = release_notes[(gpu_address >> 2) & (release_notes.size() - 1)];
    return note.address == gpu_address ? note.source : ReleaseUnknown;
}

/// Whether this thread prepares draws (set by PrepareDraw).
inline thread_local bool is_draw_thread{};
/// Whether this thread holds the cache locks for a draw.
inline thread_local bool in_draw_lock{};

inline void Add(std::atomic<u64>& counter, u64 value) {
    counter.fetch_add(value, std::memory_order_relaxed);
}

inline void Max(std::atomic<u64>& counter, u64 value) {
    u64 current = counter.load(std::memory_order_relaxed);
    while (value > current &&
           !counter.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

/// Counts a batch of (address, size) ranges into `counters` (statistics only; sorts a copy).
template <typename Address, typename Size>
void CountInvalidations(InvalidationCounters& counters, std::span<const std::pair<Address, Size>> batch) {
    if (!Enabled() || batch.empty()) {
        return;
    }
    static thread_local std::vector<std::pair<u64, u64>> ranges;
    ranges.clear();
    for (const auto& [address, size] : batch) {
        if (size != 0) {
            ranges.emplace_back(static_cast<u64>(address), static_cast<u64>(address) + static_cast<u64>(size));
        }
    }
    std::ranges::sort(ranges);
    u64 merged = 0;
    u64 pages = 0;
    u64 end = 0;
    u64 last_page = ~u64{0};
    for (const auto& [begin, finish] : ranges) {
        if (merged == 0 || begin > end) {
            ++merged;
            end = finish;
        } else {
            end = (std::max)(end, finish);
        }
        for (u64 page = begin >> 12; page <= (finish - 1) >> 12; ++page) {
            if (page != last_page && (last_page == ~u64{0} || page > last_page)) {
                ++pages;
                last_page = page;
            }
        }
    }
    Add(counters.batches, 1);
    Add(counters.ranges, ranges.size());
    Add(counters.merged, merged);
    Add(counters.pages, pages);
}

/// Ends the current draw span in `bucket` and starts the next.
inline void Mark(Draw bucket) {
    if (Enabled()) {
        const u64 now = Stamp();
        draw_thread.draw[bucket] += now - draw_thread.last;
        draw_thread.last = now;
    }
}

/// Adds the time of a scope to a draw-thread counter.
class ScopedTime {
public:
    explicit ScopedTime(u64& counter_) : counter{counter_}, start{Enabled() ? Stamp() : 0} {}
    ~ScopedTime() {
        if (start != 0) {
            counter += Stamp() - start;
        }
    }
    ScopedTime(const ScopedTime&) = delete;
    ScopedTime& operator=(const ScopedTime&) = delete;

private:
    u64& counter;
    u64 start;
};

/// Adds the time of a scope to a shared counter (any thread).
class ScopedSharedTime {
public:
    explicit ScopedSharedTime(std::atomic<u64>& counter_)
        : counter{counter_}, start{Enabled() ? Stamp() : 0} {}
    ~ScopedSharedTime() {
        if (start != 0) {
            Add(counter, Stamp() - start);
        }
    }
    ScopedSharedTime(const ScopedSharedTime&) = delete;
    ScopedSharedTime& operator=(const ScopedSharedTime&) = delete;

private:
    std::atomic<u64>& counter;
    u64 start;
};

/// Times a cache lock acquisition outside draws: construct before the lock, call Acquired after it.
class LockTimer {
public:
    explicit LockTimer(LockCounters& counters_)
        : counters{is_draw_thread ? shared.draw_thread_locks : counters_},
          start{Enabled() ? Stamp() : 0} {}
    void Acquired() const {
        if (start != 0) {
            Add(counters.count, 1);
            Add(counters.ticks, Stamp() - start);
        }
    }

private:
    LockCounters& counters;
    u64 start;
};

/// Constructed right after a draw takes the cache locks, destroyed right before it releases them.
class DrawLockHold {
public:
    DrawLockHold() : start{Enabled() ? Stamp() : 0} {
        in_draw_lock = true;
    }
    /// Ends the Tail span here, so that the next span (Unlock) is the release itself.
    ~DrawLockHold() {
        Mark(Tail);
        in_draw_lock = false;
        if (start != 0) {
            draw_thread.lock_hold += Stamp() - start;
        }
    }
    DrawLockHold(const DrawLockHold&) = delete;
    DrawLockHold& operator=(const DrawLockHold&) = delete;

private:
    u64 start;
};

/// A scheduler wait: construct with the queue state when it begins, Done when it ends.
class WaitTimer {
public:
    WaitTimer(WaitCounters& counters_, u64 queued, bool retained)
        : counters{counters_}, start{Enabled() ? Stamp() : 0} {
        if (start != 0) {
            Add(counters.count, 1);
            Add(counters.queue_sum, queued);
            Max(counters.queue_max, queued);
            if (retained) {
                Add(counters.retained, 1);
            }
        }
    }
    ~WaitTimer() {
        if (start != 0) {
            const u64 ticks = Stamp() - start;
            Add(counters.ticks, ticks);
            if (in_draw_lock) {
                Add(counters.ticks_in_draw_lock, ticks);
            }
        }
    }
    WaitTimer(const WaitTimer&) = delete;
    WaitTimer& operator=(const WaitTimer&) = delete;

private:
    WaitCounters& counters;
    u64 start;
};

/// Logs and resets the counters every 4 s (draw thread only).
void Report();

/// Calls Report every so many draws (draw thread only).
inline void MaybeReport() {
    if (Enabled() && (++draw_thread.report_check & 255) == 0) {
        Report();
    }
}

} // namespace VideoCommon::BackStats
