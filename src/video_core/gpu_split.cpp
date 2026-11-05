// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/back_thread_stats.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/gpu_split.h"

namespace VideoCommon {

namespace {


void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

/// EDEN_GPU_SPLIT: '0' off, '1' engine methods on the back, '2' (default) macros on the front too.
char SplitMode() {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_GPU_SPLIT");
        return value != nullptr && value[0] != '\0' ? value[0] : '2';
    }();
    return mode;
}

/// Engine::ConsumeSink (the methods queued while not executable), timed with the stats on.
void ConsumeSinkTimed(Tegra::Engines::EngineInterface* engine, Core::System& system) {
    if (engine->method_sink.empty()) {
        return;
    }
    const VideoCommon::BackStats::ScopedTime sink_time{VideoCommon::BackStats::draw_thread.sink_ticks};
    engine->ConsumeSink(system);
}

} // Anonymous namespace

GpuSplit GpuSplit::instance;

void GpuSplit::ClearNativeFailures(Tegra::Engines::EngineInterface* engine) {
    for (bool* const flag : engine->native_failure_flags) {
        if (flag != nullptr) {
            *flag = false;
        }
    }
}

void GpuSplit::MaybeStart(Core::System& system_) {
    const char mode = SplitMode();
    if ((mode != '1' && mode != '2') ||
        !native_channel_seen.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand everything back to one thread (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() ==
                             Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop(false);
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need one thread");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
    // Statistics: the back prepares draws from here on; this thread (the front) may have before.
    VideoCommon::BackStats::is_draw_thread = false;
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
    const char* jit = std::getenv("EDEN_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::OnStop(std::function<void()>&& leave) {
    on_stop.push_back(std::move(leave));
}

void GpuSplit::Stop(bool session_end) {
    if (!back_thread.joinable()) {
        if (session_end) {
            on_stop.clear();
            native_channel_seen.store(false, std::memory_order_relaxed);
        }
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    active = false;
    // Engines that ran their macros on the front take them back (one thread from here on).
    // At the end of a session they may be gone already: nothing runs on them again anyway.
    auto leave = std::move(on_stop);
    on_stop.clear();
    if (!session_end) {
        for (auto& func : leave) {
            func();
        }
    } else {
        native_channel_seen.store(false, std::memory_order_relaxed);
    }
    // The split is static and outlives the emulation session. The next back thread starts
    // reading at 0: with the old positions kept it would skip through the new, empty ring a
    // wrap at a time and never reach the old write position (Eglantine 092c16d6).
    ring.reset();
    write_position = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    const VideoCommon::BackStats::ScopedSharedTime space_time{VideoCommon::BackStats::shared.space_ticks};
    WaitForBack(4096, [&] { return write_position + words - consumed_cache <= RingWords; });
}

template <typename Done>
void GpuSplit::WaitForBack(u32 spin_limit, Done&& done) {
    // Spin briefly (the back often catches up within microseconds), then sleep on `consumed`
    // until the back moves it. Yield-spinning here kept a host thread busy for as long as the back
    // took, often on the same physical core as the back thread itself, which is the bottleneck
    // in draw-heavy scenes. front_waiting/consumed form a Dekker pair with the back's
    // consumed-store/front_waiting-load (all seq_cst), so a wake-up is never missed.
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (done()) {
            return;
        }
        if (spins < spin_limit) {
            Pause();
            continue;
        }
        // Statistics: a stamp left by a call this front did not sleep through is not this sleep's.
        consumed_notify_stamp.store(0, std::memory_order_relaxed);
        front_waiting.store(true, std::memory_order_seq_cst);
        consumed_cache = consumed.load(std::memory_order_seq_cst);
        if (!done()) {
            consumed.wait(consumed_cache, std::memory_order_acquire);
            if (VideoCommon::BackStats::Enabled()) {
                namespace BackStats = VideoCommon::BackStats;
                ++front_sleeps;
                if (const u64 stamp = consumed_notify_stamp.exchange(0, std::memory_order_relaxed)) {
                    BackStats::Add(BackStats::shared.front_wake, BackStats::Stamp() - stamp);
                    BackStats::Add(BackStats::shared.front_wake_count, 1);
                }
            }
        }
        front_waiting.store(false, std::memory_order_relaxed);
    }
}

void GpuSplit::StoreConsumed(u64 position) {
    consumed.store(position, std::memory_order_seq_cst);
    if (front_waiting.load(std::memory_order_seq_cst)) {
        if (VideoCommon::BackStats::Enabled()) {
            consumed_notify_stamp.store(VideoCommon::BackStats::Stamp(), std::memory_order_relaxed);
        }
        consumed.notify_one();
    }
}

void GpuSplit::Publish() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = (std::min<size_t>)(publish_every * 2, PublishEvery);
    if (drained_stamp != 0) {
        namespace BackStats = VideoCommon::BackStats;
        BackStats::Add(BackStats::shared.drain_to_publish, BackStats::Stamp() - drained_stamp);
        BackStats::Add(BackStats::shared.drain_to_publish_count, 1);
        drained_stamp = 0;
    }
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        if (VideoCommon::BackStats::Enabled()) {
            publish_notify_stamp.store(VideoCommon::BackStats::Stamp(), std::memory_order_relaxed);
        }
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

void GpuSplit::Drain(u32 site) {
    if (!active) {
        return;
    }
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position) {
        return;
    }
    Publish();
    namespace BackStats = VideoCommon::BackStats;
    const u64 drain_start = BackStats::Enabled() ? BackStats::Stamp() : 0;
    const u64 sleeps_before = front_sleeps;
    WaitForBack(16384, [&] { return consumed_cache == write_position; });
    if (drain_start != 0) {
        // Only the drain's own sleeps (WaitForSpace sleeps in WaitForBack too).
        BackStats::Add(BackStats::shared.front_slept, front_sleeps - sleeps_before);
        drained_stamp = BackStats::Stamp();
        // Only drains that waited: those leave the back with nothing to do until the front sends more.
        const u64 ticks = BackStats::Stamp() - drain_start;
        BackStats::Add(BackStats::shared.drains, 1);
        BackStats::Add(BackStats::shared.drain_ticks, ticks);
        auto& counters = BackStats::shared.drain_sites[site < BackStats::shared.drain_sites.size() ? site : 0];
        BackStats::Add(counters.count, 1);
        BackStats::Add(counters.ticks, ticks);
    }
    publish_every = 8;
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    SelectEngine(engine);
    // Runs too long for a quarter of the ring go in pieces, as when a method's data continues
    // in the next GPFIFO entry (see MaxMultiWords).
    while (amount > 0) {
        const u32 piece = (std::min)(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            // A macro's write: a failure counts until the macro ends (the front sends that).
            ConsumeSinkTimed(engine, *system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            ConsumeSinkTimed(engine, *system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
            ClearNativeFailures(engine);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        ConsumeSinkTimed(engine, *system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        ClearNativeFailures(engine);
        return data + (amount + 1) / 2;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("GPUBack");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    system->RegisterHostThread();
    namespace BackStats = VideoCommon::BackStats;
    u64 read_position = 0;
    u64 idle_start = BackStats::Enabled() ? BackStats::Stamp() : 0;
    while (!stop_token.stop_requested()) {
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            if (idle_start != 0) {
                ++BackStats::draw_thread.back_idle_episodes;
            }
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position; ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                publish_notify_stamp.store(0, std::memory_order_relaxed);
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                if (available == read_position && !stop_token.stop_requested()) {
                    wake.wait(seen, std::memory_order_seq_cst);
                    if (idle_start != 0) {
                        ++BackStats::draw_thread.back_slept;
                        if (const u64 stamp = publish_notify_stamp.exchange(0, std::memory_order_relaxed)) {
                            BackStats::draw_thread.back_wake += BackStats::Stamp() - stamp;
                            ++BackStats::draw_thread.back_wake_count;
                        }
                    }
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 busy_start{};
        if (idle_start != 0) {
            busy_start = BackStats::Stamp();
            BackStats::draw_thread.back_idle += busy_start - idle_start;
            BackStats::draw_thread.back_loop = true;
        }
        u64 last_store = read_position;
        u64 entry_start = busy_start;
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            // Statistics: the entry's kind (method, macro write, multi-word method, call, other).
            const u64 head = ring[index];
            const size_t kind = (head & 0xf) == static_cast<u64>(Kind::Method)
                                    ? ((head & Direct) != 0 ? 1 : 0)
                                : (head & 0xf) == static_cast<u64>(Kind::Multi) ? 2
                                : (head & 0xf) == static_cast<u64>(Kind::Call)  ? 3
                                                                                : 4;
            const size_t size = Execute(&ring[index]);
            if (entry_start != 0) {
                const u64 now = BackStats::Stamp();
                BackStats::draw_thread.ring_ticks[kind] += now - entry_start;
                ++BackStats::draw_thread.ring_count[kind];
                entry_start = now;
            }
            read_position += size != 0 ? size : RingWords - index;
            // Hand ring space back now and then while working through a long batch.
            if (read_position - last_store >= 4096 && read_position != available) {
                StoreConsumed(read_position);
                last_store = read_position;
            }
        }
        StoreConsumed(read_position);
        if (busy_start != 0) {
            idle_start = BackStats::Stamp();
            BackStats::draw_thread.back_busy += idle_start - busy_start;
        }
    }
}

} // namespace VideoCommon
