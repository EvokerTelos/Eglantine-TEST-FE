// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
#include "common/common_types.h"

namespace VideoCommon {
struct StorageAddressRange { u64 address; u32 size; };
struct StorageRangeGroup {
    u64 address{}, end{};
    std::vector<size_t> members;
};
// Overlapping virtual views must share one backing allocation so writes through
// one descriptor remain visible through every alias in the same dispatch.
/// Groups into groups[0, returned count), reusing the storage of order and groups (and of the
/// groups' member lists) from earlier calls: this runs on every draw.
inline size_t GroupStorageRanges(std::span<const StorageAddressRange> ranges, std::vector<size_t>& order,
                                 std::vector<StorageRangeGroup>& groups) {
    order.clear();
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (!ranges[i].address || !ranges[i].size) continue;
        if (ranges[i].address > std::numeric_limits<u64>::max() - ranges[i].size)
            throw std::overflow_error("Storage address range overflow");
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ranges[a].address < ranges[b].address; });
    size_t count = 0;
    for (size_t i : order) {
        const u64 address = ranges[i].address, end = address + ranges[i].size;
        if (count == 0 || address >= groups[count - 1].end) {
            if (count == groups.size()) groups.emplace_back();
            StorageRangeGroup& group = groups[count++];
            group.address = address;
            group.end = end;
            group.members.clear();
            group.members.push_back(i);
        } else {
            StorageRangeGroup& group = groups[count - 1];
            group.end = std::max(group.end, end);
            group.members.push_back(i);
        }
    }
    return count;
}

inline std::vector<StorageRangeGroup> GroupStorageRanges(std::span<const StorageAddressRange> ranges) {
    std::vector<size_t> order;
    std::vector<StorageRangeGroup> groups;
    groups.resize(GroupStorageRanges(ranges, order, groups));
    return groups;
}
} // namespace VideoCommon
