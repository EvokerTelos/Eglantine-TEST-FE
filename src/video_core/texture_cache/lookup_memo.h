// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace VideoCommon {

/// Direct-mapped memo of descriptor index -> resolved id, valid until the next Reset. Buffer-backed
/// texture arrays bind every record of their table on every draw, and records often repeat the same
/// texture or sampler index; within one draw a repeated index always resolves to the same id.
template <typename Id, size_t Size, typename Key = u32>
class LookupMemo {
    static_assert((Size & (Size - 1)) == 0);
    static_assert(sizeof(Key) == 4 || sizeof(Key) == 8);

public:
    /// Starts a new scope; ids stored before are no longer returned.
    void Reset() noexcept {
        if (++generation == 0) {
            entries.fill({});
            generation = 1;
        }
    }

    [[nodiscard]] const Id* Find(Key key) const noexcept {
        const Entry& entry = entries[Slot(key)];
        if (entry.generation == generation && entry.key == key) {
            return &entry.id;
        }
        return nullptr;
    }

    void Store(Key key, Id id) noexcept {
        entries[Slot(key)] = Entry{key, generation, id};
    }

private:
    static size_t Slot(Key key) noexcept {
        if constexpr (sizeof(Key) == 8) {
            return static_cast<size_t>((key ^ (key >> 29)) & (Size - 1));
        } else {
            return static_cast<size_t>(key & (Size - 1));
        }
    }

    struct Entry {
        Key key{};
        u32 generation{};
        Id id{};
    };

    std::array<Entry, Size> entries{};
    u32 generation{1};
};

} // namespace VideoCommon
