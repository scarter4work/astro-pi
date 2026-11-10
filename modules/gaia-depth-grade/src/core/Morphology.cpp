#include "Morphology.h"

#include "Numerics.h"
#include "Parallel.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace gdg {

namespace {

// scipy NI_MinOrMaxFilter1D on one line of n samples (stride `stride`), flat window of `size`,
// scipy's internal `origin`: out[i] = op(in[i - size/2 - origin .. i - size/2 - origin + size-1])
// with the reflect extension. Computed by van Herk / Gil-Werman (block prefix and suffix
// extrema), which is exact for min and max. Works in place (the line is copied out first).
template <bool kMax>
void minmax_line(const double* in, size_t stride, long n, long size, long origin, std::vector<double>& ext,
                 std::vector<double>& g, std::vector<double>& h, double* out) {
    auto op = [](double a, double b) { return kMax ? std::max(a, b) : std::min(a, b); };
    const long before = size / 2 + origin;
    const long len = n + size - 1;
    ext.resize(static_cast<size_t>(len));
    g.resize(static_cast<size_t>(len));
    h.resize(static_cast<size_t>(len));
    for (long i = 0; i < len; ++i) ext[i] = in[static_cast<size_t>(reflect_index(i - before, n)) * stride];
    for (long i = 0; i < len; ++i) g[i] = (i % size == 0) ? ext[i] : op(g[i - 1], ext[i]);
    for (long i = len - 1; i >= 0; --i) h[i] = (i % size == size - 1 || i == len - 1) ? ext[i] : op(h[i + 1], ext[i]);
    for (long i = 0; i < n; ++i) out[static_cast<size_t>(i) * stride] = op(h[i], g[i + size - 1]);
}

// scipy _min_or_max_filter with a size (separable): the 1-D filter along axis 0 (y), then 1 (x).
template <bool kMax>
void minmax_filter(Image& im, long size, long origin) {
    if (size <= 1) return;  // scipy skips axes of size 1
    const long W = im.width, H = im.height;
    double* d = im.data.data();
    parallel_for_if(static_cast<long long>(W) * H * 4, static_cast<int>(W), [&](int x) {
        std::vector<double> ext, g, h;
        minmax_line<kMax>(d + x, static_cast<size_t>(W), H, size, origin, ext, g, h, d + x);
    });
    parallel_for_if(static_cast<long long>(W) * H * 4, static_cast<int>(H), [&](int y) {
        std::vector<double> ext, g, h;
        minmax_line<kMax>(d + static_cast<long>(y) * W, 1, W, size, origin, ext, g, h, d + static_cast<long>(y) * W);
    });
}

}  // namespace

Image grey_closing(const Image& mono, int size) {
    if (mono.channels != 1) throw std::invalid_argument("grey_closing: mono image required");
    if (size < 1) throw std::invalid_argument("grey_closing: size must be >= 1");
    Image out = mono;
    if (out.data.empty()) return out;
    // grey_dilation negates the user origin (0) and, for an even size, subtracts 1.
    const long dilation_origin = (size % 2 == 0) ? -1 : 0;
    minmax_filter<true>(out, size, dilation_origin);
    minmax_filter<false>(out, size, 0);
    return out;
}

}  // namespace gdg
