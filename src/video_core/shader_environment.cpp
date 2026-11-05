// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <utility>

#include "common/assert.h"
#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include <ranges>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/native.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/shader_environment.h"
#include "video_core/texture_cache/format_lookup_table.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

constexpr std::array<char, 8> MAGIC_NUMBER{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};

constexpr size_t INST_SIZE = sizeof(u64);

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

static u64 MakeCbufKey(u32 index, u32 offset) {
    return (static_cast<u64>(index) << 32) | offset;
}

static Shader::TextureType ConvertTextureType(const Tegra::Texture::TICEntry& entry) {
    switch (entry.texture_type) {
    case Tegra::Texture::TextureType::Texture1D:
        return Shader::TextureType::Color1D;
    case Tegra::Texture::TextureType::Texture2D:
    case Tegra::Texture::TextureType::Texture2DNoMipmap:
        return entry.normalized_coords ? Shader::TextureType::Color2D
                                       : Shader::TextureType::Color2DRect;
    case Tegra::Texture::TextureType::Texture3D:
        return Shader::TextureType::Color3D;
    case Tegra::Texture::TextureType::TextureCubemap:
        return Shader::TextureType::ColorCube;
    case Tegra::Texture::TextureType::Texture1DArray:
        return Shader::TextureType::ColorArray1D;
    case Tegra::Texture::TextureType::Texture2DArray:
        return Shader::TextureType::ColorArray2D;
    case Tegra::Texture::TextureType::Texture1DBuffer:
        return Shader::TextureType::Buffer;
    case Tegra::Texture::TextureType::TextureCubeArray:
        return Shader::TextureType::ColorArrayCube;
    default:
        LOG_ERROR(Shader, "Invalid texture_type={}. Falling back to texture_type={}", static_cast<int>(entry.texture_type.Value()), Shader::TextureType::Color2D);
        return Shader::TextureType::Color2D;
    }
}

static Shader::TexturePixelFormat ConvertTexturePixelFormat(const Tegra::Texture::TICEntry& entry) {
    return static_cast<Shader::TexturePixelFormat>(
        PixelFormatFromTextureInfo(entry.format, entry.r_type, entry.g_type, entry.b_type,
                                   entry.a_type, entry.srgb_conversion));
}

static std::string_view StageToPrefix(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexB:
        return "vs";
    case Shader::Stage::TessellationControl:
        return "tc";
    case Shader::Stage::TessellationEval:
        return "te";
    case Shader::Stage::Geometry:
        return "gs";
    case Shader::Stage::Fragment:
        return "fs";
    case Shader::Stage::Compute:
        return "cs";
    case Shader::Stage::VertexA:
        return "va";
    default:
        return "uk";
    }
}

static void DumpImpl(u64 /*pipeline_hash*/, u64 shader_hash, std::span<const u64> code,
                     [[maybe_unused]] u32 read_highest, [[maybe_unused]] u32 read_lowest,
                     u32 initial_offset, Shader::Stage stage) {
    const auto dump_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir)};
    if (!Common::FS::CreateDir(dump_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create dump directory");
        return;
    }
    const auto prefix = StageToPrefix(stage);
    const auto name{dump_dir /
                    fmt::format("{:016x}_{:016x}_{}.ash",
                                Settings::GetCurrentProgramID(), shader_hash, prefix)};
    std::fstream shader_file(name, std::ios::out | std::ios::binary);
    ASSERT(initial_offset % sizeof(u64) == 0);
    const size_t jump_index = initial_offset / sizeof(u64);
    const size_t code_size = code.size_bytes() - initial_offset;
    shader_file.write(reinterpret_cast<const char*>(&code[jump_index]), code_size);

    // + 1 instruction, due to the fact that we skip the final self branch instruction in the code,
    // but we need to consider it for padding, otherwise nvdisasm rages.
    const size_t padding_needed = (32 - ((code_size + INST_SIZE) % 32)) % 32;
    for (size_t i = 0; i < INST_SIZE + padding_needed; i++) {
        shader_file.put(0);
    }
}

GenericEnvironment::GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_)
    : gpu_memory{&gpu_memory_}, program_base{program_base_} {
    start_address = start_address_;
}

GenericEnvironment::~GenericEnvironment() = default;

u32 GenericEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

u32 GenericEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 GenericEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

std::array<u32, 3> GenericEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

u64 GenericEnvironment::ReadInstruction(u32 address) {
    read_lowest = (std::min)(read_lowest, address);
    read_highest = (std::max)(read_highest, address);

    if (address >= cached_lowest && address < cached_highest) {
        return code[(address - cached_lowest) / INST_SIZE];
    }
    if (native_source_size) {
        throw Shader::RuntimeError("Lowered native instruction address out of range: {:#x}", address);
    }
    has_unbound_instructions = true;
    return gpu_memory->Read<u64>(program_base + address);
}

bool GenericEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    const u64 key = MakeCbufKey(index, offset);
    if (const auto it = storage_pointer_types.find(key); it != storage_pointer_types.end()) {
        return it->second != 0;
    }
    const u64 address = u64{ReadCbufValue(index, offset)} |
                        (u64{ReadCbufValue(index, offset + 4)} << 32);
    const bool mapped = address != 0 && gpu_memory->IsWithinGPUAddressRange(address) &&
                        gpu_memory->IsFullyMappedRange(address, 1);
    storage_pointer_types.emplace(key, static_cast<u8>(mapped));
    return mapped;
}

bool GenericEnvironment::HasNativeRegisterCopies() const {
    return native_source_size || Shader::Ampere::HasNativeAddressMap(code);
}

u32 GenericEnvironment::TranslateProgramAddress(u32 address) const {
    if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) return *translated;
    if (native_source_size || Shader::Ampere::HasNativeAddressMap(code))
        throw Shader::RuntimeError("Invalid native indirect branch address {:#x}", address);
    return address;
}

std::optional<u64> GenericEnvironment::Analyze() {
    if (!ampere_program.empty()) {
        // Native programs are keyed by their raw instruction bytes and the translator version.
        return Common::CityHash64(reinterpret_cast<const char*>(ampere_program.data()),
                                  ampere_program.size() * sizeof(Shader::Ampere::Instruction)) ^
               Shader::Ampere::NATIVE_TRANSLATOR_SALT;
    }
    if (native_source_size) {
        // Include the converted header and a version tag to isolate this experimental ABI.
        return Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                                  code.size() * sizeof(u64)) ^ 0x4f554e4345000002ULL;
    }
    const std::optional<u64> size{TryFindSize()};
    if (!size) {
        return std::nullopt;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(*size);
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()), *size);
}

void GenericEnvironment::SetCachedSize(size_t size_bytes) {
    if (native_source_size) {
        if (size_bytes != CachedSizeBytes()) {
            throw Shader::RuntimeError("Native shader changed while constructing pipeline");
        }
        return;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(size_bytes);
    code.resize(CachedSizeWords());
    gpu_memory->ReadBlock(program_base + cached_lowest, code.data(), code.size() * sizeof(u64));
}

size_t GenericEnvironment::CachedSizeWords() const noexcept {
    return CachedSizeBytes() / INST_SIZE;
}

size_t GenericEnvironment::CachedSizeBytes() const noexcept {
    return static_cast<size_t>(cached_highest) - cached_lowest + INST_SIZE;
}

size_t GenericEnvironment::ReadSizeBytes() const noexcept {
    return read_highest - read_lowest + INST_SIZE;
}

bool GenericEnvironment::CanBeSerialized() const noexcept {
    // Natively translated programs are stored as their Ampere instructions plus the tables they were built from.
    return !has_unbound_instructions;
}

u64 GenericEnvironment::CalculateHash() const {
    const size_t size{ReadSizeBytes()};
    const auto data{std::make_unique<char[]>(size)};
    gpu_memory->ReadBlock(program_base + read_lowest, data.get(), size);
    return Common::CityHash64(data.get(), size);
}

void GenericEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

void GenericEnvironment::Serialize(std::ofstream& file) const {
    const u64 code_size{static_cast<u64>(CachedSizeBytes())};
    const u64 num_texture_types{static_cast<u64>(texture_types.size())};
    const u64 num_texture_pixel_formats{static_cast<u64>(texture_pixel_formats.size())};
    const u64 num_cbuf_values{static_cast<u64>(cbuf_values.size())};
    const u64 num_storage_pointer_types{static_cast<u64>(storage_pointer_types.size())};
    const u64 num_cbuf_replacement_values{static_cast<u64>(cbuf_replacements.size())};

    file.write(reinterpret_cast<const char*>(&code_size), sizeof(code_size))
        .write(reinterpret_cast<const char*>(&num_texture_types), sizeof(num_texture_types))
        .write(reinterpret_cast<const char*>(&num_texture_pixel_formats),
               sizeof(num_texture_pixel_formats))
        .write(reinterpret_cast<const char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .write(reinterpret_cast<const char*>(&num_storage_pointer_types), sizeof(num_storage_pointer_types))
        .write(reinterpret_cast<const char*>(&num_cbuf_replacement_values),
               sizeof(num_cbuf_replacement_values))
        .write(reinterpret_cast<const char*>(&local_memory_size), sizeof(local_memory_size))
        .write(reinterpret_cast<const char*>(&texture_bound), sizeof(texture_bound))
        .write(reinterpret_cast<const char*>(&start_address), sizeof(start_address))
        .write(reinterpret_cast<const char*>(&cached_lowest), sizeof(cached_lowest))
        .write(reinterpret_cast<const char*>(&cached_highest), sizeof(cached_highest))
        .write(reinterpret_cast<const char*>(&viewport_transform_state),
               sizeof(viewport_transform_state))
        .write(reinterpret_cast<const char*>(&stage), sizeof(stage))
        .write(reinterpret_cast<const char*>(code.data()), code_size);
    for (const auto& [key, type] : texture_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, format] : texture_pixel_formats) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&format), sizeof(format));
    }
    for (const auto& [key, type] : cbuf_values) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : storage_pointer_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : cbuf_replacements) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    if (stage == Shader::Stage::Compute) {
        file.write(reinterpret_cast<const char*>(&workgroup_size), sizeof(workgroup_size))
            .write(reinterpret_cast<const char*>(&shared_memory_size), sizeof(shared_memory_size));
    } else {
        file.write(reinterpret_cast<const char*>(&sph), sizeof(sph));
        if (stage == Shader::Stage::Geometry) {
            file.write(reinterpret_cast<const char*>(&gp_passthrough_mask),
                       sizeof(gp_passthrough_mask));
        }
    }
    // Native block: salt of the translator that accepted the program, the program, and the BRX tables.
    const u64 native_count{static_cast<u64>(ampere_program.size())};
    file.write(reinterpret_cast<const char*>(&native_count), sizeof(native_count));
    if (native_count) {
        const u64 salt{Shader::Ampere::NATIVE_TRANSLATOR_SALT};
        const u64 num_targets{static_cast<u64>(ampere_indirect_targets.size())};
        const u64 num_tables{static_cast<u64>(native_indirect_tables.size())};
        file.write(reinterpret_cast<const char*>(&salt), sizeof(salt))
            .write(reinterpret_cast<const char*>(&native_source_header_size), sizeof(native_source_header_size))
            .write(reinterpret_cast<const char*>(ampere_program.data()),
                   native_count * sizeof(Shader::Ampere::Instruction))
            .write(reinterpret_cast<const char*>(&num_targets), sizeof(num_targets))
            .write(reinterpret_cast<const char*>(ampere_indirect_targets.data()), num_targets * sizeof(u32))
            .write(reinterpret_cast<const char*>(&num_tables), sizeof(num_tables));
        for (const auto& [branch, targets] : native_indirect_tables) {
            const u64 count{static_cast<u64>(targets.size())};
            file.write(reinterpret_cast<const char*>(&branch), sizeof(branch))
                .write(reinterpret_cast<const char*>(&count), sizeof(count))
                .write(reinterpret_cast<const char*>(targets.data()), count * sizeof(u32));
        }
    }
}

std::optional<u64> GenericEnvironment::TryFindSize() {
    static constexpr size_t BLOCK_SIZE = 0x1000;
    static constexpr size_t MAXIMUM_SIZE = 0x100000;

    static constexpr u64 SELF_BRANCH_A = 0xE2400FFFFF87000FULL;
    static constexpr u64 SELF_BRANCH_B = 0xE2400FFFFF07000FULL;

    static constexpr u64 EXIT_VALUE = 0xE30000000007000FULL;

    code.resize(MAXIMUM_SIZE / INST_SIZE);

    GPUVAddr guest_addr{program_base + start_address};
    size_t offset{0};
    size_t size{BLOCK_SIZE};
    while (size <= MAXIMUM_SIZE) {
        u64* const data = code.data() + offset / INST_SIZE;
        gpu_memory->ReadBlock(guest_addr, data, BLOCK_SIZE);
        for (size_t index = 0; index < BLOCK_SIZE; index += INST_SIZE) {
            const u64 inst = data[index / INST_SIZE];
            if (inst == SELF_BRANCH_A || inst == SELF_BRANCH_B) {
                return offset + index;
            }
            if (!is_proprietary_driver && inst == EXIT_VALUE) {
                return offset + index + INST_SIZE;
            }
        }
        guest_addr += BLOCK_SIZE;
        size += BLOCK_SIZE;
        offset += BLOCK_SIZE;
    }
    return std::nullopt;
}

Tegra::Texture::TICEntry GenericEnvironment::ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                                             bool via_header_index, u32 raw) {
    const auto handle{Tegra::Texture::TexturePair(raw, via_header_index)};
    ASSERT_MSG(handle.first <= tic_limit,
               "Texture handle 0x{:08x}: index {}, limit {}, table 0x{:x}, header binding {}",
               raw, handle.first, tic_limit, tic_addr, via_header_index);
    const GPUVAddr descriptor_addr{tic_addr + handle.first * sizeof(Tegra::Texture::TICEntry)};
    Tegra::Texture::TICEntry entry;
    gpu_memory->ReadBlock(descriptor_addr, &entry, sizeof(entry));
    return entry;
}

namespace {

// Reads a native program a 4 KiB page at a time instead of one 16-byte instruction at a time.
// The reads do not flush the GPU caches first (ReadBlockUnsafe): the previous per-instruction
// ReadBlock did, once per instruction. An instruction that is not inside a fully mapped page is
// left to the caller.
class NativeInstructionReader {
public:
    using Instruction = Shader::Ampere::Instruction;
    static constexpr GPUVAddr PageSize = 0x1000;

    explicit NativeInstructionReader(Tegra::MemoryManager& gpu_memory_) : gpu_memory{gpu_memory_} {}

    /// Copies the instruction at `address` into `out`, loading its page first if needed.
    /// Returns false when the instruction is not inside a fully mapped page.
    bool TryRead(GPUVAddr address, Instruction& out) {
        if (!Holds(address)) {
            LoadFrom(address);
        }
        if (!Holds(address)) {
            return false;
        }
        out = page[(address - page_begin) / sizeof(Instruction)];
        return true;
    }

private:
    bool Holds(GPUVAddr address) const {
        return address >= page_begin && address + sizeof(Instruction) <= page_end;
    }

    // Loads from `address` up to the next page boundary; loads nothing if that range is not
    // fully mapped.
    void LoadFrom(GPUVAddr address) {
        const GPUVAddr next_page = (address | (PageSize - 1)) + 1;
        const GPUVAddr length = next_page - address;
        page_begin = address;
        page_end = address;
        if (length >= sizeof(Instruction) && gpu_memory.IsFullyMappedRange(address, length)) {
            gpu_memory.ReadBlockUnsafe(address, page.data(), length);
            page_end = next_page;
        }
    }

    Tegra::MemoryManager& gpu_memory;
    std::array<Instruction, PageSize / sizeof(Instruction)> page;
    GPUVAddr page_begin{};
    GPUVAddr page_end{};
};

} // Anonymous namespace

// A native program is captured and lowered once, when the shader cache first meets it. The
// environments built later for its pipelines receive that result instead of lowering again.
void GenericEnvironment::InitializeNativeCode(std::shared_ptr<const LoweredNativeCode> lowered,
                                              u32 source_header_size,
                                              const Shader::Ampere::TrapGuardReader& guard_reader) {
    if (!lowered) {
        lowered = LowerNativeCode(source_header_size, guard_reader);
    }
    if (lowered->texture_bound != ~0U) {
        // Native TEX carries its cbuf index explicitly; TEXS obtains it from the environment.
        texture_bound = lowered->texture_bound;
    }
    native_source_size = lowered->source_size;
    native_source_header_size = lowered->source_header_size;
    ampere_program = lowered->ampere_program;
    ampere_indirect_targets = lowered->ampere_indirect_targets;
    native_indirect_tables = lowered->native_indirect_tables;
    native_trap_prefix = lowered->native_trap_prefix;
    code = lowered->code;
    cached_lowest = 0;
    cached_highest = static_cast<u32>((code.size() - 1) * sizeof(u64));
    lowered_native = std::move(lowered);
}

std::shared_ptr<const LoweredNativeCode> GenericEnvironment::LowerNativeCode(
    u32 source_header_size, const Shader::Ampere::TrapGuardReader& guard_reader) {
    using Shader::Ampere::Instruction;
    const u32 native_header_size = source_header_size;
    native_source_header_size = source_header_size;
    constexpr u32 maximum_instructions = Shader::Ampere::ProgramCaptureState::MaximumInstructions;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    NativeInstructionReader reader{*gpu_memory};
    for (u32 index = 0; index < maximum_instructions; ++index) {
        const GPUVAddr address = program_base + native_header_size + index * sizeof(Instruction);
        Instruction instruction;
        if (!reader.TryRead(address, instruction)) {
            // Not inside a fully mapped page: check and read this one instruction on its own.
            if (!gpu_memory->IsFullyMappedRange(address, sizeof(Instruction))) {
                throw Shader::RuntimeError("Unmapped native shader instruction at {:#x}", address);
            }
            gpu_memory->ReadBlockUnsafe(address, &instruction, sizeof(instruction));
        }
        native.push_back(instruction);
        const auto status = capture.Observe(instruction, index);
        if (status == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            throw Shader::RuntimeError("Native shader branch outside capture bounds");
        }
        if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    // BRX table targets can lie past the first EXIT; capture the tail blocks they select and keep each BRX's table.
    {
        const auto tables = Shader::Ampere::ResolveCaptureIndirectBranches(
            native, capture, native_header_size,
            [&](u32 index) -> std::optional<Instruction> {
                const GPUVAddr address = program_base + native_header_size + index * sizeof(Instruction);
                if (!gpu_memory->IsFullyMappedRange(address, sizeof(Instruction))) return std::nullopt;
                Instruction instruction;
                gpu_memory->ReadBlock(address, &instruction, sizeof(instruction));
                return instruction;
            },
            [&](u32 bank, u32 offset) { return ReadCbufValue(bank, offset); });
        native_indirect_tables.clear();
        static std::atomic<u32> logged{};
        for (const auto& table : tables.tables) {
            native_indirect_tables.emplace_back(table.branch, table.targets);
            if (logged.fetch_add(1) < 400) {
                std::string values;
                for (const u32 value : table.values) values += fmt::format(" {:#x}", value);
                LOG_INFO(Shader,
                         "native BRX at instruction {} (stage index {}): table words{}, base {:#x}, {} targets, capture {}{}",
                         table.branch, stage_index, values, tables.base, table.targets.size(), native.size(),
                         tables.extended ? " (extended)" : "");
            }
        }
    }
    std::vector<u32> verified_traps;
    for (u32 index = 0; index < native.size(); ++index) {
        if (native[index].Bits(0, 12) == 0x95c) verified_traps.push_back(index * sizeof(Instruction));
    }
    if (!verified_traps.empty()) {
        const size_t count = verified_traps.back() / sizeof(Instruction) + 1;
        const Shader::Ampere::TrapGuardReader read = [&](u32 bank, u32 offset) -> std::optional<u32> {
            return guard_reader ? guard_reader(bank, offset) : std::nullopt;
        };
        if (stage != Shader::Stage::Compute || count > 256 ||
            !Shader::Ampere::VerifyTrapGuards(std::span{native}.first(count), read)) {
            throw Shader::RuntimeError("Native compute trap is reachable or its uniform guard is unknown");
        }
        native_trap_prefix.assign(native.begin(), native.begin() + count);
    }
    const u64 native_program_key = Shader::Ampere::NativeProgramKey(native);
    // EDEN_NATIVE_FRONTEND selects, per program and before any IR exists, whether the native Ampere
    // frontend translates it. The default (unset or "lower") leaves the lowering path untouched.
    if (const auto frontend_mode = Shader::Ampere::SelectedFrontendMode();
        frontend_mode != Shader::Ampere::FrontendMode::Lower) {
        std::string reject;
        u32 reject_index = 0;
        Shader::Ampere::ProgramInfo program_info;
        try {
            program_info = Shader::Ampere::ValidateProgram(*this, native);
        } catch (const Shader::Ampere::UnsupportedError& e) {
            reject = e.what();
            reject_index = e.index;
        }
        // Diagnostic (auto mode): EDEN_NATIVE_AMPERE_KEYS limits the native frontend to the listed
        // program keys and "stageN" tokens; every other program falls back to lowering.
        if (reject.empty() && frontend_mode == Shader::Ampere::FrontendMode::Compare) {
            LOG_INFO(Shader, "ampere frontend: key={:016x} supported (compare mode keeps the lowering)",
                     native_program_key);
        } else if (reject.empty()) {
            LOG_INFO(Shader, "ampere frontend: key={:016x} native, {} instructions, stage {}",
                     native_program_key, native.size(), static_cast<u32>(stage));
            auto result = std::make_shared<LoweredNativeCode>();
            result->ampere_program = std::move(native);
            result->texture_bound = program_info.texture_buffer;
            result->source_size =
                native_header_size + result->ampere_program.size() * sizeof(Instruction);
            result->source_header_size = native_header_size;
            result->native_indirect_tables = std::move(native_indirect_tables);
            result->native_trap_prefix = std::move(native_trap_prefix);
            result->code.resize(sizeof(sph) / sizeof(u64));
            std::memcpy(result->code.data(), &sph, sizeof(sph));
            return result;
        } else if (frontend_mode == Shader::Ampere::FrontendMode::Ampere) {
            throw Shader::RuntimeError("Native Ampere frontend: key={:016x}, instruction {:#x}: {}",
                                       native_program_key, reject_index * sizeof(Instruction), reject);
        } else {
            LOG_WARNING(Shader, "ampere frontend: key={:016x} falls back to lowering at instruction {:#x}: {}",
                        native_program_key, reject_index * sizeof(Instruction), reject);
        }
    }
    auto lowered = Shader::Ampere::LowerToMaxwell(native, verified_traps, true);
    if (!lowered) {
        if (lowered.error_offset / sizeof(Instruction) < native.size()) {
            const auto& words = native[lowered.error_offset / sizeof(Instruction)].words;
            throw Shader::RuntimeError("Native shader at {:#x}, instruction {:#x}: {} [{:08x} {:08x} {:08x} {:08x}]",
                                       program_base, lowered.error_offset, lowered.error,
                                       words[0], words[1], words[2], words[3]);
        }
        throw Shader::RuntimeError("Native shader at {:#x}, instruction {:#x}: {}",
                                   program_base, lowered.error_offset, lowered.error);
    }
    auto result = std::make_shared<LoweredNativeCode>();
    result->texture_bound = lowered.required_texture_buffer;
    result->source_size = native_header_size + lowered.source_size;
    result->source_header_size = native_header_size;
    result->native_indirect_tables = std::move(native_indirect_tables);
    result->native_trap_prefix = std::move(native_trap_prefix);
    auto& lowered_code = result->code;
    lowered_code.resize(sizeof(sph) / sizeof(u64));
    std::memcpy(lowered_code.data(), &sph, sizeof(sph));
    for (const u64 instruction : lowered.instructions) {
        if ((lowered_code.size() * sizeof(u64)) % 32 == 0) {
            lowered_code.push_back(0); // Maxwell scheduling word, absent from native instructions.
        }
        lowered_code.push_back(instruction);
    }
    // Keep the original padding ABI, then retain the native address map as
    // unreachable data. Existing disk-cache serialization carries it with code.
    lowered_code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(lowered_code, lowered.source_instruction_offsets);
    LOG_DEBUG(Shader, "Lowered native shader at {:#x}: {} source bytes, {} instructions",
              program_base, result->source_size, lowered.instructions.size());
    return result;
}

GraphicsEnvironment::GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                         Tegra::MemoryManager& gpu_memory_,
                                         Maxwell::ShaderType program, GPUVAddr program_base_,
                                         u32 start_address_,
                                         std::shared_ptr<const LoweredNativeCode> lowered_native_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, maxwell3d{&maxwell3d_} {
    gpu_memory->ReadBlock(program_base + start_address, &sph, sizeof(sph));
    initial_offset = sizeof(sph);
    texture_bound = maxwell3d->regs.bindless_texture_const_buffer_slot;
    if (maxwell3d->GetNativeMacroEngine().native_turing) {
        if (program != Maxwell::ShaderType::VertexB && program != Maxwell::ShaderType::Pixel) {
            throw Shader::RuntimeError("Unsupported native graphics shader stage {}", static_cast<u32>(program));
        }
        stage = program == Maxwell::ShaderType::VertexB ? Shader::Stage::VertexB : Shader::Stage::Fragment;
        // The capture reads constant-buffer tables through ReadCbufValue, which indexes the stage's buffers.
        switch (program) {
        case Maxwell::ShaderType::TessellationInit:
            stage_index = 1;
            break;
        case Maxwell::ShaderType::Tessellation:
            stage_index = 2;
            break;
        case Maxwell::ShaderType::Geometry:
            stage_index = 3;
            break;
        default:
            stage_index = program == Maxwell::ShaderType::Pixel ? 4 : 0;
            break;
        }
        InitializeNativeCode(std::move(lowered_native_));
    }
    gp_passthrough_mask = maxwell3d->regs.post_vtg_shader_attrib_skip_mask;
    switch (program) {
    case Maxwell::ShaderType::VertexA:
        stage = Shader::Stage::VertexA;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::VertexB:
        stage = Shader::Stage::VertexB;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::TessellationInit:
        stage = Shader::Stage::TessellationControl;
        stage_index = 1;
        break;
    case Maxwell::ShaderType::Tessellation:
        stage = Shader::Stage::TessellationEval;
        stage_index = 2;
        break;
    case Maxwell::ShaderType::Geometry:
        stage = Shader::Stage::Geometry;
        stage_index = 3;
        break;
    case Maxwell::ShaderType::Pixel:
        stage = Shader::Stage::Fragment;
        stage_index = 4;
        break;
    default:
        ASSERT_MSG(false, "Invalid program={}", program);
        break;
    }
    const u64 local_size{sph.LocalMemorySize()};
    ASSERT(local_size <= (std::numeric_limits<u32>::max)());
    local_memory_size = static_cast<u32>(local_size) + sph.common3.shader_local_memory_crs_size;
    is_proprietary_driver = maxwell3d->GetNativeMacroEngine().native_turing || texture_bound == 2;
    has_hle_engine_state =
        maxwell3d->engine_state == Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro;
}

u32 GraphicsEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const bool native_turing = snapshot ? snapshot->native_turing
                                        : maxwell3d->GetNativeMacroEngine().native_turing;
    if (native_turing && Tegra::NativeRootTables::IsBank(cbuf_index)) {
        const auto value = snapshot ? snapshot->native_root_tables.Read(cbuf_index, cbuf_offset)
                                    : maxwell3d->native_root_tables.Read(cbuf_index, cbuf_offset);
        cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
        return value;
    }
    u32 value{};
    if (snapshot) {
        const auto& cbuf{snapshot->const_buffers[cbuf_index]};
        ASSERT(cbuf.enabled);
        if (cbuf_offset < cbuf.size) {
            std::memcpy(&value, cbuf.bytes.data() + cbuf_offset, sizeof(value));
        }
    } else {
        const auto& cbuf{maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index]};
        ASSERT(cbuf.enabled);
        if (cbuf_offset < cbuf.size) {
            value = gpu_memory->Read<u32>(cbuf.address + cbuf_offset);
        }
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

std::optional<Shader::ReplaceConstant> GraphicsEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                  u32 offset) {
    if (!has_hle_engine_state) {
        return std::nullopt;
    }
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    const auto& replace_table{snapshot ? snapshot->replace_table : maxwell3d->replace_table};
    auto it = replace_table.find(key);
    if (it == replace_table.end()) {
        return std::nullopt;
    }
    const auto converted_value = [](Tegra::Engines::Maxwell3D::HLEReplacementAttributeType name) {
        switch (name) {
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseVertex:
            return Shader::ReplaceConstant::BaseVertex;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseInstance:
            return Shader::ReplaceConstant::BaseInstance;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::DrawID:
            return Shader::ReplaceConstant::DrawID;
        default:
            UNREACHABLE();
        }
    }(it->second);
    cbuf_replacements.emplace(key, converted_value);
    return converted_value;
}

Shader::TextureType GraphicsEnvironment::ReadTextureType(u32 handle) {
    const auto entry{ReadTextureDescriptor(handle)};
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat GraphicsEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto entry{ReadTextureDescriptor(handle)};
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool GraphicsEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 GraphicsEnvironment::ReadViewportTransformState() {
    viewport_transform_state = snapshot ? snapshot->viewport_scale_offset_enabled
                                        : maxwell3d->regs.viewport_scale_offset_enabled;
    return viewport_transform_state;
}

std::optional<u32> ReadNativeGuardConstant(Tegra::Engines::KeplerCompute& compute,
                                          Tegra::MemoryManager& memory, u32 bank, u32 offset) {
    const auto& qmd = compute.launch_description;
    if (bank >= 8 || !(qmd.const_buffer_enable_mask.Value() & (1U << bank))) return std::nullopt;
    const auto& buffer = qmd.const_buffer_config[bank];
    if (offset > buffer.size || buffer.size - offset < sizeof(u32) ||
        !memory.IsFullyMappedRange(buffer.Address() + offset, sizeof(u32))) return std::nullopt;
    u32 value{};
    memory.ReadBlock(buffer.Address() + offset, &value, sizeof(value));
    return value;
}

Tegra::Texture::TICEntry GraphicsEnvironment::ReadTextureDescriptor(u32 handle) {
    if (!snapshot) {
        const auto& regs{maxwell3d->regs};
        const bool via_header_index{regs.sampler_binding ==
                                    Maxwell::SamplerBinding::ViaHeaderBinding};
        return ReadTextureInfo(regs.tex_header.Address(), regs.tex_header.limit,
                               via_header_index, handle);
    }
    const u32 index{Tegra::Texture::TexturePair(handle, snapshot->via_header_index).first};
    ASSERT(index <= snapshot->texture_pool_limit);
    Tegra::Texture::TICEntry entry{};
    if (index <= snapshot->texture_pool_limit) {
        // Only the pool's own range was checked to be contiguous: never read past it.
        snapshot->device_memory->ReadBlockUnsafe(snapshot->texture_pool + index * sizeof(entry),
                                                 &entry, sizeof(entry));
    }
    return entry;
}

// Runs on the GPU thread, before the environment is handed to a translation worker.
// - Constant buffers are copied whole: translation folds some of their values into the shader
//   (driver constants in bank 1, texture handles, indirect branch tables), and games reuse
//   uniform memory once the GPU is past the draw, so a later read could see another draw's data.
// - The texture pool is not copied, only located: its entries are read on the worker through
//   the device address, unsynchronized. An entry rewritten in the meantime gives the newer
//   texture type, an accepted risk (pools hold long-lived descriptors).
// - The viewport transform flag and the HLE macro replacements are copied.
bool GraphicsEnvironment::Detach(Tegra::MaxwellDeviceMemoryManager& device_memory) {
    // A texture handle indexes at most 2^20 descriptors; a larger limit is not a real pool and
    // would make the contiguity check below walk gigabytes of page table.
    constexpr u32 MaxTexturePoolLimit = (1U << 20) - 1;
    const auto& regs{maxwell3d->regs};
    if (regs.tex_header.limit > MaxTexturePoolLimit) {
        return false;
    }
    const GPUVAddr pool_address{regs.tex_header.Address()};
    const size_t pool_size{(size_t{regs.tex_header.limit} + 1) * sizeof(Tegra::Texture::TICEntry)};
    const std::optional<DAddr> pool_device_address{gpu_memory->GpuToCpuAddress(pool_address)};
    if (!pool_device_address || !gpu_memory->IsContinuousRange(pool_address, pool_size)) {
        return false;
    }
    EngineSnapshot copy;
    const auto& live_buffers{maxwell3d->state.shader_stages[stage_index].const_buffers};
    for (size_t index = 0; index < live_buffers.size(); ++index) {
        const auto& live{live_buffers[index]};
        auto& copied{copy.const_buffers[index]};
        copied.enabled = live.enabled;
        if (!live.enabled || live.size == 0) {
            continue;
        }
        // The size register can exceed what a constant buffer holds; never copy more than that.
        copied.size = (std::min)(live.size, static_cast<u32>(Maxwell::MaxConstBufferSize));
        copied.bytes.resize(copied.size + sizeof(u32));
        gpu_memory->ReadBlockUnsafe(live.address, copied.bytes.data(), copied.bytes.size());
    }
    copy.device_memory = &device_memory;
    copy.texture_pool = *pool_device_address;
    copy.texture_pool_limit = regs.tex_header.limit;
    copy.via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    copy.native_turing = maxwell3d->GetNativeMacroEngine().native_turing;
    copy.native_root_tables = maxwell3d->native_root_tables;
    copy.viewport_scale_offset_enabled = regs.viewport_scale_offset_enabled;
    if (has_hle_engine_state) {
        copy.replace_table = maxwell3d->replace_table;
    }
    snapshot = std::move(copy);
    maxwell3d = nullptr;
    return true;
}

size_t GraphicsEnvironment::SnapshotSizeBytes() const noexcept {
    if (!snapshot) {
        return 0;
    }
    size_t total{};
    for (const auto& cbuf : snapshot->const_buffers) {
        total += cbuf.bytes.size();
    }
    return total;
}

ComputeEnvironment::ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                       Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_,
                                       std::shared_ptr<const LoweredNativeCode> lowered_native_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_}, kepler_compute{
                                                                          &kepler_compute_} {
    const auto& qmd{kepler_compute->launch_description};
    stage = Shader::Stage::Compute;
    local_memory_size = qmd.local_pos_alloc + qmd.local_crs_alloc;
    texture_bound = kepler_compute->regs.tex_cb_index;
    is_proprietary_driver = texture_bound == 2;
    shared_memory_size = qmd.shared_alloc;
    workgroup_size = {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z};
    if (kepler_compute->IsNativeMode()) {
        initial_offset = sizeof(sph);
        is_proprietary_driver = true;
        InitializeNativeCode(std::move(lowered_native_), 0, [&](u32 bank, u32 offset) {
            return ReadNativeGuardConstant(*kepler_compute, *gpu_memory, bank, offset);
        });
        start_address = sizeof(sph);
    }
}

u32 ComputeEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto& qmd{kepler_compute->launch_description};
    ASSERT(((qmd.const_buffer_enable_mask.Value() >> cbuf_index) & 1) != 0);
    const auto& cbuf{qmd.const_buffer_config[cbuf_index]};
    u32 value{};
    if (cbuf_offset < cbuf.size) {
        value = gpu_memory->Read<u32>(cbuf.Address() + cbuf_offset);
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

Shader::TextureType ComputeEnvironment::ReadTextureType(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat ComputeEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool ComputeEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 ComputeEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

void FileEnvironment::Deserialize(std::ifstream& file) {
    u64 code_size{};
    u64 num_texture_types{};
    u64 num_texture_pixel_formats{};
    u64 num_cbuf_values{};
    u64 num_storage_pointer_types{};
    u64 num_cbuf_replacement_values{};
    file.read(reinterpret_cast<char*>(&code_size), sizeof(code_size))
        .read(reinterpret_cast<char*>(&num_texture_types), sizeof(num_texture_types))
        .read(reinterpret_cast<char*>(&num_texture_pixel_formats),
              sizeof(num_texture_pixel_formats))
        .read(reinterpret_cast<char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .read(reinterpret_cast<char*>(&num_storage_pointer_types), sizeof(num_storage_pointer_types))
        .read(reinterpret_cast<char*>(&num_cbuf_replacement_values),
              sizeof(num_cbuf_replacement_values))
        .read(reinterpret_cast<char*>(&local_memory_size), sizeof(local_memory_size))
        .read(reinterpret_cast<char*>(&texture_bound), sizeof(texture_bound))
        .read(reinterpret_cast<char*>(&start_address), sizeof(start_address))
        .read(reinterpret_cast<char*>(&read_lowest), sizeof(read_lowest))
        .read(reinterpret_cast<char*>(&read_highest), sizeof(read_highest))
        .read(reinterpret_cast<char*>(&viewport_transform_state), sizeof(viewport_transform_state))
        .read(reinterpret_cast<char*>(&stage), sizeof(stage));
    code.resize(Common::DivCeil(code_size, sizeof(u64)));
    file.read(reinterpret_cast<char*>(code.data()), code_size);
    for (size_t i = 0; i < num_texture_types; ++i) {
        u32 key;
        Shader::TextureType type;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&type), sizeof(type));
        texture_types.emplace(key, type);
    }
    for (size_t i = 0; i < num_texture_pixel_formats; ++i) {
        u32 key;
        Shader::TexturePixelFormat format;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&format), sizeof(format));
        texture_pixel_formats.emplace(key, format);
    }
    for (size_t i = 0; i < num_cbuf_values; ++i) {
        u64 key;
        u32 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_values.emplace(key, value);
    }
    for (size_t i = 0; i < num_storage_pointer_types; ++i) {
        u64 key;
        u8 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        storage_pointer_types.emplace(key, value);
    }
    for (size_t i = 0; i < num_cbuf_replacement_values; ++i) {
        u64 key;
        Shader::ReplaceConstant value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_replacements.emplace(key, value);
    }
    if (stage == Shader::Stage::Compute) {
        file.read(reinterpret_cast<char*>(&workgroup_size), sizeof(workgroup_size))
            .read(reinterpret_cast<char*>(&shared_memory_size), sizeof(shared_memory_size));
        initial_offset = Shader::Ampere::HasNativeAddressMap(code) ? sizeof(sph) : 0;
    } else {
        file.read(reinterpret_cast<char*>(&sph), sizeof(sph));
        initial_offset = sizeof(sph);
        if (stage == Shader::Stage::Geometry) {
            file.read(reinterpret_cast<char*>(&gp_passthrough_mask), sizeof(gp_passthrough_mask));
        }
    }
    u64 native_count{};
    file.read(reinterpret_cast<char*>(&native_count), sizeof(native_count));
    if (native_count) {
        u64 salt{}, num_targets{}, num_tables{};
        if (native_count > Shader::Ampere::ProgramCaptureState::MaximumInstructions) {
            throw std::ios_base::failure("Invalid native program size in pipeline cache");
        }
        file.read(reinterpret_cast<char*>(&salt), sizeof(salt))
            .read(reinterpret_cast<char*>(&native_source_header_size), sizeof(native_source_header_size));
        ampere_program.resize(native_count);
        file.read(reinterpret_cast<char*>(ampere_program.data()),
                  native_count * sizeof(Shader::Ampere::Instruction));
        file.read(reinterpret_cast<char*>(&num_targets), sizeof(num_targets));
        if (num_targets > Shader::Ampere::ProgramCaptureState::MaximumInstructions) {
            throw std::ios_base::failure("Invalid native target count in pipeline cache");
        }
        ampere_indirect_targets.resize(num_targets);
        file.read(reinterpret_cast<char*>(ampere_indirect_targets.data()), num_targets * sizeof(u32))
            .read(reinterpret_cast<char*>(&num_tables), sizeof(num_tables));
        if (num_tables > Shader::Ampere::ProgramCaptureState::MaximumInstructions) {
            throw std::ios_base::failure("Invalid native table count in pipeline cache");
        }
        for (u64 table = 0; table < num_tables; ++table) {
            u32 branch{};
            u64 count{};
            file.read(reinterpret_cast<char*>(&branch), sizeof(branch))
                .read(reinterpret_cast<char*>(&count), sizeof(count));
            if (count > Shader::Ampere::ProgramCaptureState::MaximumInstructions) {
                throw std::ios_base::failure("Invalid native table size in pipeline cache");
            }
            std::vector<u32> targets(count);
            file.read(reinterpret_cast<char*>(targets.data()), count * sizeof(u32));
            native_indirect_tables.emplace_back(branch, std::move(targets));
        }
        // The program hash used as the pipeline key includes the salt, so a pipeline stored by another
        // translator version can never be looked up again; skip it instead of translating it for nothing.
        stale = salt != Shader::Ampere::NATIVE_TRANSLATOR_SALT;
    }
    // Native programs fold bank 1 reads as they did when they were built.
    is_proprietary_driver = texture_bound == 2 || Shader::Ampere::HasNativeAddressMap(code);
}

void FileEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

bool FileEnvironment::HasNativeRegisterCopies() const {
    return !ampere_program.empty() || Shader::Ampere::HasNativeAddressMap(code);
}

u32 FileEnvironment::TranslateProgramAddress(u32 address) const {
    if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) return *translated;
    if (!ampere_program.empty() || Shader::Ampere::HasNativeAddressMap(code))
        throw Shader::RuntimeError("Invalid cached native indirect branch address {:#x}", address);
    return address;
}

u64 FileEnvironment::ReadInstruction(u32 address) {
    if (address < read_lowest || address > read_highest) {
        throw Shader::LogicError("Out of bounds address {}", address);
    }
    return code[(address - read_lowest) / sizeof(u64)];
}

bool FileEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    const auto it = storage_pointer_types.find(MakeCbufKey(index, offset));
    if (it == storage_pointer_types.end()) {
        throw Shader::LogicError("Uncached storage pointer classification");
    }
    return it->second != 0;
}

u32 FileEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto it{cbuf_values.find(MakeCbufKey(cbuf_index, cbuf_offset))};
    if (it == cbuf_values.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TextureType FileEnvironment::ReadTextureType(u32 handle) {
    const auto it{texture_types.find(handle)};
    if (it == texture_types.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TexturePixelFormat FileEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto it{texture_pixel_formats.find(handle)};
    if (it == texture_pixel_formats.end()) {
        throw Shader::LogicError("Uncached read texture pixel format");
    }
    return it->second;
}

bool FileEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 FileEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

u32 FileEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 FileEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

u32 FileEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

std::array<u32, 3> FileEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

std::optional<Shader::ReplaceConstant> FileEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                              u32 offset) {
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    auto it = cbuf_replacements.find(key);
    if (it == cbuf_replacements.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version) try {
    std::ofstream file(filename, std::ios::binary | std::ios::ate | std::ios::app);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    if (file.tellp() == 0) {
        // Write header
        file.write(MAGIC_NUMBER.data(), MAGIC_NUMBER.size())
            .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));
    }
    if (!std::ranges::all_of(envs, &GenericEnvironment::CanBeSerialized)) {
        return;
    }
    const u32 num_envs{static_cast<u32>(envs.size())};
    file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
    for (const GenericEnvironment* const env : envs) {
        env->Serialize(file);
    }
    file.write(key.data(), key.size_bytes());

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics) try {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return;
    }
    file.exceptions(std::ifstream::failbit);
    const auto end{file.tellg()};
    file.seekg(0, std::ios::beg);

    std::array<char, 8> magic_number;
    u32 cache_version;
    file.read(magic_number.data(), magic_number.size())
        .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
    if (magic_number != MAGIC_NUMBER || cache_version != expected_cache_version) {
        file.close();
        if (Common::FS::RemoveFile(filename)) {
            if (magic_number != MAGIC_NUMBER) {
                LOG_ERROR(Common_Filesystem, "Invalid pipeline cache file");
            }
            if (cache_version != expected_cache_version) {
                LOG_INFO(Common_Filesystem, "Deleting old pipeline cache");
            }
        } else {
            LOG_ERROR(Common_Filesystem,
                      "Invalid pipeline cache file and failed to delete it in \"{}\"",
                      Common::FS::PathToUTF8String(filename));
        }
        return;
    }
    size_t entries{};
    while (file.tellg() != end) {
        if (stop_loading.stop_requested()) {
            LOG_INFO(Common_Filesystem, "Pipeline cache load stopped after {} entries", entries);
            return;
        }
        u32 num_envs{};
        file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
        std::vector<FileEnvironment> envs(num_envs);
        for (FileEnvironment& env : envs) {
            env.Deserialize(file);
        }
        if (envs.front().ShaderStage() == Shader::Stage::Compute) {
            load_compute(file, std::move(envs.front()));
        } else {
            load_graphics(file, std::move(envs));
        }
        ++entries;
    }
    LOG_INFO(Common_Filesystem, "Pipeline cache read {} entries ({} bytes)", entries,
             static_cast<s64>(end));

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

} // namespace VideoCommon
