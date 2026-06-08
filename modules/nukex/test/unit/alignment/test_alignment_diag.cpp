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
