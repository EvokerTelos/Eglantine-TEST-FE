// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/back_thread_stats.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

namespace {

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

u32 ReadNativeSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    const auto cpu_address = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (cpu_address && dma_pusher.rasterizer->MustFlushRegion(*cpu_address, sizeof(u32), VideoCommon::CacheType::BufferCache)) {
        namespace BackStats = VideoCommon::BackStats;
        const u64 readback_start = BackStats::Enabled() ? BackStats::Stamp() : 0;
        dma_pusher.rasterizer->FlushRegion(*cpu_address, sizeof(u32), VideoCommon::CacheType::BufferCache);
        if (readback_start != 0) {
            BackStats::Add(BackStats::shared.semaphore_readbacks, 1);
            BackStats::Add(BackStats::shared.semaphore_readback_ticks, BackStats::Stamp() - readback_start);
        }
        const u32 value = dma_pusher.memory_manager.Read<u32>(address);
        LOG_WARNING(HW_GPU, "Native semaphore buffer readback: address={:#x}, current={}", address, value);
        return value;
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}


// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
template <typename Func>
void OnBack(const Func& func) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func);
    } else {
        func();
    }
}

void DrainBack(VideoCommon::BackStats::DrainSite site) {
    VideoCommon::GpuSplit::Get().Drain(site);
}

} // Anonymous namespace

void Puller::ProcessBindMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_DEBUG(HW_GPU, "Binding subchannel {} to engine {:#x}", method_call.subchannel, method_call.argument);
    u32 engine = method_call.argument;
    if ((engine & ~Gf100BindClassMask) != 0 && (engine & ~Gf100BindValidMask) == 0) {
        engine &= Gf100BindClassMask;
    }

    // Binding changes engine state the back thread uses (macro modes, tiling).
    DrainBack(VideoCommon::BackStats::DrainBind);
    if (dma_pusher.channel_state.native_abi) dma_pusher.memory_manager.EnableNativeTiling();
    const auto engine_id = static_cast<EngineID>(engine);
    bound_engines[method_call.subchannel] = engine_id;
    switch (engine_id) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->fermi_2d, method_call.subchannel, EngineTypes::Fermi2D);
        break;
    case EngineID::AMPERE_B:
        if (!dma_pusher.channel_state.native_abi) { UNIMPLEMENTED_MSG("Native GPU class on legacy channel"); return; }
        dma_pusher.channel_state.payload->maxwell_3d.SetNativeTuringMacroMode(true);
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.SetNativeTuringMacroMode(false);
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::AMPERE_COMPUTE_B:
        if (!dma_pusher.channel_state.native_abi) { UNIMPLEMENTED_MSG("Native GPU class on legacy channel"); return; }
        [[fallthrough]];
    case EngineID::KEPLER_COMPUTE_B:
        if (engine_id == EngineID::AMPERE_COMPUTE_B && dma_pusher.channel_state.native_macro_context) {
            auto& context = *dma_pusher.channel_state.native_macro_context;
            dma_pusher.channel_state.payload->kepler_compute.SetNativeMacroMode(&context.engine, context.positions, &context);
        } else {
            dma_pusher.channel_state.payload->kepler_compute.SetNativeMacroMode(
                engine_id == EngineID::AMPERE_COMPUTE_B ? &dma_pusher.channel_state.payload->maxwell_3d.GetNativeMacroEngine() : nullptr,
                dma_pusher.channel_state.payload->maxwell_3d.GetNativeMacroPositions());
        }
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_compute, method_call.subchannel, EngineTypes::KeplerCompute);
        break;
    case EngineID::AMPERE_DMA_COPY_B:
        if (!dma_pusher.channel_state.native_abi) { UNIMPLEMENTED_MSG("Native GPU class on legacy channel"); return; }
        [[fallthrough]];
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_dma, method_call.subchannel, EngineTypes::MaxwellDMA);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_memory, method_call.subchannel, EngineTypes::KeplerMemory);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->nv01_timer, method_call.subchannel, EngineTypes::Nv01Timer);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine {:04X}", engine_id);
        break;
    }
}

void Puller::ProcessFenceActionMethod(DmaPusher& dma_pusher) {
    switch (regs.fence_action.op) {
    case Puller::FenceOperation::Acquire:
        // UNIMPLEMENTED_MSG("Channel Scheduling pending.");
        // WaitFence(regs.fence_action.syncpoint_id, regs.fence_value);
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
        break;
    case Puller::FenceOperation::Increment:
        OnBack([rasterizer = dma_pusher.rasterizer,
                id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
            rasterizer->SignalSyncPoint(id);
        });
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented operation {}", regs.fence_action.op.Value());
        break;
    }
}

bool Puller::CanResumeNativeAcquire(DmaPusher& dma_pusher) const {
    if (!native_acquire) {
        return false;
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadNativeSemaphoreWord asks are its state).
    DrainBack(VideoCommon::BackStats::DrainNativeAcquire);
    return native_acquire && !dma_pusher.memory_manager.IsShuttingDown() &&
           native_acquire->Matches(ReadNativeSemaphoreWord(dma_pusher, native_acquire->address));
}

bool Puller::ResumeNativeAcquire(DmaPusher& dma_pusher) {
    if (!native_acquire) return true;
    if (!CanResumeNativeAcquire(dma_pusher)) return false;
    LOG_WARNING(HW_GPU, "Native channel resumed after semaphore: channel={}, address={:#x}, current={}, required={}",
                dma_pusher.channel_state.bind_id, native_acquire->address,
                dma_pusher.memory_manager.Read<u32>(native_acquire->address), native_acquire->value);
    native_acquire.reset();
    return true;
}

void Puller::ProcessSemaphoreTriggerMethod(DmaPusher& dma_pusher) {
    const auto semaphoreOperationMask = 0xF;
    const auto op = GpuSemaphoreOperation(regs.semaphore_trigger & semaphoreOperationMask);
    if (op == GpuSemaphoreOperation::WriteLong) {
        const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
        const u32 payload = regs.semaphore_sequence;
        const auto properties = dma_pusher.channel_state.native_abi && (regs.semaphore_trigger & (1U << 24))
            ? VideoCommon::QueryPropertiesFlags::IsAFence : VideoCommon::QueryPropertiesFlags::HasTimeout;
        OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, properties, payload] {
            VideoCommon::BackStats::NoteRelease(sequence_address, VideoCommon::BackStats::ReleasePuller);
            rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload, properties, payload, 0);
        });
    } else {
        // Acquires read what the back may still have to write: let it finish first. From here
        // on the back is idle, and the front may use the rasterizer until it sends more.
        // Statistics only: the word as guest memory holds it before the drain (a plain read; the
        // back may be writing it, which is what the drain is for).
        const bool probe = VideoCommon::BackStats::Enabled() && dma_pusher.channel_state.native_abi;
        const u32 word_before =
            probe ? dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress()) : 0;
        DrainBack(VideoCommon::BackStats::DrainSemaphoreAcquire);
        do {
            u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
            if (dma_pusher.channel_state.native_abi) {
                word = ReadNativeSemaphoreWord(dma_pusher, regs.semaphore_address.SemaphoreAddress());
                const auto satisfies = [&](u32 value) {
                    if (op == GpuSemaphoreOperation::AcquireEqual) return value == regs.semaphore_sequence;
                    if (op == GpuSemaphoreOperation::AcquireGequal) return value >= regs.semaphore_sequence;
                    if (op == GpuSemaphoreOperation::AcquireMask) return (value & regs.semaphore_sequence) != 0;
                    return false;
                };
                const auto ready = [&] { return satisfies(word); };
                namespace BackStats = VideoCommon::BackStats;
                if (probe) {
                    if (satisfies(word_before)) {
                        BackStats::Add(BackStats::shared.acquire_ready_before, 1);
                    }
                    // The raw word again: `word` may come from a readback of a GPU buffer, which
                    // is not a change the drain made (counted apart, semaphore_readbacks).
                    const u32 word_after = dma_pusher.memory_manager.Read<u32>(
                        regs.semaphore_address.SemaphoreAddress());
                    if (word_before != word_after) {
                        BackStats::Add(BackStats::shared.acquire_changed, 1);
                        // The back is idle after the drain: its notes are safe to read here.
                        BackStats::Add(BackStats::shared.acquire_release_source[BackStats::LastRelease(
                                           regs.semaphore_address.SemaphoreAddress())],
                                       1);
                    }
                    if (op == GpuSemaphoreOperation::AcquireEqual) {
                        BackStats::Add(BackStats::shared.acquire_ops[0], 1);
                    } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                        BackStats::Add(BackStats::shared.acquire_ops[1], 1);
                    } else if (op == GpuSemaphoreOperation::AcquireMask) {
                        BackStats::Add(BackStats::shared.acquire_ops[2], 1);
                    }
                }
                const bool ready_at_once = ready();
                if (!ready_at_once) {
                    const BackStats::ScopedSharedTime fence_time{BackStats::shared.acquire_fence_ticks};
                    dma_pusher.rasterizer->ReleaseFences(true);
                    word = ReadNativeSemaphoreWord(dma_pusher, regs.semaphore_address.SemaphoreAddress());
                }
                if (BackStats::Enabled()) {
                    BackStats::Add(ready_at_once ? BackStats::shared.acquire_ready
                                   : ready()     ? BackStats::shared.acquire_ready_after_fences
                                                 : BackStats::shared.acquire_suspended,
                                   1);
                }
                if (!ready()) {
                    if (!NativeSemaphoreWait::Supported(static_cast<u32>(op))) {
                        if (auto context = dma_pusher.channel_state.native_macro_context) context->engine.native_execution_failed = true;
                        LOG_ERROR(HW_GPU, "Unsupported native semaphore operation {}", static_cast<u32>(op));
                    } else {
                        native_acquire = NativeSemaphoreWait{regs.semaphore_address.SemaphoreAddress(), regs.semaphore_sequence, static_cast<u32>(op)};
                        LOG_WARNING(HW_GPU, "Native channel suspended for semaphore: channel={}, address={:#x}, current={}, required={}",
                                    dma_pusher.channel_state.bind_id, native_acquire->address, word, native_acquire->value);
                    }
                }
                break;
            }
            regs.acquire_source = true;
            regs.acquire_value = regs.semaphore_sequence;
            if (op == GpuSemaphoreOperation::AcquireEqual) {
                regs.acquire_active = true;
                regs.acquire_mode = false;
                if (word != regs.acquire_value) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    dma_pusher.rasterizer->ReleaseFences();
                    continue;
                }
            } else {
                LOG_ERROR(HW_GPU, "Invalid semaphore operation");
            }
        } while (false);
    }
}

void Puller::ProcessSemaphoreRelease(DmaPusher& dma_pusher) {
    const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
    const u32 payload = regs.semaphore_release;
    OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, payload] {
        VideoCommon::BackStats::NoteRelease(sequence_address, VideoCommon::BackStats::ReleasePuller);
        rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload,
                          VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
    });
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    DrainBack(VideoCommon::BackStats::DrainAcquire);
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    const auto value = regs.semaphore_acquire;
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        dma_pusher.rasterizer->ReleaseFences();
        word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
        // TODO(kemathe73) figure out how to do the acquire_timeout
        regs.acquire_mode = false;
        regs.acquire_source = false;
    }
}

/// Calls a GPU puller method.
void Puller::CallPullerMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    regs.reg_array[method_call.method] = method_call.argument;
    const auto method = static_cast<BufferMethods>(method_call.method);

    switch (method) {
    case BufferMethods::BindObject: {
        ProcessBindMethod(dma_pusher, method_call);
        break;
    }
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        break;
    case BufferMethods::RefCnt:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); });
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        break;
    case BufferMethods::WaitForIdle:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); });
        break;
    case BufferMethods::SemaphoreOperation: {
        ProcessSemaphoreTriggerMethod(dma_pusher);
        break;
    }
    case BufferMethods::NonStallInterrupt: {
        LOG_ERROR(HW_GPU, "Special puller engine method NonStallInterrupt not implemented");
        break;
    }
    case BufferMethods::MemOpA: {
        LOG_TRACE(HW_GPU, "Memory Operation A: argument={:#x}", method_call.argument);
        break;
    }
    case BufferMethods::MemOpB: {
        // Implement this better.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); });
        break;
    }
    case BufferMethods::MemOpC:
    case BufferMethods::MemOpD: {
        LOG_TRACE(HW_GPU, "Memory Operation {}: argument={:#x}",
                  method == BufferMethods::MemOpC ? 'C' : 'D', method_call.argument);
        break;
    }
    case BufferMethods::SemaphoreAcquire: {
        ProcessSemaphoreAcquire(dma_pusher);
        break;
    }
    case BufferMethods::SemaphoreRelease: {
        ProcessSemaphoreRelease(dma_pusher);
        break;
    }
    case BufferMethods::Yield: {
        // TODO(Kmather73): Research and implement this method.
        LOG_ERROR(HW_GPU, "Special puller engine method Yield not implemented");
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Special puller engine method {:X} not implemented", method);
        break;
    }
}

/// Calls a GPU engine method.
void Puller::CallEngineMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    const EngineID engine = bound_engines[method_call.subchannel];
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::AMPERE_B:
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::AMPERE_COMPUTE_B:
        if (!dma_pusher.channel_state.native_abi) { UNIMPLEMENTED_MSG("Native GPU class on legacy channel"); return; }
        [[fallthrough]];
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::AMPERE_DMA_COPY_B:
        if (!dma_pusher.channel_state.native_abi) { UNIMPLEMENTED_MSG("Native GPU class on legacy channel"); return; }
        [[fallthrough]];
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU engine multivalue method.
void Puller::CallEngineMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    const EngineID engine = bound_engines[subchannel];
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::AMPERE_B:
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::AMPERE_COMPUTE_B:
        if (!dma_pusher.channel_state.native_abi) { UNIMPLEMENTED_MSG("Native GPU class on legacy channel"); return; }
        [[fallthrough]];
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::AMPERE_DMA_COPY_B:
        if (!dma_pusher.channel_state.native_abi) { UNIMPLEMENTED_MSG("Native GPU class on legacy channel"); return; }
        [[fallthrough]];
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU method.
void Puller::CallMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method_call.method, method_call.subchannel);
    ASSERT(method_call.subchannel < bound_engines.size());

    if (ExecuteMethodOnEngine(dma_pusher, method_call.method)) {
        CallEngineMethod(dma_pusher, method_call);
    } else {
        CallPullerMethod(dma_pusher, method_call);
    }
}

/// Calls a GPU multivalue method.
void Puller::CallMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method, subchannel);
    ASSERT(subchannel < bound_engines.size());
    if (ExecuteMethodOnEngine(dma_pusher, method)) {
        CallEngineMultiMethod(dma_pusher, method, subchannel, base_start, amount, methods_pending);
    } else {
        for (u32 i = 0; i < amount; i++) {
            CallPullerMethod(dma_pusher, MethodCall{
                method,
                base_start[i],
                subchannel,
                methods_pending - i,
            });
        }
    }
}

/// Determines where the method should be executed.
[[nodiscard]] bool Puller::ExecuteMethodOnEngine(DmaPusher& dma_pusher, u32 method) {
    const auto buffer_method = BufferMethods(method);
    return buffer_method >= BufferMethods::NonPullerMethods;
}

} // namespace Tegra::Engines
