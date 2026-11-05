// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bitset>
#include <cstdlib>
#include "common/assert.h"
#include "common/logging.h"
#include "shader_recompiler/exception.h"
#include "core/core.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/back_thread_stats.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/decoders.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif

namespace Tegra::Engines {

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_GPU_SPLIT=2; ported from
// Eglantine): the front runs compute macros against its own copy of the native registers (which
// hold the last value written to each, on the back too) and loads and binds the compute macro
// code itself.
struct KeplerCompute::FrontState {
    std::array<u32, 0x1000> registers{};
    std::optional<u32> executing_macro;
    std::vector<u32> parameters;
    bool compute_uploaded{};
    u32 bind_pointer{};
    MacroEngine* macro{}; ///< Without a native context: the one macro engine
};

KeplerCompute::KeplerCompute(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}
    , upload_state{memory_manager, regs.upload}
{
    execution_mask.reset();
    execution_mask[KEPLER_COMPUTE_REG_INDEX(exec_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(data_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(launch)] = true;
}

KeplerCompute::~KeplerCompute() = default;

void KeplerCompute::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer);
}

void KeplerCompute::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

bool KeplerCompute::ProcessNativeMethod(Core::System& system, u32 method, u32 argument, bool is_last_call) {
    if (native_context && !front_macros) {
        if (method == 0x45 || method == 0x47) native_compute_uploaded = true;
        native_macro = native_compute_uploaded ? &native_context->compute_engine : &native_context->engine;
        native_macro_positions = native_compute_uploaded ? std::span<u32>{native_context->compute_positions} : std::span<u32>{native_context->positions};
    }
    if (method >= native_registers.size()) {
        LOG_ERROR(HW_GPU, "Native compute method out of range: {:#x}", method);
        return true;
    }
    if (method >= 0xe00) {
        if (!native_executing_macro) {
            if (method & 1) { LOG_ERROR(HW_GPU, "Native compute macro starts on argument method"); return true; }
            native_executing_macro = method;
        }
        native_parameters.push_back(argument);
        if (!is_last_call) return true;
        const u32 entry = ((*native_executing_macro - 0xe00) / 2) % native_macro_positions.size();
        native_executing_macro.reset();
        const auto read = [&](u32 address) {
            return address < native_registers.size() ? native_registers[address] : 0U;
        };
        const auto emit = [&](u32 address, u32 value) {
            static u32 diagnostic_writes{};
            if (diagnostic_writes++ < 4096) LOG_DEBUG(HW_GPU, "Native compute MME emit method={:#x}, value={:#x}", address, value);
            if (address >= 0xe00) return false;
            upload_state.FlushPartial();
            CallMethod(system, address, value, true);
            return true;
        };
        const auto result = [&] {
#ifdef ARCHITECTURE_x86_64
            if (TuringMacroJitEnabled()) {
                if (!native_macro->native_jit) {
                    native_macro->native_jit = std::make_shared<TuringMacroJit>();
                }
                if (native_macro->native_jit->Prepare(native_macro->native_code,
                                                      native_macro->native_code_generation)) {
                    return native_macro->native_jit->Execute(native_macro_positions[entry],
                                                             native_parameters,
                                                             native_interpreter.dram, read, emit);
                }
            }
#endif
            return native_interpreter.ExecuteDecoded(native_macro->native_decoded_code,
                                                     native_macro_positions[entry], native_parameters,
                                                     read, emit);
        }();
        upload_state.FlushPartial();
        if (!result.success) {
            native_macro->native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Native MME failed: compute start={:#x}, pc={:#x}, consumed={}/{}, error={}, parameters={:08x}",
                native_macro_positions[entry], result.pc, result.consumed, native_parameters.size(), result.error,
                fmt::join(native_parameters, ","));
        }
        native_parameters.clear();
        return true;
    }
    native_registers[method] = argument;
    if (method == 0x107) {
        const GPUVAddr address = ((GPUVAddr{native_registers[0xc6]} << 32) |
            native_registers[0xc7]) << 8;
        const u32 control = native_registers[0xa7];
        if (!memory_manager.IsFullyMappedRange(address, 256)) {
            native_macro->native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Unmapped native inline QMD destination");
            return true;
        }
        rasterizer->AccelerateInlineToMemory(address, 256,
            std::span<const u8>{reinterpret_cast<const u8*>(native_registers.data() + 0xc8), 256});
        if ((control & (1U << 24)) || !(native_registers[0xcc] & (1U << 12))) {
            const u32 previous = native_registers[0xad];
            native_registers[0xad] = static_cast<u32>(address >> 8);
            ProcessNativeMethod(system, 0xb0, 3, true);
            native_registers[0xad] = previous;
        }
        return true;
    }
    if (front_macros && method >= 0x45 && method <= 0x48) {
        return true; // Loaded and bound by the split GPU thread's front (FrontApply).
    }
    switch (method) {
    case 0x45: native_macro->ClearCode(argument); return true;
    case 0x46: native_macro->AddCode(0, argument); return true;
    case 0x47: native_bind_pointer = argument; return true;
    case 0x48:
        if (native_bind_pointer < native_macro_positions.size()) native_macro_positions[native_bind_pointer++] = argument;
        else LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        return true;
    case 0xae: case 0xaf: case 0xb0: {
        const GPUVAddr qmd_address = GPUVAddr{native_registers[0xad]} << 8;
        auto& scheduler = native_context
            ? native_context->GetQmdScheduler(memory_manager.GetID()) : native_qmd;
        if (method == 0xae || (method == 0xaf && !(argument & 2)) ||
            (method == 0xb0 && ((argument & 15) == 0 || (argument & 15) == 1))) return true;
        const auto error = scheduler.Signal(qmd_address, argument,
            [&](u64 address, NativeQmdScheduler::Descriptor& descriptor) {
                if (!memory_manager.IsFullyMappedRange(address, sizeof(descriptor))) return false;
                // QMD fields can be patched by earlier GPU writes. Fetch the
                // descriptor through the cache-coherent path before scheduling.
                memory_manager.ReadBlock(address, descriptor.data(), sizeof(descriptor));
                return true;
            },
            [&](u64 address, u32 payload, u32 flags) {
                if (!memory_manager.IsFullyMappedRange(address, sizeof(payload))) return false;
                if (flags & (1U << 26)) {
                    VideoCommon::BackStats::NoteRelease(address, VideoCommon::BackStats::ReleaseQmdAdd);
                    rasterizer->SignalFenceCoalesced([this, address, payload] {
                        const u32 value = memory_manager.Read<u32>(address) + payload;
                        memory_manager.Write<u32>(address, value);
                    });
                } else {
                    VideoCommon::BackStats::NoteRelease(address, VideoCommon::BackStats::ReleaseQmd);
                    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                        VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
                }
                return true;
            }, [&](const auto& descriptor) { return DispatchNativeCompute(descriptor); });
        if (error.empty()) {
            LOG_DEBUG(HW_GPU, "Native QMD control action={:#x}, QMD={:#x}", argument, qmd_address);
            return true;
        }
        native_macro->native_execution_failed = true;
        if (error == "native shader dispatch skipped") {
            // Rate-limited: the first 32, then every 1024th with the running total.
            static std::atomic<u64> skipped_dispatches{};
            const u64 count = ++skipped_dispatches;
            if (count <= 32 || count % 1024 == 0) {
                LOG_ERROR(HW_GPU, "Native QMD failed: {} (dispatches skipped so far: {})", error, count);
            }
        } else {
            LOG_ERROR(HW_GPU, "Native QMD failed: {}", error);
        }
        if (error == "native shader dispatch skipped") {
            // The dispatch already completed and released its semaphores. The QMD and macro dumps
            // below write several files per call, which dominates the frame time when a shader is
            // rejected on every dispatch.
            return true;
        }
        std::array<u32, 128> qmd{};
        if (memory_manager.IsFullyMappedRange(qmd_address, sizeof(qmd))) {
            memory_manager.ReadBlockUnsafe(qmd_address, qmd.data(), sizeof(qmd));
        }
        LOG_ERROR(HW_GPU, "Native compute dispatch unsupported: method={:#x}, value={:#x}, QMD={:#x}, words={:08x}", method, argument,
                  qmd_address, fmt::join(qmd, ","));
        return true;
    }
    default:
        // The inline upload register block is shared with the older class.
        if (method < Regs::NUM_REGS) regs.reg_array[method] = argument;
        return method != KEPLER_COMPUTE_REG_INDEX(exec_upload) && method != KEPLER_COMPUTE_REG_INDEX(data_upload);
    }
}

void KeplerCompute::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (native_mode && ProcessNativeMethod(system, method, method_argument, is_last_call)) return;
    if (method >= Regs::NUM_REGS) {
        LOG_ERROR(HW_GPU, "Compute register out of range: {:#x}", method);
        return;
    }

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(exec_upload): {
        UploadInfo info{.upload_address = upload_address,
                        .exec_address = upload_state.ExecTargetAddress(),
                        .copy_size = upload_state.GetUploadSize(),
                        .was_dirty = upload_dirty};
        uploads.push_back(info);
        upload_state.ProcessExec(regs.exec_upload.linear != 0);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(data_upload): {
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(method_argument, is_last_call);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(launch): {
        const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();

        for (auto& data : uploads) {
            const GPUVAddr offset = data.exec_address - launch_desc_loc;
            if (offset / sizeof(u32) == LAUNCH_REG_INDEX(grid_dim_x)) {
                const bool source_dirty = memory_manager.IsMemoryDirty(data.upload_address, data.copy_size);
                if (data.was_dirty || source_dirty) {
                    indirect_compute = {data.upload_address};
                }
            }
        }
        uploads.clear();
        ProcessLaunch();
        indirect_compute = std::nullopt;
        break;
    }
    default:
        break;
    }
}

void KeplerCompute::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    if (native_mode) {
        for (u32 i = 0; i < amount; ++i) CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        return;
    }
    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(data_upload):
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

bool KeplerCompute::DispatchNativeCompute(const NativeQmdScheduler::Descriptor& qmd) {
    launch_description = {};
    launch_description.grid_dim_x.Assign(qmd[12]);
    launch_description.grid_dim_y.Assign(qmd[13] & 0xffff);
    launch_description.grid_dim_z.Assign(qmd[14] & 0xffff);
    launch_description.shared_alloc.Assign(qmd[17] & 0x3ffff);
    launch_description.block_dim_x.Assign(qmd[18] >> 16);
    launch_description.block_dim_y.Assign(qmd[19] & 0xffff);
    launch_description.block_dim_z.Assign(qmd[19] >> 16);
    launch_description.const_buffer_enable_mask.Assign(qmd[20] & 0xff);
    launch_description.local_pos_alloc.Assign(qmd[23] & 0xffffff);
    launch_description.local_crs_alloc.Assign(qmd[50] & 0xffffff);
    regs.code_loc.address_low = qmd[48];
    regs.code_loc.address_high = qmd[49] & 0x1ffff;
    for (u32 slot = 0; slot < NumConstBuffers; ++slot) {
        const u32 high = qmd[33 + slot * 2];
        if (high & 0x1ff00) {
            LOG_ERROR(HW_GPU, "Native compute constant address exceeds supported GPU width");
            return false;
        }
        auto& buffer = launch_description.const_buffer_config[slot];
        buffer.address_low = qmd[32 + slot * 2];
        buffer.address_high.Assign(high & 0xff);
        buffer.size.Assign(((high >> 19) & 0x1fff) << 4);
    }
    if (!launch_description.block_dim_x || !launch_description.block_dim_y || !launch_description.block_dim_z)
        return false;
    try {
        rasterizer->DispatchCompute();
    } catch (const Shader::Exception& error) {
        // Rate-limited: the first 32, then every 1024th with the running total.
        static std::atomic<u64> rejected_compute{};
        const u64 count = ++rejected_compute;
        if (count <= 32 || count % 1024 == 0) {
            LOG_CRITICAL(HW_GPU, "Native compute shader rejected: {} (compute rejections so far: {})", error.what(), count);
        }
        return false;
    }
    return true;
}

void KeplerCompute::ProcessLaunch() {
    const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();
    memory_manager.ReadBlockUnsafe(launch_desc_loc, &launch_description,
                                   LaunchParams::NUM_LAUNCH_PARAMETERS * sizeof(u32));
    rasterizer->DispatchCompute();
}

Texture::TICEntry KeplerCompute::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tic.Address() + tic_index * sizeof(Texture::TICEntry)};

    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry KeplerCompute::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tsc.Address() + tsc_index * sizeof(Texture::TSCEntry)};

    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

KeplerCompute::FrontState& KeplerCompute::Front(VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle.
        split.Drain(VideoCommon::BackStats::DrainFrontCompute);
        front = std::make_unique<FrontState>();
        front->registers = native_registers;
        front->executing_macro = native_executing_macro;
        front->parameters = std::move(native_parameters);
        native_parameters.clear();
        native_executing_macro.reset();
        front->compute_uploaded = native_compute_uploaded;
        front->bind_pointer = native_bind_pointer;
        front->macro = native_macro;
        front_macros = true;
        split.OnStop([this] { LeaveFront(); });
    }
    return *front;
}

void KeplerCompute::FrontModeChanged(bool context_changed) {
    // SetNativeMacroMode on the front (a subchannel bind, after the back drained). Whether the
    // compute macros were uploaded is the front's to track (the back skips those methods): it
    // only starts over with another context, as SetNativeMacroMode does.
    if (context_changed) {
        front->compute_uploaded = false;
    }
    front->macro = native_macro;
}

void KeplerCompute::LeaveFront() {
    if (!front) {
        return;
    }
    // The back thread is gone: the engine runs its macros itself again.
    native_executing_macro = front->executing_macro;
    native_parameters = std::move(front->parameters);
    native_compute_uploaded = front->compute_uploaded;
    native_bind_pointer = front->bind_pointer;
    native_macro = &FrontMacroEngine(*front);
    native_macro_positions = FrontMacroPositions(*front);
    front.reset();
    front_macros = false;
}

void KeplerCompute::FrontApply(FrontState& fs, u32 method, u32 argument) {
    // As ProcessNativeMethod does with the macro code and bindings.
    if (method == 0x45 || method == 0x47) {
        fs.compute_uploaded = true;
    }
    fs.registers[method] = argument;
    switch (method) {
    case 0x45:
        FrontMacroEngine(fs).ClearCode(argument);
        break;
    case 0x46:
        FrontMacroEngine(fs).AddCode(0, argument);
        break;
    case 0x47:
        fs.bind_pointer = argument;
        break;
    case 0x48: {
        const auto positions = FrontMacroPositions(fs);
        if (fs.bind_pointer < positions.size()) {
            positions[fs.bind_pointer++] = argument;
        } else {
            LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        }
        break;
    }
    default:
        break;
    }
}

MacroEngine& KeplerCompute::FrontMacroEngine(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? native_context->compute_engine : native_context->engine;
    }
    return *fs.macro;
}

std::span<u32> KeplerCompute::FrontMacroPositions(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? std::span<u32>{native_context->compute_positions}
                                   : std::span<u32>{native_context->positions};
    }
    return native_macro_positions;
}

void KeplerCompute::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                    u32 method, u32 argument, bool is_last_call,
                                    const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= fs.registers.size()) {
        split.Method(this, method, argument, is_last_call, segment);
        return;
    }
    if (method >= 0xe00) {
        if (!fs.executing_macro) {
            if (method & 1) {
                LOG_ERROR(HW_GPU, "Native compute macro starts on argument method");
                return;
            }
            fs.executing_macro = method;
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontApply(fs, method, argument);
    split.Method(this, method, argument, is_last_call, segment);
}

void KeplerCompute::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                         u32 method, const u32* base_start, u32 amount,
                                         u32 methods_pending, const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= 0xe00 && method < fs.registers.size()) {
        for (u32 i = 0; i < amount; ++i) {
            FrontCallMethod(system, split, method, base_start[i], methods_pending - i <= 1, segment);
        }
        return;
    }
    if (method < fs.registers.size()) {
        for (u32 i = 0; i < amount; ++i) {
            FrontApply(fs, method, base_start[i]);
        }
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, segment);
}

void KeplerCompute::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                                  FrontState& fs) {
    const auto positions = FrontMacroPositions(fs);
    const u32 start = positions[((*fs.executing_macro - 0xe00) / 2) % positions.size()];
    fs.executing_macro.reset();
    auto& macro = FrontMacroEngine(fs);
    // An inline upload the pushbuffer left unfinished is flushed before the macro's writes and
    // after them, as ProcessNativeMethod's emits do.
    split.Defer([this] { upload_state.FlushPartial(); });
    const auto read = [&fs](u32 address) {
        return address < fs.registers.size() ? fs.registers[address] : 0U;
    };
    const auto emit = [&](u32 address, u32 value) {
        if (address >= 0xe00) {
            return false;
        }
        FrontApply(fs, address, value);
        split.MacroWrite(this, address, value);
        return true;
    };
    const auto result = [&] {
#ifdef ARCHITECTURE_x86_64
        if (!macro.native_jit) {
            macro.native_jit = std::make_shared<TuringMacroJit>();
        }
        if (macro.native_jit->Prepare(macro.native_code, macro.native_code_generation)) {
            return macro.native_jit->Execute(start, fs.parameters, native_interpreter.dram, read,
                                             emit);
        }
#endif
        return native_interpreter.ExecuteDecoded(macro.native_decoded_code, start, fs.parameters,
                                                 read, emit);
    }();
    split.Defer([this] {
        upload_state.FlushPartial();
        // A failure the macro's writes caused lasts until it ends, as on one thread.
        VideoCommon::GpuSplit::ClearNativeFailures(this);
    });
    if (!result.success) {
        // On one thread the failure flag only matters to the method's own draws (none here):
        // DmaPusher clears it right after.
        LOG_ERROR(HW_GPU, "Native MME (split front) failed: compute start={:#x}, pc={:#x}, consumed={}/{}, error={}",
                  start, result.pc, result.consumed, fs.parameters.size(), result.error);
    }
    fs.parameters.clear();
}

} // namespace Tegra::Engines
