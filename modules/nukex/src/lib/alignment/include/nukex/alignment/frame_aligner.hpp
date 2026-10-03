#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/alignment/star_detector.hpp"

#include <utility>
#include <vector>
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/alignment/channel_registration.hpp"
#include "nukex/core/coverage_mask.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// High-level frame alignment: detect → match → homography → warp.
///
/// The reference is normally chosen by the caller via set_reference() from a
/// measurement pass over every frame (see reference_selector.hpp). If none is
/// set, the first frame to arrive becomes the reference — the historical
/// behaviour, kept as a fallback for callers with a single frame or no
/// measurements. Subsequent frames are aligned to the reference catalog.
/// Meridian flips are detected and corrected automatically. Failed alignments
/// keep weight × 0.5.
class FrameAligner {
public:
    struct Config {
        StarDetector::Config star_config;
        StarMatcher::Config  match_config;
        HomographyComputer::Config homography_config;
        ChannelRegistrationConfig channel_config;

        /// Register the colour channels to each other within each frame.
        ///
        /// On by default and with no user-facing threshold, deliberately:
        /// almost nobody knows they have lateral chromatic aberration, so an
        /// opt-in would not reach the people it helps. The cost is bounded by
        /// the near-identity skip -- a rig with no colour error pays nothing.
        /// The flag exists so tests can isolate the old behaviour.
        bool register_channels = true;

        /// Rescue a frame that cannot match the reference directly by
        /// matching it against an already-aligned neighbour and composing
        /// the two transforms.
        ///
        /// A single reference cannot bridge a long session: on the M27 2025
        /// corpus (7 hours, ~110 px of cumulative drift) the frames at both
        /// temporal ends get plenty of correspondences -- 55 at 36 frames out
        /// -- and the homography rejects every one, because drift changes
        /// which stars are in the top-K and the descriptors match the wrong
        /// ones. Neighbouring frames have barely drifted at all, so the chain
        /// walks outward to the ends of the session.
        ///
        /// A FALLBACK, never the default path: a frame that matches the
        /// reference directly is untouched, which is what keeps every corpus
        /// that already aligns fully bit-identical.
        bool chain_through_anchors = true;

        /// How many anchors to try before giving up, nearest in time first.
        int max_anchor_attempts = 4;
    };

    FrameAligner() = default;
    explicit FrameAligner(const Config& config);

    /// Align a frame to the reference. Returns the aligned image and alignment result.
    /// If this is the first frame, it becomes the reference (H = identity).
    ///
    /// The input image may be mono or colour. Star detection runs on the
    /// green channel for a colour frame (see default_reference_channel()).
    struct AlignedFrame {
        Image image;              // warped image, aligned to reference
        AlignmentResult alignment;
        StarCatalog stars;        // detected stars (pre-alignment coordinates)

        /// Per-channel transforms measured on this frame, before warping.
        /// Empty for a mono frame, when registration is off, or when nothing
        /// could be measured.
        ChannelTransforms channels;

        /// Which pixels of `image` actually received source data.
        ///
        /// Empty when the frame was cloned rather than warped -- an unwarped
        /// frame covers itself completely, so there is nothing to record and
        /// no allocation to pay for. CoverageMask::empty() therefore reads as
        /// "everything is covered", and the accumulator treats it that way.
        CoverageMask coverage;

        int frame_index;
    };

    /// `obs_time` is the frame's DATE-OBS in seconds (parse_fits_datetime);
    /// 0 means unknown. It orders the chain's anchors: nearest in TIME first.
    AlignedFrame align(const Image& frame, int frame_index, double obs_time = 0.0);

    /// The order in which chaining tries anchors, as (index, time) pairs:
    /// by |time difference| when both times are known, else by |index
    /// difference|. Exposed because it was wrong once -- the comment said
    /// "nearest in time" and the code sorted by index, which on an unsorted
    /// directory is not time: the first exposure of a session was processed
    /// 48th, its temporal neighbours were never tried, and it failed with 200
    /// stars and no inliers.
    static std::vector<int> chain_order(const std::vector<std::pair<int, double>>& anchors,
                                        int frame_index, double frame_time);

    /// Install `frame` as the alignment reference before any align() call.
    ///
    /// `frame_index` is the index align() will later see for this same frame;
    /// that call short-circuits to the identity rather than matching the frame
    /// against its own catalog.
    void set_reference(const Image& frame, int frame_index);

    /// How many frames were rescued by chaining. Diagnostic.
    int chained_count() const { return chained_count_; }

    /// Get the reference catalog (for inspection/debugging).
    const StarCatalog& reference_catalog() const { return ref_catalog_; }

    /// Has a reference frame been set?
    bool has_reference() const { return has_ref_; }

    /// Reset the aligner (clear reference).
    void reset();

private:
    /// Adopt `frame` as the reference. Shared by set_reference() and the
    /// first-frame fallback inside align().
    void adopt_reference(const Image& frame, const StarCatalog& stars,
                         int frame_index);

    /// A frame already aligned to the reference, usable as a stepping stone.
    ///
    /// Holds its catalog and its transform TO the reference, so a later frame
    /// that matches this one composes straight through:
    ///   H_ref<-frame = H_ref<-anchor * H_anchor<-frame
    /// A frame rescued through an anchor becomes an anchor itself, which is
    /// what lets the chain reach the ends of a session.
    struct Anchor {
        int              index = -1;
        double           time  = 0.0;      ///< DATE-OBS seconds, 0 = unknown
        StarCatalog      catalog;
        HomographyMatrix H_ref_from_anchor;
    };

    /// Try to reach the reference through an anchor. Returns true and fills
    /// `result` on success.
    bool try_chain(const StarCatalog& stars, int frame_index, double frame_time,
                   AlignmentResult& result) const;

    Config config_;
    StarCatalog ref_catalog_;
    bool has_ref_ = false;
    int ref_width_ = 0;
    int ref_height_ = 0;
    int ref_index_ = -1;
    std::vector<Anchor> anchors_;
    int chained_count_ = 0;
};

} // namespace nukex
