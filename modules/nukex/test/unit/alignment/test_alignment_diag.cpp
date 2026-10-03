// Diagnostic test for the alignment pipeline — instrument each stage on real data.
//
// This test exists to gather evidence about why stacking reports ~61/65 frames
// as "failed alignment" on real multi-hour imaging sessions. It is not a PASS/FAIL
// test — it writes diagnostic numbers to stdout and always passes. Run with:
//
//   ctest -V -R test_alignment_diag
//
// It skips (not fails) if the NGC7635 data directory is unavailable.

#include "catch_amalgamated.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numeric>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

// Collect FITS paths from a directory, sorted alphabetically (≈ chronological
// for our file naming convention).
std::vector<std::string> collect_fits(const std::string& dir) {
    std::vector<std::string> out;
    if (!fs::is_directory(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        if (ext == ".fit" || ext == ".fits" || ext == ".FIT" || ext == ".FITS")
            out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct SweepRow {
    float desc_tol;
    float scale_tol;
    int   n_matches;
    int   n_inliers;
    float rms_error;
    bool  alignment_failed;
};

SweepRow run_one(const StarCatalog& src, const StarCatalog& ref,
                 float desc_tol, float scale_tol) {
    StarMatcher::Config cfg;
    cfg.descriptor_tolerance = desc_tol;
    cfg.scale_tolerance_log  = scale_tol;
    auto matches = StarMatcher::match(src, ref, cfg);
    HomographyComputer::Config hc;
    auto r = HomographyComputer::compute(src, ref, matches, hc);
    return { desc_tol, scale_tol, static_cast<int>(matches.size()),
             r.match.n_inliers, r.match.rms_error, r.alignment_failed };
}

void print_header() {
    std::cout << std::setw(10) << "desc_tol"
              << std::setw(10) << "scale_tol"
              << std::setw(12) << "n_matches"
              << std::setw(12) << "n_inliers"
              << std::setw(14) << "rms_error"
              << std::setw(10) << "failed?" << "\n";
}

void print_row(const SweepRow& r) {
    std::cout << std::setw(10) << std::fixed << std::setprecision(4) << r.desc_tol
              << std::setw(10) << std::setprecision(4) << r.scale_tol
              << std::setw(12) << r.n_matches
              << std::setw(12) << r.n_inliers
              << std::setw(14) << std::setprecision(3) << r.rms_error
              << std::setw(10) << (r.alignment_failed ? "FAIL" : "ok") << "\n";
}

} // namespace

TEST_CASE("alignment diagnostic: NGC7635 drift sweep", "[alignment][diag][.diag]") {
    const std::string dir = "/mnt/qnap/astro_data/NGC7635/L/Lights";
    auto paths = collect_fits(dir);
    if (paths.size() < 3) {
        SKIP("NGC7635 dataset unavailable at " << dir);
    }

    // Reference = first; probe 1 (adjacent), middle, last — isolates whether
    // matching fails from drift or from cross-session star-selection instability.
    std::vector<int> probes = { 0, 1, static_cast<int>(paths.size()) / 2,
                                static_cast<int>(paths.size()) - 1 };

    StarDetector::Config sd;
    sd.max_stars = 200;

    std::cout << "\n=== Alignment diagnostic on " << paths.size() << " frames ===\n";
    std::cout << "Reference: " << paths[probes[0]] << "\n";

    // Read + detect reference
    auto ref_read = FITSReader::read(paths[probes[0]]);
    REQUIRE(ref_read.success);
    auto ref_stars = StarDetector::detect(ref_read.image, sd);
    std::cout << "Reference detected " << ref_stars.size() << " stars\n\n";

    for (size_t k = 1; k < probes.size(); ++k) {
        int idx = probes[k];
        std::cout << "--- Probe frame " << idx << ": " << paths[idx] << " ---\n";
        auto r = FITSReader::read(paths[idx]);
        REQUIRE(r.success);
        auto src_stars = StarDetector::detect(r.image, sd);
        std::cout << "Detected " << src_stars.size() << " stars in source frame\n";

        // Top-K flux stability: how many top-30 brightest stars in the source
        // have a counterpart in reference's top-30 brightest within 300 px
        // (accommodating drift)?
        {
            auto top_src = [&] {
                std::vector<int> idx(src_stars.size());
                std::iota(idx.begin(), idx.end(), 0);
                std::partial_sort(idx.begin(), idx.begin() + std::min<size_t>(30, src_stars.size()), idx.end(),
                    [&](int a, int b){ return src_stars.stars[a].flux > src_stars.stars[b].flux; });
                idx.resize(std::min<size_t>(30, src_stars.size()));
                return idx;
            }();
            auto top_ref = [&] {
                std::vector<int> idx(ref_stars.size());
                std::iota(idx.begin(), idx.end(), 0);
                std::partial_sort(idx.begin(), idx.begin() + std::min<size_t>(30, ref_stars.size()), idx.end(),
                    [&](int a, int b){ return ref_stars.stars[a].flux > ref_stars.stars[b].flux; });
                idx.resize(std::min<size_t>(30, ref_stars.size()));
                return idx;
            }();
            int overlap = 0;
            for (int si : top_src) {
                float sx = src_stars.stars[si].x, sy = src_stars.stars[si].y;
                for (int ri : top_ref) {
                    float rx = ref_stars.stars[ri].x, ry = ref_stars.stars[ri].y;
                    float dd = (sx-rx)*(sx-rx) + (sy-ry)*(sy-ry);
                    if (dd < 300.0f*300.0f) { overlap++; break; }
                }
            }
            std::cout << "  top-30 spatial overlap (within 300 px): " << overlap << "/30\n";
        }

        print_header();
        for (float dt : { 0.003f, 0.005f, 0.01f, 0.02f, 0.05f }) {
            for (float st : { 0.0086f, 0.02f, 0.05f, 0.1f }) {
                print_row(run_one(src_stars, ref_stars, dt, st));
            }
        }
        std::cout << "\n";
    }

    SUCCEED();
}

// Why do 21 of 72 frames still fail on M27 2025 after reference selection?
//
// Reference selection took this corpus from 1 of 72 aligned to 51 of 72 by
// picking a 200-star frame instead of the 32-star blue frame that happened to
// sort first. What remains is not a filter problem and not a reference-choice
// problem: the corpus is filter-blocked and chronological (L 0-23, R 24-35,
// G 36-47, B 48-71) and the failures are the two temporal ENDS of the
// seven-hour session -- chronological frames 0-19 and 66-71 -- while
// everything within about 90 minutes of the reference aligns. Measured from
// the run log, failures against distance from the reference (index 36):
//
//     |frames from ref|   ok  FAILED
//              0 - 15      31       0
//             16 - 31      20      12
//             32 - 39       0       9
//
// Header RA/DEC puts the mount drift at ~110 px over the session with
// PIERSIDE constant throughout, so there is no meridian flip to correct. The
// reference is already the temporal midpoint, so no choice of single
// reference reaches both ends. Bridging this needs chained or multi-reference
// alignment, which the pipeline does not have.
//
// What this case shows when run: the matcher keeps finding correspondences at
// every distance, but their QUALITY decays -- 83 inliers of 87 matches four
// frames out, 12 of 62 at twenty-six, and 0 of 55 at thirty-six. The far
// frames are not short of matches; the matches are increasingly wrong, which
// is what drift does to top-K triangle descriptors. Separately, the last few
// B frames are genuinely star-poor (21 and 32 stars against 200 elsewhere)
// and fail for that reason instead.
TEST_CASE("alignment diagnostic: M27 2025 drift from the chosen reference",
          "[alignment][diag][.diag]") {
    const std::string dir = "/mnt/qnap/astro_data/9_1_2025/M27";
    auto paths = collect_fits(dir);
    if (paths.size() < 72) {
        SKIP("M27 2025 dataset unavailable at " << dir);
    }

    StarDetector::Config sd;
    sd.max_stars = 200;

    // collect_fits sorts by filename, which for this corpus is chronological.
    // Index 36 is the frame select_reference_frame() picks (logged as
    // "frame 10/72", a G frame, 200 stars, FWHM 2.87 px).
    const int ref_idx = 36;
    // Outward from the reference in both directions, ending at both extremes.
    const std::vector<int> probes = { 40, 30, 20, 60, 10, 68, 0, 71 };

    std::cout << "\n=== M27 2025: " << paths.size() << " frames ===\n";
    std::cout << "Reference [" << ref_idx << "]: "
              << fs::path(paths[ref_idx]).filename().string() << "\n";

    auto ref_read = FITSReader::read(paths[ref_idx]);
    REQUIRE(ref_read.success);
    auto ref_stars = StarDetector::detect(ref_read.image, sd);
    std::cout << "Reference detected " << ref_stars.size() << " stars\n\n";
    std::cout << std::setw(8) << "probe" << std::setw(10) << "distance"
              << std::setw(10) << "stars" << std::setw(14) << "top30 overlap"
              << std::setw(12) << "n_matches" << std::setw(12) << "n_inliers"
              << std::setw(10) << "result" << "\n";

    for (int idx : probes) {
        auto r = FITSReader::read(paths[idx]);
        REQUIRE(r.success);
        auto src = StarDetector::detect(r.image, sd);

        int overlap = 0;
        int n = std::min<size_t>(30, src.stars.size());
        for (int i = 0; i < n; i++) {
            float sx = src.stars[i].x, sy = src.stars[i].y;
            for (size_t j = 0; j < std::min<size_t>(30, ref_stars.stars.size()); j++) {
                float rx = ref_stars.stars[j].x, ry = ref_stars.stars[j].y;
                if ((sx-rx)*(sx-rx) + (sy-ry)*(sy-ry) < 300.0f*300.0f) { overlap++; break; }
            }
        }

        SweepRow row = run_one(src, ref_stars, 0.003f, 0.02f);
        std::cout << std::setw(8) << idx
                  << std::setw(10) << std::abs(idx - ref_idx)
                  << std::setw(10) << src.size()
                  << std::setw(11) << overlap << "/30"
                  << std::setw(12) << row.n_matches
                  << std::setw(12) << row.n_inliers
                  << std::setw(10) << (row.alignment_failed ? "FAIL" : "ok") << "\n";
    }
    std::cout << "\n";

    SUCCEED();
}
