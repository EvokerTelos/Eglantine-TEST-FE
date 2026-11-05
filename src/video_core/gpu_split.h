// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

// Splits the GPU thread in two (ported from Eglantine's gpu_split, NX2_GPU_THREAD_SPLIT.md).
// The GPU thread (the front) takes the CPU's commands, parses command lists and runs the puller;
// engine method calls and everything behind them (register state, draws, dispatches, the
// rasterizer and its caches) run in order on a second thread (the back), fed through a
// single-producer single-consumer ring. Work the front would do on back-owned state is deferred
// into the same ring; the front waits for the back to catch up (Drain) only where it needs a
// result from that state, e.g. a semaphore it acquires or memory a macro reads.
//
// EDEN_GPU_SPLIT=2 (the default) also runs the 3D and compute engines' macros on the front,
// against copies of the registers they read (Maxwell3D and KeplerCompute Front*), and sends the
// writes they make. EDEN_GPU_SPLIT=1 keeps the macros on the back; EDEN_GPU_SPLIT=0 runs
// everything on one thread. Only for Switch 2 channels, Vulkan and a non-accurate GPU.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// A channel with the Switch 2 ABI submitted work: the split may start.
    static void NoteNativeChannel() {
        native_channel_seen.store(true, std::memory_order_relaxed);
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_GPU_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Starts the back thread once the split is wanted and allowed, and stops it when the
    /// settings stop allowing it. Called by the front between commands.
    void MaybeStart(Core::System& system);
    /// Stops the back thread. `session_end`: the emulation session ends (the engines may be gone).
    void Stop(bool session_end = true);
    /// Runs `leave` on the front when the split stops mid-session: an engine whose macros the
    /// front ran takes them back for one thread.
    void OnStop(std::function<void()>&& leave);

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
    }

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    template <typename Func>
    void Defer(const Func& func) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func);

    /// On one GPU thread DmaPusher clears the channel's native failure flags after each method
    /// (HasFailedNativeWork): a failure only skips the draws of the method that failed. The back
    /// does it after each method it executes, and the front's macros at their end.
    static void ClearNativeFailures(Tegra::Engines::EngineInterface* engine);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far. `site`: the caller, for statistics
    /// (VideoCommon::BackStats::DrainSite).
    void Drain(u32 site = 0);

private:
    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1), Direct
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    /// Multi-word runs go whole up to this size: engines treat a run that ends before
    /// methods_pending as continued, which inline uploads (non-linear ones, Maxwell3D inline_data)
    /// do not handle. Larger runs (over 1 MiB of data) go in pieces so they fit the ring.
    static constexpr u32 MaxMultiWords = static_cast<u32>(RingWords / 4);
    static constexpr size_t PublishEvery = 256;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        if (write_position - published_local >= publish_every) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    /// Front: spins up to spin_limit pauses, then sleeps until the back moves `consumed`, until
    /// done() (which reads consumed_cache) holds.
    template <typename Done>
    void WaitForBack(u32 spin_limit, Done&& done);
    /// Back: publishes its read position and wakes the front if it sleeps on it.
    void StoreConsumed(u64 position);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);

    void BackLoop(std::stop_token stop_token);
    size_t Execute(const u64* entry);

    Core::System* system{};
    std::vector<std::function<void()>> on_stop;
    bool active{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    static GpuSplit instance;
    static inline std::atomic<bool> native_channel_seen{};

    alignas(64) std::atomic<u64> published{};
    alignas(64) std::atomic<u64> consumed{};
    alignas(64) std::atomic<bool> back_waiting{};
    // The front sleeps on `consumed` once Drain/WaitForSpace have spun a while; the back wakes it.
    alignas(64) std::atomic<bool> front_waiting{};
    // Statistics (EDEN_BACK_THREAD_STATS): when each side last called the other awake.
    std::atomic<u64> consumed_notify_stamp{};
    std::atomic<u64> publish_notify_stamp{};
    u64 drained_stamp{}; ///< Front: when the last drain ended, until the next publish
    u64 front_sleeps{};  ///< Front: sleeps in WaitForBack (statistics)
    std::atomic<u32> wake{};
    std::jthread back_thread;
};

} // namespace VideoCommon
