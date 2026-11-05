// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include "common/common_types.h"

namespace Service::AM {
inline std::array<u8, 4> ConvertNativeVideoYuv709(u8 y, u8 u, u8 v, bool full_range) {
    const float luma = full_range ? float(y) : (float(y) - 16.0f) * (255.0f / 219.0f);
    const float cb = (float(u) - 128.0f) * (full_range ? 1.0f : 255.0f / 224.0f);
    const float cr = (float(v) - 128.0f) * (full_range ? 1.0f : 255.0f / 224.0f);
    const auto channel = [](float value) {
        return static_cast<u8>(std::clamp(std::lround(value), 0L, 255L));
    };
    return {channel(luma + 1.5748f * cr), channel(luma - 0.187324f * cb - 0.468124f * cr),
            channel(luma + 1.8556f * cb), 255};
}

// ConvertNativeVideoYuv709 with its per-component terms tabulated: same float operations in the same
// order, so the output is identical, without per-pixel multiplies or lround calls.
class NativeVideoYuv709Table {
public:
    explicit NativeVideoYuv709Table(bool full_range) {
        const float chroma_scale = full_range ? 1.0f : 255.0f / 224.0f;
        for (u32 i = 0; i < 256; ++i) {
            luma[i] = full_range ? float(i) : (float(i) - 16.0f) * (255.0f / 219.0f);
            const float chroma = (float(i) - 128.0f) * chroma_scale;
            r_cr[i] = 1.5748f * chroma;
            g_cb[i] = 0.187324f * chroma;
            g_cr[i] = 0.468124f * chroma;
            b_cb[i] = 1.8556f * chroma;
        }
    }

    void Convert(u8 y, u8 u, u8 v, u8* out) const {
        const float l = luma[y];
        out[0] = Channel(l + r_cr[v]);
        out[1] = Channel(l - g_cb[u] - g_cr[v]);
        out[2] = Channel(l + b_cb[u]);
        out[3] = 255;
    }

private:
    // lround with clamping; the double sum is exact for float inputs, so this matches lround.
    static u8 Channel(float value) {
        if (!(value > 0.0f)) return 0;
        if (value >= 255.0f) return 255;
        return static_cast<u8>(static_cast<double>(value) + 0.5);
    }

    std::array<float, 256> luma;
    std::array<float, 256> r_cr;
    std::array<float, 256> g_cb;
    std::array<float, 256> g_cr;
    std::array<float, 256> b_cb;
};

struct NativeVideoImageLayout {
    u64 size;
    u64 alignment;
    u32 row_bytes;
    u32 image_bytes;
    u32 image_stride;
    // Kind 1: two-plane 8-bit 4:2:0 (NV12) pitch-linear surface, luma at 0 and interleaved
    // chroma at chroma_offset, both planes using row_bytes as pitch.
    bool nv12{};
    u32 chroma_offset{};
    u32 luma_bytes{};
    u32 chroma_bytes{};
};

inline std::optional<NativeVideoImageLayout> CalculateNativeVideoImageLayout(
    std::span<const u32> preference, u32 count) {
    if (preference.size() != 16 || preference[0] != 0 || (preference[1] != 5 && preference[1] != 1) ||
        count == 0 ||
        std::any_of(preference.begin() + 3, preference.end(), [](u32 value) { return value != 0; })) {
        return {};
    }
    const u64 width = (preference[2] & 0xffffU) + 1;
    const u64 height = (preference[2] >> 16) + 1;
    if (preference[1] == 1) {
        // gsv size calculation for kind 1 (run in emulation over a size grid): even dimensions,
        // pitch aligned to 256, luma and chroma row counts aligned to 4, at least 4096 bytes,
        // images packed back to back.
        if (width < 2 || height < 2 || ((width | height) & 1) != 0) return {};
        const u64 pitch = (width + 255) & ~u64{255};
        const u64 chroma_offset = pitch * ((height + 3) & ~u64{3});
        const u64 image_bytes =
            (std::max)(u64{4096}, chroma_offset + pitch * (((height / 2) + 3) & ~u64{3}));
        if (image_bytes > (std::numeric_limits<u32>::max)()) return {};
        return NativeVideoImageLayout{image_bytes * count, 4096, static_cast<u32>(pitch),
                                      static_cast<u32>(image_bytes), static_cast<u32>(image_bytes),
                                      true, static_cast<u32>(chroma_offset),
                                      static_cast<u32>(pitch * height), static_cast<u32>(pitch * (height / 2))};
    }
    const u64 row_bytes = ((width + 15) & ~u64{15}) * 4;
    const u64 image_bytes = (std::max)(u64{4096}, row_bytes * ((height + 127) & ~u64{127}));
    const u64 stride = (image_bytes + 8191) & ~u64{8191};
    if (stride > (std::numeric_limits<u32>::max)()) return {};
    return NativeVideoImageLayout{
        stride * (count - 1) + image_bytes, 4096, static_cast<u32>(row_bytes),
        static_cast<u32>(image_bytes), static_cast<u32>(stride)};
}
} // namespace Service::AM
