#pragma once
#include "nukex/io/image.hpp"
#include "nukex/core/luminance_spec.hpp"
#include "nukex/core/cube.hpp"

namespace nukex {
class OutputAssembler {
public:
    struct OutputImages {
        Image stacked;
        Image noise_map;
        Image quality_map;
    };
    static Image assemble_quality_map(const Cube& cube);

    /// The stack's REALISED pixel-to-pixel scatter, one channel, from
    /// SubcubeVoxel::local_rms. Distinct from noise_map, which is the
    /// PREDICTED uncertainty of each estimate; the two answer different
    /// questions and only the predicted one existed before.
    static Image assemble_measured_noise(const Cube& cube);

    /// The three numbers the console prints, from ONE computation so they
    /// reconcile by construction: `ratio` IS measured_median / predicted_median.
    /// (A median of per-pixel ratios is not the ratio of medians; printing one
    /// beside the other two made the line contradict itself by 2-3%.)
    ///
    /// The measured map is the spatial kernel's scatter on its luminance
    /// window, so a 3-channel predicted map is combined the same way,
    /// sqrt((0.2126 s0)^2 + (0.7152 s1)^2 + (0.0722 s2)^2) on channels 0..2,
    /// before its median is taken. A ratio above 1 means the stack is noisier
    /// than its own model says -- what an estimator injecting noise looks like
    /// from outside.
    struct NoiseCheck {
        double measured_median  = 0.0;
        double predicted_median = 0.0;
        double ratio            = 0.0;
        bool   comparable       = false;   ///< false when either median is 0
    };
    static NoiseCheck noise_check(const Image& measured, const Image& predicted,
                                  LuminanceSpec luminance = LuminanceSpec{});

    /// Structure-blind noise of a one-channel plane: lag-8 differences in
    /// both directions, each centred on its own median, pooled MAD, over
    /// 1.4826 / sqrt(2). Smooth structure cancels; correlated neighbours are
    /// cleared by the lag. The same estimator the spatial kernel applies per
    /// window, here over the whole plane.
    static double plane_noise_lag8(const Image& plane, int channel = 0);

    /// Split the measured noise into what varies between frames and what
    /// does not. Measured on four real sessions with an odd/even split: on a
    /// 24 MP OSC stack without flats 41-59% of the measured variance was a
    /// FIXED pattern -- pixel response and debayer residuals, present in every
    /// frame and invisible to any across-frame model -- while the stochastic
    /// part was within 10-14% of the model on every channel, exactly as on
    /// mono. A single "measured / predicted" number reads that as a bad
    /// estimator. This is the honest reading.
    ///
    /// `half_even` and `half_odd` are stacks of the even- and odd-indexed
    /// samples (GPUExecutor::HalfStackFn). Their difference cancels anything
    /// common to both; its structure-blind noise, halved, is the full stack's
    /// stochastic noise (each half carries sqrt(2) of the full stack's, and
    /// the difference sqrt(2) of a half's).
    struct NoiseDecomposition {
        double measured   = 0.0;   ///< the spatial kernel's median local_rms
        double stochastic = 0.0;   ///< from the half-stack difference
        double fixed      = 0.0;   ///< sqrt(max(0, measured^2 - stochastic^2))
        double fixed_share = 0.0;  ///< fixed^2 / measured^2
        bool   valid = false;
    };
    static NoiseDecomposition noise_decomposition(double measured_median,
                                                  const Image& half_even,
                                                  const Image& half_odd,
                                                  LuminanceSpec luminance = LuminanceSpec{});
};
} // namespace nukex
