#include "nukex/alignment/reference_selector.hpp"

namespace nukex {

int select_reference_frame(const std::vector<FrameQuality>& frames) {
    if (frames.empty()) {
        return -1;
    }

    int best_count = -1;
    int incumbent  = -1;   // the first usable frame: what the aligner would take
    for (std::size_t i = 0; i < frames.size(); i++) {
        if (!frames[i].usable) continue;
        if (incumbent < 0) incumbent = static_cast<int>(i);
        if (frames[i].star_count > best_count) best_count = frames[i].star_count;
    }
    if (incumbent < 0) {
        return 0;   // nothing usable -- keep the historical first-frame choice
    }

    const float threshold =
        static_cast<float>(best_count) * kReferenceCandidateBand;

    // Minimal intervention: a viable incumbent stays. Among viable candidates
    // the reference is arbitrary, and moving it costs alignments (see header).
    if (static_cast<float>(frames[incumbent].star_count) >= threshold) {
        return incumbent;
    }

    int   chosen      = -1;
    float chosen_fwhm = 0.0f;
    for (std::size_t i = 0; i < frames.size(); i++) {
        const auto& f = frames[i];
        if (!f.usable || static_cast<float>(f.star_count) < threshold) {
            continue;
        }
        // An unmeasurable FWHM must not read as infinitely sharp, so it sorts
        // behind every measured one instead of ahead of them.
        const bool measured        = f.median_fwhm > 0.0f;
        const bool chosen_measured = chosen >= 0 && chosen_fwhm > 0.0f;

        bool better;
        if (chosen < 0) {
            better = true;
        } else if (measured != chosen_measured) {
            better = measured;
        } else if (!measured) {
            better = false;   // both unmeasurable -- keep the lower index
        } else {
            better = f.median_fwhm < chosen_fwhm;
        }

        if (better) {
            chosen      = static_cast<int>(i);
            chosen_fwhm = f.median_fwhm;
        }
    }

    return chosen >= 0 ? chosen : incumbent;
}

} // namespace nukex
