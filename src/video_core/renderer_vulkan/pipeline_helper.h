// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/exception.h"
#include "video_core/back_thread_stats.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/compact_texture_table.h"
#include "video_core/texture_cache/lookup_memo.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

using Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS;

[[nodiscard]] inline std::optional<PixelFormat> PixelFormatFromImageFormat(
    Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::Typeless:
        return std::nullopt;
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    }
    return std::nullopt;
}

[[nodiscard]] inline VkDeviceSize DescriptorSizeForType(const Device& device,
                                                        VkDescriptorType type) {
    const auto& props = device.DescriptorBufferProperties();
    const bool robust = device.IsRobustBufferAccessEnabled();
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return robust ? props.robustUniformBufferDescriptorSize : props.uniformBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        return robust ? props.robustStorageBufferDescriptorSize : props.storageBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        return robust ? props.robustUniformTexelBufferDescriptorSize
                      : props.uniformTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return robust ? props.robustStorageTexelBufferDescriptorSize
                      : props.storageTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        return props.combinedImageSamplerDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        return props.storageImageDescriptorSize;
    default:
        return 0;
    }
}

struct DescriptorBufferBinding {
    VkDescriptorType type;
    u32 count;
    VkDeviceSize offset;
    VkDeviceSize stride;
};

struct DescriptorBufferLayout {
    VkDeviceSize size{};
    boost::container::small_vector<DescriptorBufferBinding, 32> bindings;

    [[nodiscard]] bool Empty() const noexcept {
        return bindings.empty();
    }
};

inline void WriteDescriptorBuffer(const Device& device, const DescriptorBufferLayout& layout,
                                  const DescriptorUpdateEntry* payload, u8* host) {
    const vk::Device& dev = device.GetLogical();
    for (const DescriptorBufferBinding& binding : layout.bindings) {
        for (u32 index = 0; index < binding.count; ++index) {
            const DescriptorUpdateEntry& entry = *(payload++);
            const VkDescriptorAddressInfoEXT address_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
                .pNext = nullptr,
                .address = entry.address.address,
                .range = entry.address.range,
                .format = entry.address.format,
            };
            VkDescriptorGetInfoEXT get_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                .pNext = nullptr,
                .type = binding.type,
                .data{},
            };
            switch (binding.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                get_info.data.pUniformBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                get_info.data.pStorageBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                get_info.data.pUniformTexelBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                get_info.data.pStorageTexelBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                get_info.data.pCombinedImageSampler = &entry.image;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                get_info.data.pStorageImage = &entry.image;
                break;
            default:
                continue;
            }
            dev.GetDescriptorEXT(get_info, binding.stride,
                                 host + binding.offset + index * binding.stride);
        }
    }
}

[[nodiscard]] inline u32 NumDescriptorEntries(const Shader::Info& info) {
    return Shader::NumDescriptors(info.constant_buffer_descriptors) +
           Shader::NumDescriptors(info.storage_buffers_descriptors) +
           Shader::NumDescriptors(info.texture_buffer_descriptors) +
           Shader::NumDescriptors(info.image_buffer_descriptors) +
           Shader::NumDescriptors(info.texture_descriptors) +
           Shader::NumDescriptors(info.image_descriptors);
}

class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder(const Device& device_) : device{&device_} {}

    bool CanUsePushDescriptor() const noexcept {
        if (!device->IsKhrPushDescriptorSupported() ||
            num_descriptors > device->MaxPushDescriptors()) {
            return false;
        }
        return !device->IsExtDescriptorBufferSupported() ||
               device->DescriptorBufferProperties().bufferlessPushDescriptors;
    }

    bool CanUseDescriptorBuffer() const noexcept {
        const auto& props = device->DescriptorBufferProperties();
        if (!device->IsExtDescriptorBufferSupported() || bindings.empty() ||
            !props.combinedImageSamplerDescriptorSingleArray) {
            return false;
        }
        return !props.bufferlessPushDescriptors || !CanUsePushDescriptor();
    }

    DescriptorBufferLayout MakeDescriptorBufferLayout(VkDescriptorSetLayout layout) const {
        DescriptorBufferLayout result;
        if (!layout) {
            return result;
        }
        const vk::Device& dev = device->GetLogical();
        result.size = dev.GetDescriptorSetLayoutSizeEXT(layout);
        result.bindings.reserve(bindings.size());
        for (const VkDescriptorSetLayoutBinding& entry : bindings) {
            result.bindings.push_back(DescriptorBufferBinding{
                .type = entry.descriptorType,
                .count = entry.descriptorCount,
                .offset = dev.GetDescriptorSetLayoutBindingOffsetEXT(layout, entry.binding),
                .stride = DescriptorSizeForType(*device, entry.descriptorType),
            });
        }
        return result;
    }

    vk::DescriptorSetLayout CreateDescriptorSetLayout(bool use_push_descriptor,
                                                      bool use_descriptor_buffer = false) const {
        if (bindings.empty()) {
            return nullptr;
        }
        VkDescriptorSetLayoutCreateFlags flags = 0;
        if (use_push_descriptor) {
            flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        }
        if (use_descriptor_buffer) {
            flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        }
        boost::container::small_vector<VkDescriptorBindingFlags, 32> binding_flags;
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_ci{};
        const void* pnext = nullptr;
        if (!use_push_descriptor && device->IsDescriptorBindingPartiallyBoundSupported()) {
            binding_flags.assign(bindings.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
            binding_flags_ci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .pNext = nullptr,
                .bindingCount = static_cast<u32>(binding_flags.size()),
                .pBindingFlags = binding_flags.data(),
            };
            pnext = &binding_flags_ci;
        }
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = pnext,
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
    }

    vk::DescriptorUpdateTemplate CreateTemplate(VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout pipeline_layout,
                                                bool use_push_descriptor) const {
        if (entries.empty()) {
            return nullptr;
        }
        const VkDescriptorUpdateTemplateType type =
            use_push_descriptor ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
                                : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        return device->GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = type,
            .descriptorSetLayout = descriptor_set_layout,
            .pipelineBindPoint =
                is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = pipeline_layout,
            .set = 0,
        });
    }

    vk::PipelineLayout CreatePipelineLayout(VkDescriptorSetLayout descriptor_set_layout) const {
        using Shader::Backend::SPIRV::RenderAreaLayout;
        using Shader::Backend::SPIRV::RescalingLayout;
        const u32 size_offset = is_compute ? sizeof(RescalingLayout::down_factor) : 0u;
        const VkPushConstantRange range{
            .stageFlags = static_cast<VkShaderStageFlags>(
                is_compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS),
            .offset = 0,
            .size = static_cast<u32>(sizeof(RescalingLayout)) - size_offset +
                    static_cast<u32>(sizeof(RenderAreaLayout)),
        };
        return device->GetLogical().CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = descriptor_set_layout ? 1U : 0U,
            .pSetLayouts = bindings.empty() ? nullptr : &descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
    }

    void Add(const Shader::Info& info, VkShaderStageFlags stage) {
        is_compute |= (stage & VK_SHADER_STAGE_COMPUTE_BIT) != 0;

        Add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stage, info.constant_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, info.storage_buffers_descriptors);
        Add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, stage, info.texture_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, stage, info.image_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, info.texture_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stage, info.image_descriptors);
    }

private:
    template <typename Descriptors>
    void Add(VkDescriptorType type, VkShaderStageFlags stage, const Descriptors& descriptors) {
        const size_t num{descriptors.size()};
        for (size_t i = 0; i < num; ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = descriptors[i].count,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = descriptors[i].count,
                .descriptorType = type,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += descriptors[i].count;
            offset += sizeof(DescriptorUpdateEntry) * descriptors[i].count;
        }
    }

    const Device* device{};
    bool is_compute{};
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 32> bindings;
    boost::container::small_vector<VkDescriptorUpdateTemplateEntry, 32> entries;
    u32 binding{};
    u32 num_descriptors{};
    size_t offset{};
};

class RescalingPushConstant {
public:
    explicit RescalingPushConstant() noexcept {}

    void PushTexture(bool is_rescaled) noexcept {
        *texture_ptr |= is_rescaled ? texture_bit : 0u;
        texture_bit <<= 1u;
        if (texture_bit == 0u) {
            texture_bit = 1u;
            ++texture_ptr;
        }
    }

    void PushImage(bool is_rescaled) noexcept {
        *image_ptr |= is_rescaled ? image_bit : 0u;
        image_bit <<= 1u;
        if (image_bit == 0u) {
            image_bit = 1u;
            ++image_ptr;
        }
    }

    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& Data() const noexcept {
        return words;
    }

private:
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> words{};
    u32* texture_ptr{words.data()};
    u32* image_ptr{words.data() + Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

class RenderAreaPushConstant {
public:
    bool uses_render_area{};
    std::array<f32, 4> words{};
};

/// A draw's compact texture tables (see compact_texture_table.h), in binding order.
struct CompactTextureBinding {
    std::span<const VideoCommon::CompactTextureBlock> blocks;
    const VideoCommon::ImageViewInOut* views_base{};
    std::span<const VideoCommon::ImageViewId> view_ids;
    std::span<const VideoCommon::SamplerId> sampler_ids;
    size_t next{};
};

inline void PushImageDescriptors(TextureCache& texture_cache,
                                 GuestDescriptorQueue& guest_descriptor_queue,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VideoCommon::SamplerId*& samplers,
                                 const VideoCommon::ImageViewInOut*& views,
                                 CompactTextureBinding* compact = nullptr) {
    const u32 num_texture_buffers = Shader::NumDescriptors(info.texture_buffer_descriptors);
    const u32 num_image_buffers = Shader::NumDescriptors(info.image_buffer_descriptors);
    views += num_texture_buffers;
    views += num_image_buffers;
    // Records of buffer-backed arrays repeat the same view and sampler; within one descriptor of one
    // draw the pair always yields the same handles.
    struct SampledImage {
        VkImageView view{};
        VkSampler sampler{};
        bool rescaled{};
    };
    static thread_local VideoCommon::LookupMemo<SampledImage, 1024, u64> memo;
    static thread_local std::vector<SampledImage> pair_images;
    for (const auto& desc : info.texture_descriptors) {
        // The same handles per (view, sampler), whether made for an element or for a distinct pair.
        const auto make_sampled = [&](VideoCommon::ImageViewId image_view_id,
                                      VideoCommon::SamplerId sampler_id, u32 index,
                                      u32 guest_index) -> SampledImage {
            ImageView& image_view{texture_cache.GetImageView(image_view_id)};
            VkImageView vk_image_view{image_view.Handle(desc.type)};
            if (desc.buffer_source.enabled && image_view_id != VideoCommon::NULL_IMAGE_VIEW_ID &&
                (VideoCore::Surface::IsPixelFormatInteger(image_view.format) || vk_image_view == VK_NULL_HANDLE)) {
                // Every record of the table is bound, including ones this instruction cannot
                // sample (FE light lists: a point-light cube slot can hold a single-layer 2D).
                // Hardware samples such a slot as undefined data; a typed null view keeps the
                // draw instead of dropping it (that dropped the battle floor until a restart).
                static u32 warnings{};
                if (warnings < 16) {
                    ++warnings;
                    LOG_WARNING(Render_Vulkan,
                        "Buffer-backed texture entry {} (CB{}+{:#x}, field {:#x}) has format {}, expected type {}, view available {}, texture index {}, guest type {}, {}x{} layers {}; sampling a null view",
                        index, desc.cbuf_index, desc.cbuf_offset, desc.buffer_source.offset,
                        static_cast<u32>(image_view.format), static_cast<u32>(desc.type), vk_image_view != VK_NULL_HANDLE,
                        guest_index, static_cast<u32>(image_view.type), image_view.size.width,
                        image_view.size.height, image_view.range.extent.layers);
                }
                vk_image_view = VK_NULL_HANDLE;
            }
            if (vk_image_view == VK_NULL_HANDLE) {
                const VkImageView null_image_view{texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID).Handle(desc.type)};
                if (null_image_view != VK_NULL_HANDLE) vk_image_view = null_image_view;
            }
            Sampler& sampler{texture_cache.GetSampler(sampler_id)};
            const VkSampler vk_sampler{sampler.HandleFor(image_view, desc.is_depth)};
            return SampledImage{vk_image_view, vk_sampler, texture_cache.IsRescaling(image_view)};
        };
        // A compact table: one SampledImage per distinct pair, then one copy per element.
        if (compact && compact->next < compact->blocks.size() &&
            compact->blocks[compact->next].first_view == static_cast<size_t>(views - compact->views_base) &&
            compact->blocks[compact->next].table->element_tics.size() == desc.count) {
            const auto& block = compact->blocks[compact->next++];
            // The descriptors written for this table last time, if they were made from the same
            // resolved banks (same views and samplers) at the same texture cache generation (same
            // image handles and scaling).
            auto& cache = block.table->descriptor_cache;
            static_assert(sizeof(DescriptorUpdateEntry) % sizeof(u64) == 0);
            constexpr size_t words_per_entry = sizeof(DescriptorUpdateEntry) / sizeof(u64);
            // Descriptors only depend on the views' and samplers' handles and scaling: the view
            // generation, which images registered or written by the CPU do not bump.
            const u64 generation = texture_cache.ViewGeneration();
            const bool reusable = block.view_stamp != 0 && block.sampler_stamp != 0;
            // Entries written in this view generation for this many elements: patchable element by
            // element (same view and sampler ids, same descriptor), and reusable as they are when
            // the table did not change and the banks are the same.
            const bool patchable = cache.generation == generation &&
                                   cache.words.size() == size_t{desc.count} * words_per_entry &&
                                   cache.keys.size() == desc.count;
            const bool same_banks = patchable && cache.current && reusable &&
                                    cache.view_stamp == block.view_stamp &&
                                    cache.sampler_stamp == block.sampler_stamp;
            auto& stats = VideoCommon::CompactTextureStats::Get();
            bool patched = false;
            if (patchable && !same_banks) {
                // The array or its banks changed: rewrite only the elements whose view or sampler
                // changed.
                static thread_local std::vector<u8> pairs_made;
                const size_t rewritten = VideoCommon::PatchCompactElements<SampledImage>(
                    block, compact->view_ids, compact->sampler_ids, cache.keys, pair_images,
                    pairs_made,
                    [&](VideoCommon::ImageViewId image_view_id, VideoCommon::SamplerId sampler_id,
                        u32 guest_index) {
                        return make_sampled(image_view_id, sampler_id, 0, guest_index);
                    },
                    [&](size_t element, const SampledImage& image) {
                        // As AddSampledImage writes it, padding zeroed.
                        std::array<u64, words_per_entry> entry{};
                        const VkDescriptorImageInfo info{
                            .sampler = image.sampler,
                            .imageView = image.view,
                            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                        };
                        std::memcpy(entry.data(), &info.sampler, sizeof(info.sampler));
                        std::memcpy(entry.data() + 1, &info.imageView, sizeof(info.imageView));
                        std::memcpy(entry.data() + 2, &info.imageLayout, sizeof(info.imageLayout));
                        std::memcpy(cache.words.data() + element * words_per_entry, entry.data(),
                                    sizeof(entry));
                        const u8 rescaled = image.rescaled ? 1 : 0;
                        cache.rescaled_count = cache.rescaled_count - cache.rescaled_elements[element] + rescaled;
                        cache.rescaled_elements[element] = rescaled;
                    });
                cache.rescaled = cache.rescaled_count != 0;
                cache.current = true;
                cache.view_stamp = block.view_stamp;
                cache.sampler_stamp = block.sampler_stamp;
                patched = true;
                ++stats.descriptor_patches;
                stats.patched_elements += rewritten;
            }
            if (same_banks || patched) {
                guest_descriptor_queue.AddEntries(cache.words.data(), desc.count);
                if (VideoCommon::BackStats::Enabled()) {
                    VideoCommon::BackStats::draw_thread.copied_bytes +=
                        u64{desc.count} * sizeof(DescriptorUpdateEntry);
                }
                views += desc.count;
                samplers += desc.count;
                rescaling.PushTexture(cache.rescaled);
                if (!patched) {
                    ++stats.descriptor_hits;
                }
                continue;
            }
            const DescriptorUpdateEntry* const first_entry = guest_descriptor_queue.Cursor();
            const bool is_rescaled = VideoCommon::PushCompactTextureBlock<SampledImage>(
                block, compact->view_ids, compact->sampler_ids, pair_images,
                [&](VideoCommon::ImageViewId image_view_id, VideoCommon::SamplerId sampler_id, u32 guest_index) {
                    const SampledImage image = make_sampled(image_view_id, sampler_id, 0, guest_index);
                    return std::pair{image, image.rescaled};
                },
                [&](const SampledImage& image) {
                    guest_descriptor_queue.AddSampledImage(image.view, image.sampler);
                });
            views += desc.count;
            samplers += desc.count;
            rescaling.PushTexture(is_rescaled);
            {
                ++stats.descriptor_misses;
                const auto& table = *block.table;
                cache.view_stamp = block.view_stamp;
                cache.sampler_stamp = block.sampler_stamp;
                cache.generation = generation;
                cache.rescaled = is_rescaled;
                cache.current = true;
                cache.words.resize(size_t{desc.count} * words_per_entry);
                std::memcpy(cache.words.data(), first_entry, size_t{desc.count} * sizeof(DescriptorUpdateEntry));
                cache.keys.resize(desc.count);
                cache.rescaled_elements.resize(desc.count);
                cache.rescaled_count = 0;
                for (size_t element = 0; element < desc.count; ++element) {
                    cache.keys[element] = VideoCommon::CompactElementKey(
                        block, element, compact->view_ids, compact->sampler_ids);
                    const u8 rescaled = pair_images[table.element_pairs[element]].rescaled ? 1 : 0;
                    cache.rescaled_elements[element] = rescaled;
                    cache.rescaled_count += rescaled;
                }
            }
            continue;
        }
        bool is_rescaled{};
        memo.Reset();
        for (u32 index = 0; index < desc.count; ++index) {
            const auto& guest_view = *views++;
            const VideoCommon::ImageViewId image_view_id{guest_view.id};
            const VideoCommon::SamplerId sampler_id{*(samplers++)};
            const u64 memo_key = u64{image_view_id.index} | (u64{sampler_id.index} << 32);
            if (const SampledImage* const known = memo.Find(memo_key)) {
                guest_descriptor_queue.AddSampledImage(known->view, known->sampler);
                is_rescaled |= known->rescaled;
                continue;
            }
            const SampledImage image = make_sampled(image_view_id, sampler_id, index, guest_view.index);
            guest_descriptor_queue.AddSampledImage(image.view, image.sampler);
            is_rescaled |= image.rescaled;
            memo.Store(memo_key, image);
        }
        rescaling.PushTexture(is_rescaled);
    }
    for (const auto& desc : info.image_descriptors) {
        bool is_rescaled{};
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            if (desc.is_written) {
                texture_cache.MarkModification(image_view.image_id);
            }
            const VkImageView vk_image_view{image_view.StorageView(desc.type, desc.format)};
            guest_descriptor_queue.AddImage(vk_image_view);
            const bool element_rescaled{texture_cache.IsRescaling(image_view)};
            is_rescaled |= element_rescaled;
        }
        rescaling.PushImage(is_rescaled);
    }
}

} // namespace Vulkan
