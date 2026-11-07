// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <locale>
#include <string_view>
#include <cstdlib>
#include <array>
#include <algorithm>
#include "common/hex_util.h"
#include "common/swap.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_process_page_table.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/service/hid/hid_server.h"
#include "core/hle/service/sm/sm.h"
#include "core/memory.h"
#include "core/memory/cheat_engine.h"
#include "core/perf_stats.h"
#include "hid_core/resource_manager.h"
#include "hid_core/resources/npad/npad.h"

namespace Core::Memory {
namespace {
constexpr auto CHEAT_ENGINE_NS = std::chrono::nanoseconds{1000000000 / 12};

std::string_view ExtractName(std::size_t& out_name_size, std::string_view data,
                             std::size_t start_index, char match) {
    auto end_index = start_index;
    while (data[end_index] != match) {
        ++end_index;
        if (end_index > data.size()) {
            return {};
        }
    }

    out_name_size = end_index - start_index;

    // Clamp name if it's too big
    if (out_name_size > sizeof(CheatDefinition::readable_name)) {
        end_index = start_index + sizeof(CheatDefinition::readable_name);
    }

    return data.substr(start_index, end_index - start_index);
}
} // Anonymous namespace

StandardVmCallbacks::StandardVmCallbacks(System& system_, const CheatProcessMetadata& metadata_)
    : metadata{metadata_}, system{system_} {}

StandardVmCallbacks::~StandardVmCallbacks() = default;

Kernel::KProcess* StandardVmCallbacks::GetProcess() const {
    if (cached_process != nullptr && cached_process_id == metadata.process_id) {
        return cached_process;
    }

    auto process = system.Kernel().GetProcessByProcessId(metadata.process_id);
    cached_process = process.IsNull() ? nullptr : process.GetPointerUnsafe();
    cached_process_id = metadata.process_id;
    return cached_process;
}

void StandardVmCallbacks::MemoryReadUnsafe(VAddr address, void* data, u64 size) {
    auto* const process = this->GetProcess();

    // Return zero on invalid address
    if (process == nullptr || !IsAddressInRange(address) ||
        !process->GetMemory().IsValidVirtualAddress(address)) {
        std::memset(data, 0, size);
        return;
    }

    process->GetMemory().ReadBlock(address, data, size);
}

void StandardVmCallbacks::MemoryWriteUnsafe(VAddr address, const void* data, u64 size) {
    auto* const process = this->GetProcess();

    // Skip invalid memory write address
    if (process == nullptr || !IsAddressInRange(address) ||
        !process->GetMemory().IsValidVirtualAddress(address)) {
        return;
    }

    if (process->GetMemory().WriteBlock(address, data, size)) {
        Core::InvalidateInstructionCacheRange(process, address, size);
    }
}

u64 StandardVmCallbacks::HidKeysDown() {
    const auto hid = system.ServiceManager().GetService<Service::HID::IHidServer>("hid");
    if (hid == nullptr) {
        LOG_WARNING(CheatEngine, "Attempted to read input state, but hid is not initialized!");
        return 0;
    }

    const auto applet_resource = hid->GetResourceManager();
    if (applet_resource == nullptr || applet_resource->GetNpad() == nullptr) {
        LOG_WARNING(CheatEngine,
                    "Attempted to read input state, but applet resource is not initialized!");
        return 0;
    }

    const auto press_state = applet_resource->GetNpad()->GetAndResetPressState();
    return static_cast<u64>(press_state & HID::NpadButton::All);
}

void StandardVmCallbacks::PauseProcess() {
    auto* const process = this->GetProcess();
    if (process != nullptr && !process->IsSuspended()) {
        process->SetActivity(system.Kernel(), Kernel::Svc::ProcessActivity::Paused);
    }
}

void StandardVmCallbacks::ResumeProcess() {
    auto* const process = this->GetProcess();
    if (process != nullptr && process->IsSuspended()) {
        process->SetActivity(system.Kernel(), Kernel::Svc::ProcessActivity::Runnable);
    }
}

void StandardVmCallbacks::DebugLog(u8 id, u64 value) {
    LOG_INFO(CheatEngine, "Cheat triggered DebugLog: ID '{:01X}' Value '{:016X}'", id, value);
}

void StandardVmCallbacks::CommandLog(std::string_view data) {
    LOG_DEBUG(CheatEngine, "[DmntCheatVm]: {}",
              data.back() == '\n' ? data.substr(0, data.size() - 1) : data);
}

bool StandardVmCallbacks::IsAddressInRange(VAddr in) const {
    if ((in < metadata.main_nso_extents.base ||
         in >= metadata.main_nso_extents.base + metadata.main_nso_extents.size) &&
        (in < metadata.heap_extents.base ||
         in >= metadata.heap_extents.base + metadata.heap_extents.size) &&
        (in < metadata.alias_extents.base ||
         in >= metadata.alias_extents.base + metadata.alias_extents.size) &&
        (in < metadata.aslr_extents.base ||
         in >= metadata.aslr_extents.base + metadata.aslr_extents.size)) {
        LOG_DEBUG(CheatEngine,
                  "Cheat attempting to access memory at invalid address={:016X}, if this "
                  "persists, "
                  "the cheat may be incorrect. However, this may be normal early in execution if "
                  "the game has not properly set up yet.",
                  in);
        return false; ///< Invalid addresses will hard crash
    }

    return true;
}

CheatParser::~CheatParser() = default;

TextCheatParser::~TextCheatParser() = default;

std::vector<CheatEntry> TextCheatParser::Parse(std::string_view data) const {
    std::vector<CheatEntry> out(1);
    std::optional<u64> current_entry;

    for (std::size_t i = 0; i < data.size(); ++i) {
        if (::isspace(data[i])) {
            continue;
        }

        if (data[i] == '{') {
            current_entry = 0;

            if (out[*current_entry].definition.num_opcodes > 0) {
                return {};
            }

            std::size_t name_size{};
            const auto name = ExtractName(name_size, data, i + 1, '}');
            if (name.empty()) {
                return {};
            }

            std::memcpy(out[*current_entry].definition.readable_name.data(), name.data(),
                        std::min<std::size_t>(out[*current_entry].definition.readable_name.size(),
                                              name.size()));
            out[*current_entry]
                .definition.readable_name[out[*current_entry].definition.readable_name.size() - 1] =
                '\0';

            i += name_size + 1;
        } else if (data[i] == '[') {
            current_entry = out.size();
            out.emplace_back();

            std::size_t name_size{};
            const auto name = ExtractName(name_size, data, i + 1, ']');
            if (name.empty()) {
                return {};
            }

            std::memcpy(out[*current_entry].definition.readable_name.data(), name.data(),
                        std::min<std::size_t>(out[*current_entry].definition.readable_name.size(),
                                              name.size()));
            out[*current_entry]
                .definition.readable_name[out[*current_entry].definition.readable_name.size() - 1] =
                '\0';

            i += name_size + 1;
        } else if (::isxdigit(data[i])) {
            if (!current_entry || out[*current_entry].definition.num_opcodes >=
                                      out[*current_entry].definition.opcodes.size()) {
                return {};
            }

            const auto hex = std::string(data.substr(i, 8));
            if (!std::all_of(hex.begin(), hex.end(), ::isxdigit)) {
                return {};
            }

            const auto value = static_cast<u32>(std::strtoul(hex.c_str(), nullptr, 0x10));
            out[*current_entry].definition.opcodes[out[*current_entry].definition.num_opcodes++] =
                value;

            i += 8;
        } else {
            return {};
        }
    }

    out[0].enabled = out[0].definition.num_opcodes > 0;
    out[0].cheat_id = 0;

    for (u32 i = 1; i < out.size(); ++i) {
        out[i].enabled = out[i].definition.num_opcodes > 0;
        out[i].cheat_id = i;
    }

    return out;
}

CheatEngine::CheatEngine(System& system_, std::vector<CheatEntry> cheats_,
                         const std::array<u8, 0x20>& build_id_)
    : vm{std::make_unique<StandardVmCallbacks>(system_, metadata)},
      cheats(std::move(cheats_)), core_timing{system_.CoreTiming()}, system{system_} {
    metadata.main_nso_build_id = build_id_;
}

CheatEngine::~CheatEngine() {
    if (event)
        core_timing.UnscheduleEvent(event);
    else
        LOG_ERROR(CheatEngine, "~CheatEngine before event was registered");
}

void CheatEngine::Initialize() {
    event = Core::Timing::CreateEvent(
        "CheatEngine::FrameCallback::" + Common::HexToString(metadata.main_nso_build_id),
        [this](s64 time, std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
            FrameCallback(ns_late);
            return std::nullopt;
        });
    core_timing.ScheduleLoopingEvent(CHEAT_ENGINE_NS, CHEAT_ENGINE_NS, event);

    metadata.process_id = system.ApplicationProcess()->GetProcessId();
    metadata.title_id = system.GetApplicationProcessProgramID();

    const auto& page_table = system.ApplicationProcess()->GetPageTable();
    metadata.heap_extents = {
        .base = GetInteger(page_table.GetHeapRegionStart()),
        .size = page_table.GetHeapRegionSize(),
    };
    metadata.aslr_extents = {
        .base = GetInteger(page_table.GetAliasCodeRegionStart()),
        .size = page_table.GetAliasCodeRegionSize(),
    };
    metadata.alias_extents = {
        .base = GetInteger(page_table.GetAliasRegionStart()),
        .size = page_table.GetAliasRegionSize(),
    };

    is_pending_reload.exchange(true);
}

void CheatEngine::SetMainMemoryParameters(VAddr main_region_begin, u64 main_region_size) {
    metadata.main_nso_extents = {
        .base = main_region_begin,
        .size = main_region_size,
    };
}

void CheatEngine::Reload(std::vector<CheatEntry> reload_cheats) {
    cheats = std::move(reload_cheats);
    is_pending_reload.exchange(true);
}

void CheatEngine::FrameCallback(std::chrono::nanoseconds ns_late) {
    if (is_pending_reload.exchange(false)) {
        vm.LoadProgram(cheats);
    }

    // The governor decides from the field as the game left it, before a cheat overwrites it, and
    // puts its value back after the cheats so that is what the game reads.
    GovernFortunesWeaveFrameRate();
    if (vm.GetProgramSize() != 0) {
        vm.Execute(metadata);
        ReapplyFortunesWeaveFrameRate();
    }
}

void CheatEngine::ReapplyFortunesWeaveFrameRate() {
    if (governor.mode <= 0 || governor.field == 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    if (memory.IsValidVirtualAddress(governor.field) && memory.Read32(governor.field) != governor.target) {
        memory.Write32(governor.field, governor.target);
    }
}

/// Fortune's Weave (main DB93B0125F485060, v131072) steps its logic by 60 / target, where target is
/// +0x1dc of the graphics object at [main + 0x21e60d8] (the 60fps cheat writes 60 there). With 60
/// asked and fewer frames delivered, the game runs slow; with 30 it never shows more than 30.
/// The governor asks for 60 while the emulator delivers it and for 30 when it falls short, and
/// from 30 tries 60 again now and then (and whenever the game itself rewrites the field, i.e. on
/// a scene change); a failed try backs off up to a minute. It runs after the cheats, so its value
/// is the one the game reads. EDEN_FE_FPS=adaptive (default when a cheat is loaded for this
/// build), 60, 30 or off (leave the field to the game and the cheats).
void CheatEngine::GovernFortunesWeaveFrameRate() {
    using namespace std::chrono;
    constexpr u64 graphics_pointer = 0x21e60d8;
    constexpr u64 target_field = 0x1dc;
    auto& g = governor;
    if (g.mode == -1) {
        static constexpr std::array<u8, 8> build{0xDB, 0x93, 0xB0, 0x12, 0x5F, 0x48, 0x50, 0x60};
        const bool this_game = std::equal(build.begin(), build.end(),
                                          metadata.main_nso_build_id.begin());
        const char* const env = std::getenv("EDEN_FE_FPS");
        const std::string_view value = env != nullptr ? env : "adaptive";
        g.mode = !this_game || value == "off" ? 0 : value == "60" ? 60 : value == "30" ? 30 : 1;
        if (g.mode != 0) {
            LOG_INFO(CheatEngine, "Fortune's Weave frame-rate governor: {}",
                     g.mode == 1 ? "adaptive" : value);
        }
    }
    if (g.mode == 0 || metadata.main_nso_extents.size == 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const VAddr pointer_address = metadata.main_nso_extents.base + graphics_pointer;
    if (!memory.IsValidVirtualAddress(pointer_address)) {
        return;
    }
    const u64 graphics = memory.Read64(pointer_address);
    if (graphics == 0 || !memory.IsValidVirtualAddress(graphics + target_field)) {
        return;
    }
    const VAddr field = graphics + target_field;
    g.field = field;
    if (g.mode != 1) {
        g.target = static_cast<u32>(g.mode);
        memory.Write32(field, g.target);
        return;
    }

    const auto now = steady_clock::now();
    const u64 frames = system.GetPerfStats().GetTotalGameFrames();
    const u32 current = memory.Read32(field);
    if (g.last_written != 0 && current != g.last_written && g.target == 30) {
        // The game rewrote the field (a new scene): try 60 there right away.
        g.next_probe = now;
        g.backoff = seconds{10};
    }
    if (g.window_start == steady_clock::time_point{}) {
        g.window_start = now;
        g.window_frames = frames;
    }
    const auto window = now - g.window_start;
    if (window >= milliseconds{500}) {
        const double fps = static_cast<double>(frames - g.window_frames) /
                           duration<double>(window).count();
        g.window_start = now;
        g.window_frames = frames;
        if (g.target == 60) {
            if (g.probing && g.probe_windows++ == 0) {
                // The first window after asking for 60 still holds frames paced for 30.
            } else if (g.probing) {
                g.probing = false;
                if (fps < 55.0) {
                    g.target = 30;
                    g.next_probe = now + g.backoff;
                    g.backoff = std::min<seconds>(g.backoff * 2, seconds{60});
                    LOG_INFO(CheatEngine, "FE governor: 60 not reached ({:.1f} fps), 30", fps);
                } else {
                    g.backoff = seconds{10};
                    LOG_INFO(CheatEngine, "FE governor: 60 holds ({:.1f} fps)", fps);
                }
            } else if (fps < 52.0) {
                if (++g.low_windows >= 2) {
                    g.target = 30;
                    g.low_windows = 0;
                    g.next_probe = now + g.backoff;
                    LOG_INFO(CheatEngine, "FE governor: {:.1f} fps, 30", fps);
                }
            } else {
                g.low_windows = 0;
            }
        } else if (now >= g.next_probe) {
            g.target = 60;
            g.probing = true;
            g.probe_windows = 0;
            g.low_windows = 0;
        }
    }
    if (current != g.target) {
        memory.Write32(field, g.target);
    }
    g.last_written = g.target;
}

} // namespace Core::Memory
