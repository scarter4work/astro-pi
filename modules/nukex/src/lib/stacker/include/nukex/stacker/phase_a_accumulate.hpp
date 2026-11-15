#pragma once

#include "nukex/core/coverage_mask.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/parallel_for.hpp"
#include "nukex/core/running_accumulators.hpp"
#include "nukex/io/image.hpp"
#include <cstddef>

namespace nukex {

// ── Phase A accumulation targets ─────────────────────────────────────
//
// Phase A's per-sample state lives in the voxel record itself when the
// record is in memory, and in RunningAccumulators when it is file-backed (so
// a frame does not dirty every page of the file). Both expose the same two
// operations per pixel, and accumulate_frame is written once against them.

/// Accumulates straight into the voxel record.
struct CubeSink {
    Cube& cube;
    struct Pixel {
        SubcubeVoxel& v;
        void add(int c, float value, bool covered) {
            accumulate_sample(v.channel(c).welford, v.channel(c).histogram, value, covered);
        }
        void count_frame() { v.n_frames++; }
    };
    Pixel at(int x, int y, std::size_t) { return Pixel{cube.at(x, y)}; }
};

/// Accumulates into the RAM-resident RunningAccumulators.
struct RamSink {
    RunningAccumulators& acc;
    struct Pixel {
        RunningAccumulators& a;
        std::size_t p;
        void add(int c, float value, bool covered) {
            auto& ch = a.channel(p, c);
            accumulate_sample(ch.welford, ch.histogram, value, covered);
        }
        void count_frame() { a.n_frames(p)++; }
    };
    Pixel at(int, int, std::size_t p) { return Pixel{acc, p}; }
};

/// How one frame's planes map onto cube slots.
enum class AccumulateKind {
    OSC_WITH_LUMA,   ///< planes 0-2 -> slots[0..2], plus rec709 L -> slots[3]
    THREE_PLANE,     ///< planes 0-2 -> slots[0..2]
    MONO             ///< plane 0 -> slots[0]
};

/// Fold one aligned frame into every pixel's accumulators over a w x h grid.
///
/// Row-parallel through parallel_for_dynamic: a pixel's accumulators belong
/// to that pixel alone, and each voxel still sees its samples in frame order,
/// so the result is bit-identical to the serial loop this replaced.
template <typename Sink>
void accumulate_frame(Sink sink, const Image& img, const CoverageMask& cov,
                      int w, int h, AccumulateKind kind, const int slots[4]) {
    // An empty mask means the frame was cloned rather than warped, so it
    // covers itself completely.
    auto covered_at = [&cov](int c, int y, int x) {
        return cov.empty() || cov.covered(c, y, x);
    };
    parallel_for_dynamic(h, 8, [&](int y, int) {
        Sink s = sink;
        for (int x = 0; x < w; ++x) {
            auto px = s.at(x, y, static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x);
            switch (kind) {
                case AccumulateKind::OSC_WITH_LUMA: {
                    const float r = img.at(x, y, 0);
                    const float g = img.at(x, y, 1);
                    const float b = img.at(x, y, 2);
                    const float l = 0.299f * r + 0.587f * g + 0.114f * b;
                    const bool cr = covered_at(0, y, x);
                    const bool cg = covered_at(1, y, x);
                    const bool cb = covered_at(2, y, x);
                    px.add(slots[0], r, cr);
                    px.add(slots[1], g, cg);
                    px.add(slots[2], b, cb);
                    // Synthetic luminance mixes all three planes, so it is
                    // only a measurement where all three are.
                    px.add(slots[3], l, cr && cg && cb);
                    if (cr || cg || cb) px.count_frame();
                    break;
                }
                case AccumulateKind::THREE_PLANE: {
                    const bool cr = covered_at(0, y, x);
                    const bool cg = covered_at(1, y, x);
                    const bool cb = covered_at(2, y, x);
                    px.add(slots[0], img.at(x, y, 0), cr);
                    px.add(slots[1], img.at(x, y, 1), cg);
                    px.add(slots[2], img.at(x, y, 2), cb);
                    if (cr || cg || cb) px.count_frame();
                    break;
                }
                case AccumulateKind::MONO: {
                    const bool c0 = covered_at(0, y, x);
                    px.add(slots[0], img.at(x, y, 0), c0);
                    if (c0) px.count_frame();
                    break;
                }
            }
        }
    });
}

} // namespace nukex
