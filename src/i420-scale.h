#pragma once

// Filtered I420 resampling for screen-share frames.
//
// Every frame a ZoomSource publishes is letterboxed into the source's fixed
// canvas (1920x1080 by default) so a scene item never changes size under the
// operator. Participant video is nearly always delivered at a canvas size, so
// that scale is usually a copy. A screen share almost never is: a MacBook
// shares 2560x1600, a 1440p monitor 2560x1440, a window whatever size it is
// (1728x1117 was observed live). Decimating those by nearest neighbour at a
// non-integer ratio -- 2560 -> 1728 is 1.48x -- drops roughly every third
// column and row outright, so one-pixel strokes in shared text vanish, double
// up, and shimmer as the presenter scrolls. That is the content a share
// exists to carry.
//
// This is a separable, fixed-point resampler: an AREA (box) filter when
// shrinking, so every source pixel contributes in proportion to the area it
// covers, and BILINEAR when enlarging. Weights per output sample sum to
// exactly 1 << kScaleWeightBits, so flat regions stay bit-exact flat -- a
// filter that brightens or darkens a slide background by one code value is
// visible as a band at the letterbox edge.
//
// It is deliberately used for screen share only. Eight participant feeds go
// through this path at up to 30 fps each, nearly always unscaled, and their
// cost profile is not something to change as a side effect.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace i420_scale_detail {

constexpr int kScaleWeightBits = 14;
constexpr int32_t kScaleWeightOne = 1 << kScaleWeightBits;

// Per-output-sample filter taps along one axis.
struct AxisTaps {
    std::vector<uint32_t> first;  // first source index
    std::vector<uint32_t> offset; // into weights
    std::vector<uint32_t> count;
    std::vector<int32_t>  weights;
};

inline void normalise_weights(int32_t *w, uint32_t n)
{
    // Rounding each weight independently can leave the sum one or two units
    // off; give the residual to the largest tap so the sum is exact.
    int32_t sum = 0;
    uint32_t largest = 0;
    for (uint32_t i = 0; i < n; ++i) {
        sum += w[i];
        if (w[i] > w[largest]) largest = i;
    }
    w[largest] += kScaleWeightOne - sum;
}

inline AxisTaps compute_axis_taps(uint32_t src_n, uint32_t dst_n)
{
    AxisTaps taps;
    taps.first.resize(dst_n);
    taps.offset.resize(dst_n);
    taps.count.resize(dst_n);

    if (src_n == dst_n) {
        taps.weights.assign(dst_n, kScaleWeightOne);
        for (uint32_t i = 0; i < dst_n; ++i) {
            taps.first[i] = i;
            taps.offset[i] = i;
            taps.count[i] = 1;
        }
        return taps;
    }

    const double scale = static_cast<double>(src_n) / dst_n;
    if (scale > 1.0) {
        // Area: output i covers source interval [i*scale, (i+1)*scale).
        taps.weights.reserve(static_cast<size_t>(dst_n) *
                             (static_cast<size_t>(std::ceil(scale)) + 1));
        for (uint32_t i = 0; i < dst_n; ++i) {
            const double lo = i * scale;
            const double hi = std::min<double>((i + 1) * scale, src_n);
            const uint32_t j0 = static_cast<uint32_t>(lo);
            uint32_t j1 = static_cast<uint32_t>(std::ceil(hi));
            if (j1 > src_n) j1 = src_n;
            if (j1 <= j0) j1 = j0 + 1;
            taps.first[i] = j0;
            taps.offset[i] = static_cast<uint32_t>(taps.weights.size());
            taps.count[i] = j1 - j0;
            for (uint32_t j = j0; j < j1; ++j) {
                const double overlap =
                    std::min<double>(hi, j + 1.0) - std::max<double>(lo, j);
                taps.weights.push_back(static_cast<int32_t>(
                    std::lround(std::max(0.0, overlap) / scale *
                                kScaleWeightOne)));
            }
            normalise_weights(taps.weights.data() + taps.offset[i],
                              taps.count[i]);
        }
        return taps;
    }

    // Bilinear, pixel-centre aligned, edges clamped.
    taps.weights.reserve(static_cast<size_t>(dst_n) * 2);
    for (uint32_t i = 0; i < dst_n; ++i) {
        double pos = (i + 0.5) * scale - 0.5;
        pos = std::clamp(pos, 0.0, static_cast<double>(src_n - 1));
        const uint32_t j0 = static_cast<uint32_t>(pos);
        const double frac = pos - j0;
        taps.first[i] = j0;
        taps.offset[i] = static_cast<uint32_t>(taps.weights.size());
        if (j0 + 1 >= src_n || frac <= 0.0) {
            taps.count[i] = 1;
            taps.weights.push_back(kScaleWeightOne);
        } else {
            taps.count[i] = 2;
            const int32_t w1 =
                static_cast<int32_t>(std::lround(frac * kScaleWeightOne));
            taps.weights.push_back(kScaleWeightOne - w1);
            taps.weights.push_back(w1);
        }
    }
    return taps;
}


template <uint32_t N>
inline void horizontal_fixed_taps(const uint8_t *tmp, uint32_t src_w,
                                  uint32_t rows, uint8_t *dst, uint32_t dst_w,
                                  uint32_t dst_stride,
                                  const std::vector<uint32_t> &first,
                                  const std::vector<int32_t> &weights)
{
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t *row = tmp + static_cast<size_t>(y) * src_w;
        uint8_t *out = dst + static_cast<size_t>(y) * dst_stride;
        const int32_t *w = weights.data();
        for (uint32_t x = 0; x < dst_w; ++x, w += N) {
            const uint8_t *s = row + first[x];
            int32_t sum = kScaleWeightOne / 2;
            for (uint32_t k = 0; k < N; ++k)
                sum += s[k] * w[k];
            out[x] = static_cast<uint8_t>(sum >> kScaleWeightBits);
        }
    }
}

} // namespace i420_scale_detail

// Resample one 8-bit plane. `scratch` holds the intermediate between the two
// passes and is grown as needed; pass the same vector every frame to avoid
// reallocating.
//
// Vertical pass first, then horizontal. The vertical pass is a weighted sum
// of whole rows -- linear in memory and auto-vectorised -- and doing it first
// means the horizontal pass, whose per-sample taps are a gather the compiler
// cannot vectorise, only runs over the OUTPUT row count. Accumulators are 32
// bit: 255 * kScaleWeightOne is ~4.2M, far inside int32, and 64-bit lanes
// halve the vector width. It runs on the plugin's single video dispatch lane,
// which every camera feed shares, so it has to stay cheap even for a share
// playing video at 30 fps. Measured at -O2 on Apple Silicon (2026-11-14),
// whole I420 frame into 1920x1080: 2560x1600 3.6 ms, 3840x2160 2.7 ms,
// 1280x720 1.9 ms -- against 2.8 / 1.8 / 1.9 ms for the nearest-neighbour
// path it replaces, and 10.7 ms for this filter's first, horizontal-first
// int64 draft.
inline void scale_plane_filtered(const uint8_t *src, uint32_t src_w,
                                 uint32_t src_h, uint32_t src_stride,
                                 uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                                 uint32_t dst_stride,
                                 std::vector<uint8_t> &scratch)
{
    using namespace i420_scale_detail;
    if (!src || !dst || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        return;

    if (src_w == dst_w && src_h == dst_h) {
        for (uint32_t y = 0; y < dst_h; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                        src + static_cast<size_t>(y) * src_stride, dst_w);
        return;
    }

    const AxisTaps xt = compute_axis_taps(src_w, dst_w);
    const AxisTaps yt = compute_axis_taps(src_h, dst_h);

    // Vertical pass: dst_h rows of src_w samples.
    const size_t tmp_size = static_cast<size_t>(src_w) * dst_h;
    if (scratch.size() < tmp_size) scratch.resize(tmp_size);
    uint8_t *tmp = scratch.data();
    std::vector<int32_t> acc(src_w);
    constexpr int32_t kHalf = kScaleWeightOne / 2;
    for (uint32_t y = 0; y < dst_h; ++y) {
        int32_t *a = acc.data();
        std::fill(a, a + src_w, kHalf);
        const int32_t *w = yt.weights.data() + yt.offset[y];
        for (uint32_t k = 0; k < yt.count[y]; ++k) {
            const uint8_t *row =
                src + static_cast<size_t>(yt.first[y] + k) * src_stride;
            const int32_t wk = w[k];
            for (uint32_t x = 0; x < src_w; ++x)
                a[x] += row[x] * wk;
        }
        uint8_t *out = tmp + static_cast<size_t>(y) * src_w;
        for (uint32_t x = 0; x < src_w; ++x) {
            // Weights are non-negative and sum to one, so the result is
            // already in 0..255; no clamp needed.
            out[x] = static_cast<uint8_t>(a[x] >> kScaleWeightBits);
        }
    }

    // Horizontal pass over the dst_h intermediate rows. Share ratios give 1-4
    // taps per sample; padding every sample to the same count (zero weights,
    // first index pulled back so the taps stay inside the row) lets a fixed
    // trip count unroll. Anything wider takes the generic loop.
    uint32_t max_taps = 1;
    for (uint32_t x = 0; x < dst_w; ++x)
        max_taps = std::max(max_taps, xt.count[x]);
    if (max_taps <= 4 && max_taps <= src_w) {
        const uint32_t n = max_taps;
        std::vector<uint32_t> first(dst_w);
        std::vector<int32_t> weights(static_cast<size_t>(dst_w) * n, 0);
        for (uint32_t x = 0; x < dst_w; ++x) {
            uint32_t f = xt.first[x];
            if (f + n > src_w) f = src_w - n;
            first[x] = f;
            const uint32_t shift = xt.first[x] - f;
            for (uint32_t k = 0; k < xt.count[x]; ++k)
                weights[static_cast<size_t>(x) * n + shift + k] =
                    xt.weights[xt.offset[x] + k];
        }
        switch (n) {
        case 1: horizontal_fixed_taps<1>(tmp, src_w, dst_h, dst, dst_w, dst_stride, first, weights); break;
        case 2: horizontal_fixed_taps<2>(tmp, src_w, dst_h, dst, dst_w, dst_stride, first, weights); break;
        case 3: horizontal_fixed_taps<3>(tmp, src_w, dst_h, dst, dst_w, dst_stride, first, weights); break;
        default: horizontal_fixed_taps<4>(tmp, src_w, dst_h, dst, dst_w, dst_stride, first, weights); break;
        }
        return;
    }
    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint8_t *row = tmp + static_cast<size_t>(y) * src_w;
        uint8_t *out = dst + static_cast<size_t>(y) * dst_stride;
        for (uint32_t x = 0; x < dst_w; ++x) {
            const uint8_t *s = row + xt.first[x];
            const int32_t *w = xt.weights.data() + xt.offset[x];
            int32_t sum = kHalf;
            for (uint32_t k = 0; k < xt.count[x]; ++k)
                sum += s[k] * w[k];
            out[x] = static_cast<uint8_t>(sum >> kScaleWeightBits);
        }
    }
}

// Letterbox an I420 frame into a dst_w x dst_h canvas (contiguous Y, U, V
// planes, no padding), black borders. Same fit and even-aligned offsets as
// the nearest-neighbour scale_i420_letterbox() in zoom-source.cpp, so a
// source that switches between the two never shifts its picture.
inline void scale_i420_letterbox_filtered(const uint8_t *src_y,
                                          const uint8_t *src_u,
                                          const uint8_t *src_v,
                                          uint32_t src_w, uint32_t src_h,
                                          uint8_t *dst, uint32_t dst_w,
                                          uint32_t dst_h,
                                          std::vector<uint8_t> &scratch)
{
    const size_t dst_y_size = static_cast<size_t>(dst_w) * dst_h;
    const size_t dst_uv_size = dst_y_size / 4;
    uint8_t *dst_y = dst;
    uint8_t *dst_u = dst_y + dst_y_size;
    uint8_t *dst_v = dst_u + dst_uv_size;

    std::memset(dst_y, 0, dst_y_size);
    std::memset(dst_u, 128, dst_uv_size);
    std::memset(dst_v, 128, dst_uv_size);

    if (!src_y || !src_u || !src_v || src_w < 2 || src_h < 2 ||
        dst_w < 2 || dst_h < 2)
        return;

    uint32_t fit_w = dst_w;
    uint32_t fit_h = static_cast<uint32_t>(
        (static_cast<uint64_t>(dst_w) * src_h) / src_w);
    if (fit_h > dst_h) {
        fit_h = dst_h;
        fit_w = static_cast<uint32_t>(
            (static_cast<uint64_t>(dst_h) * src_w) / src_h);
    }
    fit_w = std::max<uint32_t>(2, fit_w & ~1u);
    fit_h = std::max<uint32_t>(2, fit_h & ~1u);
    const uint32_t off_x = ((dst_w - fit_w) / 2) & ~1u;
    const uint32_t off_y = ((dst_h - fit_h) / 2) & ~1u;

    scale_plane_filtered(src_y, src_w, src_h, src_w,
                         dst_y + static_cast<size_t>(off_y) * dst_w + off_x,
                         fit_w, fit_h, dst_w, scratch);
    scale_plane_filtered(src_u, src_w / 2, src_h / 2, src_w / 2,
                         dst_u + static_cast<size_t>(off_y / 2) * (dst_w / 2) +
                             off_x / 2,
                         fit_w / 2, fit_h / 2, dst_w / 2, scratch);
    scale_plane_filtered(src_v, src_w / 2, src_h / 2, src_w / 2,
                         dst_v + static_cast<size_t>(off_y / 2) * (dst_w / 2) +
                             off_x / 2,
                         fit_w / 2, fit_h / 2, dst_w / 2, scratch);
}
