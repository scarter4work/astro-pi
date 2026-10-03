#include "nukex/core/coverage_trim.hpp"
#include "nukex/core/cube.hpp"

#include <cstdint>
#include <vector>

namespace nukex {

TrimBounds full_coverage_rect(const Cube& cube) {
    TrimBounds t;
    const int w = cube.width;
    const int h = cube.height;
    t.x1 = w - 1;
    t.y1 = h - 1;
    if (w <= 0 || h <= 0) return t;

    const int nc = std::min<int>(cube.channel_config.n_channels,
                                 cube.allocated_channels());
    if (nc <= 0) return t;

    // Peak coverage per slot. A slot that peaks at zero was never filled and
    // takes no part in the decision.
    std::vector<std::uint32_t> peak(static_cast<std::size_t>(nc), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const SubcubeVoxel& v = cube.at(x, y);
            for (int c = 0; c < nc; ++c)
                peak[c] = std::max(peak[c], v.channel(c).welford.count());
        }

    std::vector<std::uint8_t> complete(static_cast<std::size_t>(w) * h, 1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const SubcubeVoxel& v = cube.at(x, y);
            for (int c = 0; c < nc; ++c) {
                if (peak[c] == 0) continue;
                if (v.channel(c).welford.count() < peak[c]) {
                    complete[static_cast<std::size_t>(y) * w + x] = 0;
                    break;
                }
            }
        }

    // Largest all-complete rectangle: per row, the height of the complete run
    // ending at each column, then the largest rectangle in that histogram.
    // O(w*h) with one index stack, against O(w^2 h^2) for the obvious search.
    std::vector<int> height(static_cast<std::size_t>(w), 0);
    std::vector<int> stack;
    stack.reserve(static_cast<std::size_t>(w) + 1);
    long long best = 0;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            height[x] = complete[static_cast<std::size_t>(y) * w + x] ? height[x] + 1 : 0;

        stack.clear();
        for (int x = 0; x <= w; ++x) {
            const int cur = (x < w) ? height[x] : 0;
            while (!stack.empty() && height[stack.back()] >= cur) {
                const int ht = height[stack.back()];
                stack.pop_back();
                const int left  = stack.empty() ? 0 : stack.back() + 1;
                const int right = x - 1;
                const long long area =
                    static_cast<long long>(ht) * (right - left + 1);
                if (ht > 0 && area > best) {
                    best = area;
                    t.x0 = left;
                    t.x1 = right;
                    t.y0 = y - ht + 1;
                    t.y1 = y;
                }
            }
            stack.push_back(x);
        }
    }

    if (best == 0) {                 // nothing complete anywhere: keep the frame
        t.x0 = 0; t.y0 = 0; t.x1 = w - 1; t.y1 = h - 1;
        return t;
    }
    t.applied = (t.x0 != 0 || t.y0 != 0 || t.x1 != w - 1 || t.y1 != h - 1);
    return t;
}

} // namespace nukex
