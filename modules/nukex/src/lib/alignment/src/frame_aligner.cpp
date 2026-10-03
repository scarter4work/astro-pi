#include "nukex/alignment/frame_aligner.hpp"

#include <algorithm>
#include <cmath>

#include <cmath>

namespace nukex {

FrameAligner::FrameAligner(const Config& config) : config_(config) {}

std::vector<int> FrameAligner::chain_order(const std::vector<std::pair<int, double>>& anchors,
                                           int frame_index, double frame_time) {
    std::vector<int> order(anchors.size());
    for (std::size_t i = 0; i < anchors.size(); ++i) order[i] = static_cast<int>(i);
    // Nearest in TIME first: the least drift between the two frames, so the
    // best chance their top-K star sets still overlap. Index distance only
    // when a time is unknown -- processing order is directory order, which
    // need not be chronological.
    auto key = [&](int i) {
        const auto& a = anchors[static_cast<std::size_t>(i)];
        if (frame_time > 0.0 && a.second > 0.0) return std::fabs(a.second - frame_time);
        return static_cast<double>(std::abs(a.first - frame_index)) * 1e9;   // rank below any timed anchor
    };
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return key(x) < key(y); });
    return order;
}

bool FrameAligner::try_chain(const StarCatalog& stars, int frame_index, double frame_time,
                             AlignmentResult& result) const {
    if (anchors_.empty()) return false;

    std::vector<std::pair<int, double>> it;
    it.reserve(anchors_.size());
    for (const auto& a : anchors_) it.emplace_back(a.index, a.time);
    const std::vector<int> order = chain_order(it, frame_index, frame_time);

    const int limit = std::min<int>(config_.max_anchor_attempts,
                                    static_cast<int>(order.size()));
    for (int i = 0; i < limit; ++i) {
        const Anchor* a = &anchors_[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])];
        auto matches = StarMatcher::match(stars, a->catalog, config_.match_config);
        AlignmentResult step = HomographyComputer::compute(
            stars, a->catalog, matches, config_.homography_config);
        if (step.alignment_failed) continue;

        // H_ref<-frame = H_ref<-anchor * H_anchor<-frame. Composing the
        // TRANSFORMS, so the frame is still resampled exactly once; chained
        // resampling would blur every rescued frame.
        result.H = a->H_ref_from_anchor.compose(step.H);
        result.match = step.match;
        result.alignment_failed = false;
        result.weight_penalty = 1.0f;
        result.chained = true;
        result.chained_via = a->index;
        return true;
    }
    return false;
}

FrameAligner::AlignedFrame FrameAligner::align(const Image& frame, int frame_index, double obs_time) {
    AlignedFrame result;
    result.frame_index = frame_index;

    // Detect stars
    result.stars = StarDetector::detect(frame, config_.star_config);

    if (!has_ref_) {
        // No reference was set: fall back to the first frame to arrive.
        adopt_reference(frame, result.stars, frame_index);
    }

    // Measure the channel disagreement on the UNWARPED frame, using the stars
    // we already have. Lateral colour is a property of this exposure through
    // this optic at this altitude; measuring it after warping would mix it
    // with the frame-to-frame transform.
    if (config_.register_channels && frame.n_channels() >= 2
        && !result.stars.empty()) {
        // Same expression star detection used, so the two cannot disagree
        // if a caller ever sets star_config.channel.
        result.channels = measure_channel_transforms(
            frame, result.stars,
            resolve_detection_channel(config_.star_config.channel,
                                      frame.n_channels()),
            config_.channel_config);
    }

    // The near-identity skip. A well-corrected rig should not pay for
    // interpolation it does not need, and this is what makes "always on"
    // affordable without exposing a threshold for users to argue about.
    // The radius is the frame's own corner, where the scale term is largest.
    const double corner_radius =
        std::hypot(frame.width() / 2.0, frame.height() / 2.0);
    const bool channels_matter =
        !result.channels.empty()
        && !result.channels.negligible(corner_radius,
                                       kNegligibleChannelShiftPx);
    // result.channels is kept even when it does not matter enough to apply --
    // it is a measurement, and the Fit rung it recorded (including a
    // deliberate Fit::Identity from the fallback ladder) is reported
    // downstream. Only the decision to warp with it depends on
    // channels_matter; each warp call below passes the transform only when
    // it does.

    if (frame_index == ref_index_) {
        // This IS the reference. Matching it against its own catalog would
        // only reintroduce fit noise into a transform that is exactly the
        // identity by construction.
        //
        // Its CHANNELS are a different matter. Every other frame registers to
        // this one, so if its own channels disagree the whole stack inherits
        // that. Warp it with the identity homography when there is a channel
        // correction to make, and clone it when there is not.
        result.alignment.H = HomographyMatrix::identity();
        result.alignment.match.success = true;
        result.alignment.match.n_inliers = result.stars.size();
        if (channels_matter) {
            result.image = HomographyComputer::warp(frame, result.alignment.H,
                                                    frame.width(), frame.height(),
                                                    result.channels,
                                                    result.coverage);
        } else {
            // Cloned, not warped: it covers itself completely, so the mask
            // stays empty and reads as fully covered.
            result.image = frame.clone();
        }
        return result;
    }

    // Match stars to reference using triangle similarity matching.
    auto matches = StarMatcher::match(result.stars, ref_catalog_,
                                       config_.match_config);

    // Compute homography
    result.alignment = HomographyComputer::compute(
        result.stars, ref_catalog_, matches, config_.homography_config);

    // A meridian flip needs no correction. The matcher is rotation-invariant,
    // so the homography it solves for a flipped frame ALREADY contains the
    // 180-degree rotation -- that rotation is what maps the frame onto the
    // reference. `is_meridian_flipped` is therefore a report, not a repair:
    // it is carried into frame_stats and logged, and nothing transforms H.
    //
    // Stripping the rotation back out (which this did until v5.0.3.1) lays
    // the frame down rotated 180 degrees from where it belongs, and the
    // aligner reports success while doing it.

    // A single reference cannot bridge a long session. If the direct match
    // failed, try to reach the reference through an already-aligned
    // neighbour: neighbouring frames have barely drifted, so they match, and
    // composing their transforms walks the chain outward to the ends of the
    // session. Fallback only -- a frame that matched directly never gets
    // here, which is what keeps fully-aligning corpora bit-identical.
    if (result.alignment.alignment_failed && config_.chain_through_anchors) {
        if (try_chain(result.stars, frame_index, obs_time, result.alignment)) {
            ++chained_count_;
        }
    }

    // Every frame that reached the reference -- directly or through the chain
    // -- can serve as a stepping stone for the next one.
    if (!result.alignment.alignment_failed) {
        anchors_.push_back(Anchor{frame_index, obs_time, result.stars, result.alignment.H});
    }

    if (!result.alignment.alignment_failed) {
        result.image = HomographyComputer::warp(
            frame, result.alignment.H, ref_width_, ref_height_,
            channels_matter ? result.channels : ChannelTransforms{},
            result.coverage);
    } else if (channels_matter) {
        // Failed alignment: the frame is still stacked, with its weight
        // penalised, so its channels still have to be put right. The
        // homography is the identity because there isn't a usable one.
        result.image = HomographyComputer::warp(
            frame, HomographyMatrix::identity(),
            frame.width(), frame.height(), result.channels,
            result.coverage);
    } else {
        result.image = frame.clone();
    }

    return result;
}

void FrameAligner::set_reference(const Image& frame, int frame_index) {
    adopt_reference(frame, StarDetector::detect(frame, config_.star_config),
                    frame_index);
}

void FrameAligner::adopt_reference(const Image& frame, const StarCatalog& stars,
                                   int frame_index) {
    ref_catalog_ = stars;
    ref_width_   = frame.width();
    ref_height_  = frame.height();
    ref_index_   = frame_index;
    has_ref_     = true;
}

void FrameAligner::reset() {
    ref_catalog_ = StarCatalog{};
    has_ref_ = false;
    ref_width_ = 0;
    ref_height_ = 0;
    ref_index_ = -1;
}

} // namespace nukex
