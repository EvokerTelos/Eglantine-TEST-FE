// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"

namespace VideoCommon {

class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
public:
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
    }

    void Reset() noexcept {
        recent.fill(NO_RANGE);
        // Only the pages tracked since the last reset can be set.
        if (dirty_begin < dirty_end) {
            std::fill(pages.begin() + dirty_begin, pages.begin() + dirty_end, 0ULL);
        }
        dirty_begin = (std::numeric_limits<size_t>::max)();
        dirty_end = 0;
    }

    void Track(u64 offset, u64 size) noexcept {
        // Consecutive draws keep binding the same ranges (a vertex buffer can span tens of MiB);
        // tracking a range again changes nothing until the next reset.
        for (const Range& range : recent) {
            if (range.offset == offset && range.size == size) {
                return;
            }
        }
        recent[recent_next++ % recent.size()] = Range{offset, size};
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        dirty_begin = (std::min)(dirty_begin, page);
        dirty_end = (std::max)(dirty_end, page_end + 1);
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            pages[i] = ~u64{0};
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return false;
        }
        if (IsPageUsed(page, offset, size)) {
            return true;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            if (pages[i] != 0) {
                return true;
            }
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        return IsPageUsed(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

private:
    void TrackPage(u64 page, u64 offset, u64 size) noexcept {
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = std::min<size_t>(size, BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        pages[page] |= (~u64{0} & mask) << first_bit;
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const noexcept {
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = std::min<size_t>(size, BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        const size_t mask2 = (~u64{0} & mask) << first_bit;
        return (pages[page] & mask2) != 0;
    }

private:
    struct Range {
        u64 offset;
        u64 size;
    };
    static constexpr Range NO_RANGE{(std::numeric_limits<u64>::max)(), 0};

    std::vector<u64> pages;
    /// A draw can bind more than four ranges of one buffer (storage buffers carved from one pool):
    /// with too few slots each draw forgot the others and filled their pages again.
    std::array<Range, 16> recent = [] {
        std::array<Range, 16> ranges;
        ranges.fill(NO_RANGE);
        return ranges;
    }();
    size_t recent_next{};
    size_t dirty_begin{(std::numeric_limits<size_t>::max)()};
    size_t dirty_end{};
};

} // namespace VideoCommon
