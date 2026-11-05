// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/back_thread_stats.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

/// EDEN_NO_HEAP_REUSE disables the reuse of resolved texture banks between draws
/// (texture_bank_reuse.h) and, with it, the reuse of their descriptors. Kept as a safety switch
/// for isolating wrong textures.
inline bool HeapReuseEnabled() {
    static const bool enabled = std::getenv("EDEN_NO_HEAP_REUSE") == nullptr;
    return enabled;
}

/// A buffer-backed texture array as its distinct (texture, sampler) pairs and the pair each element
/// samples. Fortune's Weave binds hundreds of records on every draw that name a few dozen distinct
/// textures and samplers: resolving, preparing and turning into descriptors only the distinct ones
/// leaves one copy per element. Built from the decoded handles, so it changes only with them.
struct CompactTextureTable {
    struct Pair {
        u32 tic_slot;  ///< Into unique_tics
        u32 tsc_slot;  ///< Into unique_tscs
        bool excluded; ///< The element samples a null view (buffer texture exclusion field)
    };
    std::vector<u32> element_tics;  ///< TIC index of every element (views keep it)
    std::vector<u32> unique_tics;   ///< Distinct TIC indices, first-occurrence order (all elements)
    std::vector<u32> unique_tscs;   ///< Distinct TSC indices, first-occurrence order
    std::vector<Pair> pairs;        ///< Distinct pairs, first-occurrence order
    std::vector<u32> element_pairs; ///< Pair of every element
    bool has_excluded{};            ///< Some pair samples a null view (exclusion field)
    u64 tics_hash{};                ///< Of unique_tics (finds the resolved bank)
    u64 tscs_hash{};                ///< Of unique_tscs

    /// The descriptors last written for this table, with what named them: the resolved banks
    /// (stamps, see texture_bank_reuse.h) and the texture cache view generation. Raw bytes of the
    /// backend's descriptor entries. Build keeps them (current = false): when the array changes
    /// within one generation, the elements that resolve to the same view and sampler keep their
    /// entries (PatchCompactElements).
    struct DescriptorCache {
        u64 view_stamp{};
        u64 sampler_stamp{};
        u64 generation{};
        bool rescaled{};
        bool current{}; ///< Written for the table as it is now
        std::vector<u64> words;
        std::vector<u64> keys;             ///< CompactElementKey of every element the words were written for
        std::vector<u8> rescaled_elements; ///< Whether each element's image is rescaled
        u32 rescaled_count{};
    };
    mutable DescriptorCache descriptor_cache;


    /// handles: (TIC, TSC) of every element; excluded: non-zero for excluded elements.
    /// Direct-mapped like the per-draw lookup memos, so a build costs about what binding the elements
    /// one by one did. A collision only lists a key twice (two slots that resolve the same way).
    void Build(std::span<const std::pair<u32, u32>> handles, std::span<const u8> excluded) {
        if (BackStats::Enabled()) {
            CountChanges(handles, excluded);
        }
        element_tics.clear();
        unique_tics.clear();
        unique_tscs.clear();
        pairs.clear();
        element_pairs.clear();
        element_tics.reserve(handles.size());
        element_pairs.reserve(handles.size());
        static thread_local SlotMemo<u32, 2048> tic_slots;
        static thread_local SlotMemo<u32, 256> tsc_slots;
        static thread_local SlotMemo<u64, 4096> pair_slots;
        tic_slots.Reset();
        tsc_slots.Reset();
        pair_slots.Reset();
        for (size_t element = 0; element < handles.size(); ++element) {
            const auto [tic, tsc] = handles[element];
            const bool is_excluded = excluded[element] != 0;
            const auto [tic_slot, new_tic] = tic_slots.Get(tic, static_cast<u32>(unique_tics.size()));
            if (new_tic) {
                unique_tics.push_back(tic);
            }
            const auto [tsc_slot, new_tsc] = tsc_slots.Get(tsc, static_cast<u32>(unique_tscs.size()));
            if (new_tsc) {
                unique_tscs.push_back(tsc);
            }
            const u64 key = u64{tic_slot} | (u64{tsc_slot} << 32) | (is_excluded ? (u64{1} << 63) : 0);
            const auto [pair_slot, new_pair] = pair_slots.Get(key, static_cast<u32>(pairs.size()));
            if (new_pair) {
                pairs.push_back(Pair{tic_slot, tsc_slot, is_excluded});
            }
            element_tics.push_back(tic);
            element_pairs.push_back(pair_slot);
        }
        has_excluded = std::ranges::any_of(pairs, &Pair::excluded);
        tics_hash = Common::CityHash64(reinterpret_cast<const char*>(unique_tics.data()),
                                       unique_tics.size() * sizeof(u32));
        tscs_hash = Common::CityHash64(reinterpret_cast<const char*>(unique_tscs.data()),
                                       unique_tscs.size() * sizeof(u32));
        descriptor_cache.current = false;
    }

private:
    /// Statistics: how many elements differ from the table as last built (EDEN_BACK_THREAD_STATS).
    void CountChanges(std::span<const std::pair<u32, u32>> handles, std::span<const u8> excluded) const {
        auto& stats = BackStats::draw_thread;
        ++stats.builds;
        stats.build_elements += handles.size();
        if (element_tics.size() != handles.size()) {
            ++stats.build_resized;
            stats.build_changed += handles.size();
            return;
        }
        for (size_t element = 0; element < handles.size(); ++element) {
            const Pair& pair = pairs[element_pairs[element]];
            const bool was_excluded = pair.excluded;
            if (element_tics[element] != handles[element].first ||
                unique_tscs[pair.tsc_slot] != handles[element].second ||
                was_excluded != (excluded[element] != 0)) {
                ++stats.build_changed;
            }
        }
    }

    /// Direct-mapped key -> slot map, emptied by Reset in O(1) (generation tags).
    template <typename Key, size_t Size>
    struct SlotMemo {
        static_assert((Size & (Size - 1)) == 0);
        struct Entry {
            Key key{};
            u32 generation{};
            u32 slot{};
        };
        std::array<Entry, Size> entries{};
        u32 generation{};

        void Reset() noexcept {
            if (++generation == 0) {
                entries.fill({});
                generation = 1;
            }
        }
        /// The slot of key, or next (stored) if key is not there.
        std::pair<u32, bool> Get(Key key, u32 next) noexcept {
            const u64 wide = static_cast<u64>(key);
            Entry& entry = entries[static_cast<size_t>((wide ^ (wide >> 29) ^ (wide >> 32)) & (Size - 1))];
            if (entry.generation == generation && entry.key == key) {
                return {entry.slot, false};
            }
            entry = Entry{key, generation, next};
            return {next, true};
        }
    };
};

/// Where one compact table sits in a draw: its elements in the draw's views and samplers, and its
/// resolved view ids (one per unique TIC) and sampler ids (one per unique TSC) in per-draw arrays.
struct CompactTextureBlock {
    const CompactTextureTable* table{};
    size_t first_view{};
    size_t first_sampler{};
    size_t view_ids{};    ///< Offset of this block's resolved view ids
    size_t sampler_ids{}; ///< Offset of this block's resolved sampler ids
    bool views_reused{};  ///< view_ids already hold the bank's ids (nothing to look up)
    u64 view_stamp{};     ///< Resolved view bank (0: none; see texture_bank_reuse.h)
    u64 sampler_stamp{};  ///< Resolved sampler bank (0: none)
};

/// Lists what a draw's view lookup has to resolve: every view outside the blocks (in order) and, in
/// place of each block, its unique TICs. The texture cache then visits the same first occurrences in
/// the same order as with the elements themselves (its per-draw memo serves the repeats).
/// positions receives, for each entry of fill outside a block, its index in views. Returns false if
/// a block does not start at an element boundary (nothing may be bound then). Blocks whose views
/// were reused add no lookups.
template <typename View>
[[nodiscard]] bool CompactViewLookups(std::span<const View> views,
                                      std::span<const CompactTextureBlock> blocks,
                                      std::vector<View>& fill, std::vector<size_t>& positions,
                                      std::vector<size_t>& block_fill_offsets) {
    fill.clear();
    positions.clear();
    block_fill_offsets.clear();
    size_t next_block = 0;
    for (size_t index = 0; index < views.size();) {
        if (next_block < blocks.size() && blocks[next_block].first_view == index) {
            const CompactTextureBlock& block = blocks[next_block++];
            block_fill_offsets.push_back(fill.size());
            if (!block.views_reused) {
                for (const u32 tic : block.table->unique_tics) {
                    fill.push_back(View{.index = tic, .blacklist = false, .id = {}});
                }
            }
            index += block.table->element_tics.size();
            continue;
        }
        fill.push_back(views[index]);
        positions.push_back(index);
        ++index;
    }
    return next_block == blocks.size();
}

/// Writes every block element's view (excluded ones get the null view) and sampler, so views and
/// samplers read as if bound element by element. view_ids must hold the blocks' resolved ids.
template <typename View>
void ExpandBlockElements(std::span<View> views, std::span<SamplerId> samplers,
                         std::span<const CompactTextureBlock> blocks,
                         std::span<const ImageViewId> view_ids,
                         std::span<const SamplerId> sampler_ids) {
    for (const CompactTextureBlock& block : blocks) {
        const CompactTextureTable& table = *block.table;
        for (size_t element = 0; element < table.element_tics.size(); ++element) {
            const auto& pair = table.pairs[table.element_pairs[element]];
            views[block.first_view + element] = View{
                .index = table.element_tics[element],
                .blacklist = false,
                .id = pair.excluded ? NULL_IMAGE_VIEW_ID : view_ids[block.view_ids + pair.tic_slot],
            };
            samplers[block.first_sampler + element] = sampler_ids[block.sampler_ids + pair.tsc_slot];
        }
    }
}

/// After the lookup: gives back the ids of the views outside the blocks and keeps each block's
/// resolved ids in view_ids. With elements, also expands the blocks (ExpandBlockElements); without,
/// the blocks' ranges of views and samplers are left as they were (the compact descriptor path does
/// not read them; expand before anything else does).
template <typename View>
void ExpandViewLookups(std::span<View> views, std::span<SamplerId> samplers,
                              std::span<const CompactTextureBlock> blocks,
                              std::span<const View> fill,
                              std::span<const size_t> positions,
                              std::span<const size_t> block_fill_offsets,
                              std::span<ImageViewId> view_ids,
                              std::span<const SamplerId> sampler_ids, bool elements = true) {
    size_t entry = 0;
    size_t position = 0;
    for (size_t block_index = 0; block_index <= blocks.size(); ++block_index) {
        const size_t block_start = block_index < blocks.size() ? block_fill_offsets[block_index] : fill.size();
        for (; entry < block_start; ++entry) {
            views[positions[position++]].id = fill[entry].id;
        }
        if (block_index == blocks.size()) {
            break;
        }
        const CompactTextureBlock& block = blocks[block_index];
        const CompactTextureTable& table = *block.table;
        if (!block.views_reused) {
            for (size_t slot = 0; slot < table.unique_tics.size(); ++slot) {
                view_ids[block.view_ids + slot] = fill[entry++].id;
            }
        }
    }
    if (elements) {
        ExpandBlockElements<View>(views, samplers, blocks, view_ids, sampler_ids);
    }
}

/// Writes a block's descriptors: make(view_id, sampler_id, guest_index) runs once per distinct pair
/// and returns {image, rescaled}; add(image) runs once per element, in element order.
template <typename Image, typename Make, typename Add>
bool PushCompactTextureBlock(const CompactTextureBlock& block, std::span<const ImageViewId> view_ids,
                             std::span<const SamplerId> sampler_ids, std::vector<Image>& scratch,
                             Make&& make, Add&& add) {
    const CompactTextureTable& table = *block.table;
    bool rescaled = false;
    scratch.resize(table.pairs.size());
    for (size_t index = 0; index < table.pairs.size(); ++index) {
        const auto& pair = table.pairs[index];
        const ImageViewId view_id = pair.excluded ? NULL_IMAGE_VIEW_ID : view_ids[block.view_ids + pair.tic_slot];
        const SamplerId sampler_id = sampler_ids[block.sampler_ids + pair.tsc_slot];
        const auto [image, image_rescaled] = make(view_id, sampler_id, table.unique_tics[pair.tic_slot]);
        scratch[index] = image;
        rescaled |= image_rescaled;
    }
    for (const u32 pair : table.element_pairs) {
        add(scratch[pair]);
    }
    return rescaled;
}

/// What an element's descriptor is made from: the view it samples (null if excluded) and its
/// sampler, as resolved for this draw. Within one texture cache generation the same ids make the
/// same descriptor, whichever bank or table they came from.
[[nodiscard]] inline u64 CompactElementKey(const CompactTextureBlock& block, size_t element,
                                           std::span<const ImageViewId> view_ids,
                                           std::span<const SamplerId> sampler_ids) noexcept {
    const CompactTextureTable& table = *block.table;
    const auto& pair = table.pairs[table.element_pairs[element]];
    const ImageViewId view_id =
        pair.excluded ? NULL_IMAGE_VIEW_ID : view_ids[block.view_ids + pair.tic_slot];
    return u64{view_id.index} | (u64{sampler_ids[block.sampler_ids + pair.tsc_slot].index} << 32);
}

/// Brings a block's cached per-element results up to date with its table: for every element whose
/// CompactElementKey differs from keys[element], write(element, image of its pair) runs and
/// keys[element] is updated. make(view_id, sampler_id, guest_index) runs once per distinct pair
/// needed, as in PushCompactTextureBlock. Only valid when keys were written in the same texture
/// cache view generation (the same ids then make the same images). Returns the elements rewritten.
template <typename Image, typename Make, typename Write>
size_t PatchCompactElements(const CompactTextureBlock& block, std::span<const ImageViewId> view_ids,
                            std::span<const SamplerId> sampler_ids, std::span<u64> keys,
                            std::vector<Image>& scratch, std::vector<u8>& made, Make&& make,
                            Write&& write) {
    const CompactTextureTable& table = *block.table;
    scratch.resize(table.pairs.size());
    made.assign(table.pairs.size(), 0);
    size_t rewritten = 0;
    for (size_t element = 0; element < table.element_tics.size(); ++element) {
        const u64 key = CompactElementKey(block, element, view_ids, sampler_ids);
        if (keys[element] == key) {
            continue;
        }
        const u32 pair_index = table.element_pairs[element];
        if (!made[pair_index]) {
            const auto& pair = table.pairs[pair_index];
            const ImageViewId view_id =
                pair.excluded ? NULL_IMAGE_VIEW_ID : view_ids[block.view_ids + pair.tic_slot];
            scratch[pair_index] = make(view_id, sampler_ids[block.sampler_ids + pair.tsc_slot],
                                       table.unique_tics[pair.tic_slot]);
            made[pair_index] = 1;
        }
        write(element, scratch[pair_index]);
        keys[element] = key;
        ++rewritten;
    }
    return rewritten;
}

/// EDEN_TEXTURE_TABLE_STATS: logs every 4 s how many draws used compact tables and how much they
/// folded (elements bound vs distinct pairs prepared).
struct CompactTextureStats {
    u64 draws{};
    u64 decoded{};  ///< Buffer texture arrays decoded again (records changed)
    u64 reused{};   ///< Buffer texture arrays whose decoded handles were reused
    u64 cbuf_decoded{}; ///< Constant buffer texture arrays decoded again
    u64 cbuf_reused{};  ///< Constant buffer texture arrays whose decoded handles were reused
    u64 table_draws{};
    u64 elements{};
    u64 pairs{};
    u64 unique_views{};
    u64 unique_samplers{};
    // Bank reuse (texture_bank_reuse.h); counted by the texture cache and the pipeline.
    u64 view_hits{};
    u64 view_misses{};       ///< No bank with these indices
    u64 view_stale_table{};  ///< Descriptor table moved, resized or another channel
    u64 view_stale_generation{}; ///< Images created, deleted, rescaled or written by the CPU
    u64 view_stale_memory{}; ///< Descriptor table entries rewritten
    u64 view_unstored{};     ///< Could not be kept (an image still pending upload, unmapped table)
    u64 view_retries{};      ///< Reused, then invalidated by the rest of the draw's lookups
    u64 alias_prepares{};
    u64 snapshot_bytes{};
    u64 sampler_hits{};
    u64 sampler_misses{};
    u64 descriptor_hits{};
    u64 descriptor_misses{};
    u64 descriptor_patches{};     ///< Changed arrays whose unchanged elements kept their entries
    u64 patched_elements{};       ///< Elements rewritten by those patches
    // Draw path (buffer cache and draw manager).
    u64 range_hits{};          ///< Virtual range lookups served from the cache
    u64 range_misses{};        ///< ... walked again (GetSubmappedRange)
    u64 host_conditional_draws{}; ///< Draws prepared under host conditional rendering
    u64 skipped_draws{};       ///< Draws a CPU-evaluated condition skipped (no preparation)
    u64 cached_write_batches{}; ///< InvalidateGPUCache batches of held guest writes
    u64 cached_write_regions{}; ///< Regions in those batches
    std::chrono::steady_clock::time_point window{std::chrono::steady_clock::now()};

    static bool Enabled() {
        static const bool enabled = std::getenv("EDEN_TEXTURE_TABLE_STATS") != nullptr;
        return enabled;
    }

    /// The GPU thread's statistics (one thread configures graphics draws).
    static CompactTextureStats& Get() {
        static CompactTextureStats stats;
        return stats;
    }

    void Report() {
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds{4}) {
            return;
        }
        const double seconds = std::chrono::duration<double>(now - window).count();
        LOG_INFO(Render_Vulkan,
                 "Texture tables: {:.0f} draws/s, {:.0f} with tables/s; buffer arrays reused {:.1f}% of {:.0f}/s; "
                 "cbuf arrays reused {:.1f}% of {:.0f}/s; per table draw {:.0f} elements, {:.1f} pairs, "
                 "{:.1f} unique views",
                 draws / seconds, table_draws / seconds,
                 decoded + reused ? reused * 100.0 / double(decoded + reused) : 0.0,
                 (decoded + reused) / seconds,
                 cbuf_decoded + cbuf_reused ? cbuf_reused * 100.0 / double(cbuf_decoded + cbuf_reused) : 0.0,
                 (cbuf_decoded + cbuf_reused) / seconds,
                 table_draws ? double(elements) / table_draws : 0.0,
                 table_draws ? double(pairs) / table_draws : 0.0,
                 table_draws ? double(unique_views) / table_draws : 0.0);
        const auto per_second = [seconds](u64 value) { return value / seconds; };
        const auto rate = [](u64 part, u64 total) { return total ? part * 100.0 / double(total) : 0.0; };
        const u64 view_lookups = view_hits + view_misses + view_stale_table + view_stale_generation +
                                 view_stale_memory;
        LOG_INFO(Render_Vulkan,
                 "Texture banks: views reused {:.1f}% of {:.0f}/s (new {:.0f}/s, table {:.0f}/s, "
                 "generation {:.0f}/s, entries {:.0f}/s, unstored {:.0f}/s, retries {:.0f}/s); "
                 "samplers reused {:.1f}% of {:.0f}/s; descriptors reused {:.1f}% of {:.0f}/s "
                 "(patched {:.1f}%, {:.0f} elements rewritten per patch); "
                 "{:.1f} unique samplers, {:.1f} alias prepares and {:.0f} snapshot bytes per reuse",
                 rate(view_hits, view_lookups), per_second(view_lookups), per_second(view_misses),
                 per_second(view_stale_table), per_second(view_stale_generation),
                 per_second(view_stale_memory), per_second(view_unstored), per_second(view_retries),
                 rate(sampler_hits, sampler_hits + sampler_misses),
                 per_second(sampler_hits + sampler_misses),
                 rate(descriptor_hits, descriptor_hits + descriptor_misses + descriptor_patches),
                 per_second(descriptor_hits + descriptor_misses + descriptor_patches),
                 rate(descriptor_patches, descriptor_hits + descriptor_misses + descriptor_patches),
                 descriptor_patches ? double(patched_elements) / descriptor_patches : 0.0,
                 table_draws ? double(unique_samplers) / table_draws : 0.0,
                 view_hits ? double(alias_prepares) / view_hits : 0.0,
                 view_hits ? double(snapshot_bytes) / view_hits : 0.0);
        LOG_INFO(Render_Vulkan,
                 "Draw path: virtual ranges reused {:.1f}% of {:.0f}/s; draws under host conditional "
                 "rendering {:.0f}/s, skipped by a CPU-evaluated condition {:.0f}/s; held guest writes "
                 "{:.0f} regions/s in {:.0f} batches/s",
                 rate(range_hits, range_hits + range_misses), per_second(range_hits + range_misses),
                 per_second(host_conditional_draws), per_second(skipped_draws),
                 per_second(cached_write_regions), per_second(cached_write_batches));
        *this = {};
        window = now;
    }
};

} // namespace VideoCommon
