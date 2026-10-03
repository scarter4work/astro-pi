#include "catch_amalgamated.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>
#include <cmath>

using namespace nukex;

/// Create a synthetic star field with Gaussian stars.
static Image create_star_field(int w, int h,
                                const std::vector<std::tuple<float,float,float>>& stars,
                                float bg = 0.05f, float sigma = 3.0f) {
    Image img(w, h, 1);
    img.fill(bg);
    for (const auto& [sx, sy, amp] : stars) {
        int r = static_cast<int>(sigma * 4);
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int px = static_cast<int>(sx + 0.5f) + dx;
                int py = static_cast<int>(sy + 0.5f) + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                float fx = static_cast<float>(px) - sx;
                float fy = static_cast<float>(py) - sy;
                img.at(px, py, 0) += amp * std::exp(-(fx*fx + fy*fy) / (2*sigma*sigma));
            }
        }
    }
    return img;
}

/// The star list a meridian flip produces: every star rotated 180 degrees
/// about the frame centre, which is exactly what the mount does when it
/// swings the camera through the meridian.
static std::vector<std::tuple<float,float,float>> rotate_180(
    const std::vector<std::tuple<float,float,float>>& stars, int w, int h) {
    std::vector<std::tuple<float,float,float>> out;
    for (const auto& [x, y, a] : stars)
        out.emplace_back(static_cast<float>(w - 1) - x,
                         static_cast<float>(h - 1) - y, a);
    return out;
}

/// Brightest pixel within `r` of (x,y), as a multiple of the background.
static float peak_near(const Image& img, float x, float y, int r = 6) {
    float best = 0.0f;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            int px = static_cast<int>(x + 0.5f) + dx;
            int py = static_cast<int>(y + 0.5f) + dy;
            if (px < 0 || px >= img.width() || py < 0 || py >= img.height()) continue;
            best = std::max(best, img.at(px, py, 0));
        }
    return best;
}

TEST_CASE("FrameAligner: a meridian-flipped frame lands on the reference, "
          "not rotated away from it", "[aligner][meridian]") {
    // Measured on the user's 114-frame M63: 74 frames at one rotation, 40 at
    // the other. In the stack, EVERY bright star came with a companion hole
    // at its exact 180-degree reflection about the frame centre -- 64 such
    // regions, each ~130x the median noise, the stacked value 3% low. The raw
    // frames have nothing at the ghost positions: blank sky, indistinguishable
    // from a control patch. So the 40 flipped frames were being laid down
    // rotated 180 degrees from where they belong, and the stacker's own
    // robustness hid it by rejecting them as the minority -- leaving the holes
    // the user saw as "black circles where the stars should be".
    const int W = 240, H = 200;
    // Every star is kept clear of EVERY star's 180-degree reflection, so a
    // probe at a reflection can only ever see background. Without that the
    // test lies: a neighbour 7 px from a reflection reads as a ghost.
    std::vector<std::tuple<float,float,float>> stars = {
        {28, 32, 0.9f}, {95, 45, 0.75f}, {60, 78, 0.85f}, {33, 120, 0.7f},
        {104, 128, 0.8f}, {72, 160, 0.65f}, {20, 96, 0.6f}, {110, 96, 0.72f},
        {48, 55, 0.55f}, {88, 175, 0.68f}
    };

    Image reference = create_star_field(W, H, stars);
    Image flipped   = create_star_field(W, H, rotate_180(stars, W, H));

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 3.0f;
    config.match_config.max_distance = 10.0f;
    FrameAligner aligner(config);
    aligner.align(reference, 0);
    auto result = aligner.align(flipped, 1);

    REQUIRE(result.alignment.alignment_failed == false);

    // A flip is a real 180-degree rotation between frame and reference, and
    // the homography that maps one onto the other necessarily contains it.
    // The frame must come back UNDONE: stars where the reference has stars.
    const float bg = 0.05f;
    for (const auto& [sx, sy, amp] : stars) {
        const float here  = peak_near(result.image, sx, sy);
        const float ghost = peak_near(result.image, static_cast<float>(W - 1) - sx,
                                                    static_cast<float>(H - 1) - sy);
        INFO("star (" << sx << "," << sy << "): peak here " << here
             << ", peak at its 180-degree reflection " << ghost
             << ", background " << bg);
        REQUIRE(here > bg + 0.3f * amp);
        REQUIRE(ghost < bg + 0.3f * amp);
    }
}

TEST_CASE("FrameAligner: first frame becomes reference", "[aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f},
        {120, 120, 0.55f}, {30, 100, 0.5f}, {170, 100, 0.45f}
    };

    Image frame = create_star_field(200, 200, stars);
    FrameAligner aligner;
    auto result = aligner.align(frame, 0);

    REQUIRE(aligner.has_reference() == true);
    REQUIRE(result.alignment.H.is_identity() == true);
    REQUIRE(result.alignment.alignment_failed == false);
    REQUIRE(result.image.width() == 200);
}

TEST_CASE("FrameAligner: identical frames align to identity", "[aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f},
        {120, 120, 0.55f}, {30, 100, 0.5f}, {170, 100, 0.45f}
    };

    Image frame = create_star_field(200, 200, stars);

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 3.0f;
    config.match_config.max_distance = 10.0f;
    FrameAligner aligner(config);

    auto ref_result = aligner.align(frame, 0);
    auto result = aligner.align(frame, 1);

    REQUIRE(result.alignment.match.success == true);
    REQUIRE(result.alignment.H.is_identity(1.0f) == true);
    REQUIRE(result.alignment.match.rms_error < 1.0f);
}

TEST_CASE("FrameAligner: real FITS frames", "[aligner][integration]") {
    std::string path1 = "/home/scarter4work/projects/processing/M16/"
                        "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";
    std::string path2 = "/home/scarter4work/projects/processing/M16/"
                        "Light_M16_300.0s_Bin1_HaO3_20230901-232001_0002.fit";

    if (!std::filesystem::exists(path1) || !std::filesystem::exists(path2)) {
        SKIP("Test FITS files not available");
    }

    auto r1 = FITSReader::read(path1);
    auto r2 = FITSReader::read(path2);
    REQUIRE(r1.success);
    REQUIRE(r2.success);

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 5.0f;
    config.star_config.max_stars = 200;
    config.match_config.max_distance = 20.0f;

    FrameAligner aligner(config);

    auto ref = aligner.align(r1.image, 0);
    REQUIRE(ref.alignment.H.is_identity());
    INFO("Reference stars: " << ref.stars.size());
    REQUIRE(ref.stars.size() >= 10);

    auto aligned = aligner.align(r2.image, 1);
    INFO("Inliers: " << aligned.alignment.match.n_inliers);
    INFO("RMS error: " << aligned.alignment.match.rms_error);
    REQUIRE(aligned.alignment.match.success == true);
    REQUIRE(aligned.alignment.alignment_failed == false);
    REQUIRE(aligned.alignment.match.n_inliers >= 8);
    REQUIRE(aligned.alignment.match.rms_error < 5.0f);
}

TEST_CASE("FrameAligner: reset clears reference", "[aligner]") {
    FrameAligner aligner;
    Image frame(100, 100, 1);
    frame.fill(0.5f);

    aligner.align(frame, 0);
    REQUIRE(aligner.has_reference() == true);

    aligner.reset();
    REQUIRE(aligner.has_reference() == false);
}

// ── Explicit reference selection ─────────────────────────────────────

TEST_CASE("FrameAligner: an explicitly set reference wins over the first frame aligned",
          "[aligner]") {
    // Directory order used to decide the reference. The engine now measures
    // every frame first and hands the aligner the one it picked, so the frame
    // that arrives first at align() must NOT capture the reference.
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f},
        {120, 120, 0.55f}, {30, 100, 0.5f}, {170, 100, 0.45f}
    };
    Image chosen = create_star_field(200, 200, stars);

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 3.0f;
    config.match_config.max_distance = 10.0f;
    FrameAligner aligner(config);

    aligner.set_reference(chosen, /*frame_index*/ 7);
    REQUIRE(aligner.has_reference() == true);

    // Frame 0 arrives first but must be matched against the reference, not
    // installed as one.
    auto first_seen = aligner.align(chosen, 0);
    REQUIRE(first_seen.alignment.alignment_failed == false);

    // The chosen frame still resolves to the identity when it comes round.
    auto ref_frame = aligner.align(chosen, 7);
    REQUIRE(ref_frame.alignment.H.is_identity() == true);
    REQUIRE(ref_frame.alignment.alignment_failed == false);
}

TEST_CASE("FrameAligner: reset clears an explicitly set reference", "[aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f}
    };
    Image frame = create_star_field(200, 200, stars);
    FrameAligner aligner;
    aligner.set_reference(frame, 3);
    aligner.reset();
    REQUIRE(aligner.has_reference() == false);

    // With no reference, the next frame to arrive becomes it again.
    auto r = aligner.align(frame, 5);
    REQUIRE(aligner.has_reference() == true);
    REQUIRE(r.alignment.H.is_identity() == true);
}

// --- per-channel registration through the aligner ------------------------
//
// nukex/alignment/channel_registration.hpp is reachable transitively through
// frame_aligner.hpp, already included above.

namespace {

// A 3-channel frame with a star grid; red displaced by a pure translation.
nukex::Image make_colour_frame(int w, int h, double red_dx, double red_dy,
                               double jitter_x = 0.0, double jitter_y = 0.0) {
    nukex::Image img(w, h, 3);
    img.fill(0.002f);
    auto blob = [&](int ch, double cx, double cy) {
        for (int dy = -6; dy <= 6; dy++)
            for (int dx = -6; dx <= 6; dx++) {
                int px = int(std::lround(cx)) + dx;
                int py = int(std::lround(cy)) + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                double ex = px - cx, ey = py - cy;
                img.at(px, py, ch) += float(0.5 * std::exp(-(ex*ex + ey*ey) / 5.12));
            }
    };
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++) {
            // Break the grid's exact periodicity. Triangle-similarity matching is
            // degenerate on a lattice -- many triangles are congruent to a triangle
            // in another cell, so the matcher pairs stars across cells and the
            // homography fails on too few correct correspondences. Real star fields
            // are never periodic. Deterministic, so the test does not depend on a
            // seed.
            const double ox = 9.0 * std::sin(2.7 * (i + 1) + 0.9 * (j + 1));
            const double oy = 9.0 * std::cos(1.9 * (i + 1) + 1.3 * (j + 1));
            double x = 50.0 + 0.37 + i * (w - 100.0) / 4.0 + ox + jitter_x;
            double y = 50.0 + 0.61 + j * (h - 100.0) / 4.0 + oy + jitter_y;
            blob(1, x, y);
            blob(2, x, y);
            blob(0, x + red_dx, y + red_dy);
        }
    return img;
}

// A 3-channel frame with only 3 stars -- too few to reach the matcher's
// 8-match minimum against a 25-star reference catalog, so alignment fails
// for a reason unrelated to how well the stars themselves would line up.
// Used to test that a failed-alignment frame still gets its channels put
// right.
nukex::Image make_sparse_colour_frame(int w, int h, double red_dx, double red_dy) {
    nukex::Image img(w, h, 3);
    img.fill(0.002f);
    auto blob = [&](int ch, double cx, double cy) {
        for (int dy = -6; dy <= 6; dy++)
            for (int dx = -6; dx <= 6; dx++) {
                int px = int(std::lround(cx)) + dx;
                int py = int(std::lround(cy)) + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                double ex = px - cx, ey = py - cy;
                img.at(px, py, ch) += float(0.5 * std::exp(-(ex*ex + ey*ey) / 5.12));
            }
    };
    static const double positions[3][2] = {
        {60.0, 70.0}, {250.0, 120.0}, {150.0, 300.0}};
    for (const auto& p : positions) {
        double x = p[0], y = p[1];
        blob(1, x, y);
        blob(2, x, y);
        blob(0, x + red_dx, y + red_dy);
    }
    return img;
}

double channel_offset_x(const nukex::Image& im, int ch_a, int ch_b,
                        int x0, int x1, int y0, int y1) {
    auto com = [&](int ch) {
        double w = 0, wx = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                double v = im.at(x, y, ch) - 0.002;
                if (v <= 0) continue;
                w += v; wx += v * x;
            }
        return wx / w;
    };
    return com(ch_a) - com(ch_b);
}

} // namespace

TEST_CASE("FrameAligner registers channels on the reference frame itself",
          "[frame_aligner]") {
    // The reference frame is not warped for alignment -- H is identity by
    // construction. Its channels still have to be put right, or every other
    // frame inherits its colour error through the reference.
    nukex::Image ref = make_colour_frame(400, 400, 1.2, 0.0);

    nukex::FrameAligner aligner;
    aligner.set_reference(ref, 0);
    auto out = aligner.align(ref, 0);

    REQUIRE_FALSE(out.channels.empty());
    REQUIRE(out.channels.reference_channel == 1);

    CHECK(std::abs(channel_offset_x(ref, 0, 1, 30, 90, 30, 90)) > 1.0);
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) < 0.06);
}

TEST_CASE("FrameAligner registers channels on a warped frame",
          "[frame_aligner]") {
    nukex::Image ref   = make_colour_frame(400, 400, 1.2, 0.0);
    nukex::Image moved = make_colour_frame(400, 400, 1.2, 0.0, 3.0, 2.0);

    nukex::FrameAligner aligner;
    aligner.set_reference(ref, 0);
    (void)aligner.align(ref, 0);
    auto out = aligner.align(moved, 1);

    REQUIRE_FALSE(out.alignment.alignment_failed);
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) < 0.06);
}

TEST_CASE("a mono frame produces no channel transforms and is untouched",
          "[frame_aligner]") {
    nukex::Image mono(300, 300, 1);
    mono.fill(0.002f);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            double cx = 50.0 + 0.3 + i * 66.0, cy = 50.0 + 0.7 + j * 66.0;
            for (int dy = -6; dy <= 6; dy++)
                for (int dx = -6; dx <= 6; dx++) {
                    int px = int(std::lround(cx)) + dx, py = int(std::lround(cy)) + dy;
                    if (px < 0 || px >= 300 || py < 0 || py >= 300) continue;
                    double ex = px - cx, ey = py - cy;
                    mono.at(px, py, 0) += float(0.5 * std::exp(-(ex*ex+ey*ey)/5.12));
                }
        }

    nukex::FrameAligner aligner;
    aligner.set_reference(mono, 0);
    auto out = aligner.align(mono, 0);

    REQUIRE(out.channels.empty());
    for (int y = 0; y < 300; y++)
        for (int x = 0; x < 300; x++)
            REQUIRE(out.image.at(x, y, 0) == mono.at(x, y, 0));
}

TEST_CASE("a frame whose channels already agree is not resampled for it",
          "[frame_aligner]") {
    // All three channels drawn at the same positions. The near-identity skip
    // must take the plain path, leaving the reference frame bit-identical.
    nukex::Image ref = make_colour_frame(400, 400, 0.0, 0.0);

    nukex::FrameAligner aligner;
    aligner.set_reference(ref, 0);
    auto out = aligner.align(ref, 0);

    for (int c = 0; c < 3; c++)
        for (int y = 0; y < 400; y++)
            for (int x = 0; x < 400; x++)
                REQUIRE(out.image.at(x, y, c) == ref.at(x, y, c));
}

TEST_CASE("channel registration can be switched off",
          "[frame_aligner]") {
    nukex::Image ref = make_colour_frame(400, 400, 1.2, 0.0);

    nukex::FrameAligner::Config cfg;
    cfg.register_channels = false;
    nukex::FrameAligner aligner(cfg);
    aligner.set_reference(ref, 0);
    auto out = aligner.align(ref, 0);

    REQUIRE(out.channels.empty());
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) > 1.0);
}

TEST_CASE("a frame whose alignment fails still gets its channels registered",
          "[frame_aligner]") {
    // Alignment fails here because the frame has too few stars to reach the
    // matcher's 8-match minimum, not because the stars are badly placed.
    // This is the one image-producing path whose output geometry is the
    // frame's own dimensions rather than the reference's, and one of only
    // two paths that changed from cloning to warping -- it needs its own
    // coverage rather than an inference from the reference-frame path.
    nukex::Image ref    = make_colour_frame(400, 400, 1.2, 0.0);
    nukex::Image sparse = make_sparse_colour_frame(400, 400, 1.2, 0.0);

    nukex::FrameAligner aligner;
    aligner.set_reference(ref, 0);
    (void)aligner.align(ref, 0);
    auto out = aligner.align(sparse, 1);

    REQUIRE(out.alignment.alignment_failed);
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) < 0.06);
}

// ---------------------------------------------------------------------------
// Anchor chaining for session drift
//
// M27 2025 aligns 51 of 72 frames. The 21 failures are the two temporal ends
// of a seven-hour session with ~110 px of cumulative tracking drift. The far
// frames are not short of correspondences -- at 36 frames out they get 55
// matches and the homography rejects every one -- because drift changes WHICH
// stars are in the top-K, so triangle descriptors match the wrong stars.
//
// The synthetic version of that is a window panning across a larger sky:
// neighbouring frames share almost all their stars, distant ones share few.
// ---------------------------------------------------------------------------

namespace {

// A fixed, irregular sky. Irregular on purpose: triangle-similarity matching
// is degenerate on a regular grid.
std::vector<std::pair<float,float>> drift_sky() {
    std::vector<std::pair<float,float>> sky;
    std::uint32_t st = 12345u;
    for (int i = 0; i < 220; i++) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        const float x = static_cast<float>((st >> 8) % 5200) / 10.0f;   // 0..520
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        const float y = static_cast<float>((st >> 8) % 1900) / 10.0f;   // 0..190
        sky.emplace_back(x, y);
    }
    return sky;
}

// One 200x200 exposure of that sky, its window offset by (ox, 0).
Image drift_frame(const std::vector<std::pair<float,float>>& sky, float ox) {
    std::vector<std::tuple<float,float,float>> stars;
    for (size_t i = 0; i < sky.size(); i++) {
        const float x = sky[i].first - ox, y = sky[i].second;
        if (x < 6 || x > 194 || y < 6 || y > 194) continue;
        stars.emplace_back(x, y, 0.5f + 0.004f * (i % 100));
    }
    return create_star_field(200, 200, stars);
}

} // namespace

TEST_CASE("FrameAligner: a drifted frame that cannot match the reference "
          "directly is rescued by chaining through an anchor",
          "[aligner][chaining]") {
    const auto sky = drift_sky();

    // Reference at offset 0; the session pans 22 px per frame.
    FrameAligner chained;                       // chaining on (default)

    FrameAligner::Config no_chain_cfg;
    no_chain_cfg.chain_through_anchors = false;
    FrameAligner direct(no_chain_cfg);

    const int n = 9;
    int ok_direct = 0, ok_chained = 0;
    for (int i = 0; i < n; i++) {
        Image f = drift_frame(sky, 22.0f * i);
        if (!direct.align(f, i).alignment.alignment_failed)  ok_direct++;
        if (!chained.align(f, i).alignment.alignment_failed) ok_chained++;
    }

    INFO("direct=" << ok_direct << " chained=" << ok_chained << " of " << n);
    // Chaining must rescue frames the direct path loses, and must never lose
    // one the direct path found.
    REQUIRE(ok_chained >= ok_direct);
    REQUIRE(ok_chained > ok_direct);
    // The direct path must genuinely lose frames here, or the case is not
    // exercising what it claims to.
    REQUIRE(ok_direct < n);
    // And chaining should recover the whole pan, which is the point: each
    // step is a small drift even though the ends share almost no stars.
    REQUIRE(ok_chained == n);
}

TEST_CASE("FrameAligner: chaining does not disturb a session that already "
          "aligns fully", "[aligner][chaining]") {
    // The reason chaining is a FALLBACK and not the default path: the three
    // regression corpora align every frame today, so they must never enter
    // it and their goldens cannot move.
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 47, 0.7f}, {103, 100, 0.9f},
        {47, 152, 0.6f}, {150, 150, 0.75f}, {83, 78, 0.65f},
        {122, 118, 0.55f}, {31, 97, 0.5f}, {168, 104, 0.45f}
    };
    Image ref = create_star_field(200, 200, stars);

    FrameAligner aligner;
    auto r0 = aligner.align(ref, 0);
    REQUIRE_FALSE(r0.alignment.alignment_failed);
    REQUIRE_FALSE(r0.alignment.chained);

    auto r1 = aligner.align(ref, 1);   // identical frame: matches directly
    REQUIRE_FALSE(r1.alignment.alignment_failed);
    REQUIRE_FALSE(r1.alignment.chained);
}

// ── Chaining walks by TIME, not by processing order ──────────────────────

TEST_CASE("FrameAligner::chain_order: anchors nearest in time come first, index "
          "only when a time is unknown", "[aligner][chain]") {
    // Processing order is directory order. The frame at index 47 is the
    // first exposure of the session (t = 0); anchor 46 was taken 28 minutes
    // later, anchor 3 two minutes later. Index distance would try 46 first.
    // Times are epoch seconds; 0 is the "unknown" sentinel, so the session
    // starts at T0.
    const double T0 = 1756677586.0;
    std::vector<std::pair<int, double>> anchors = {
        {46, T0 + 1680.0}, {3, T0 + 120.0}, {10, T0 + 7200.0}, {20, T0 + 60.0},
    };
    auto order = FrameAligner::chain_order(anchors, 47, T0);
    REQUIRE(order.size() == 4);
    REQUIRE(anchors[order[0]].first == 20);   // 60 s away
    REQUIRE(anchors[order[1]].first == 3);    // 120 s
    REQUIRE(anchors[order[2]].first == 46);   // 1680 s
    REQUIRE(anchors[order[3]].first == 10);   // 7200 s

    // No times at all: fall back to index distance.
    auto by_index = FrameAligner::chain_order({{46, 0.0}, {3, 0.0}, {10, 0.0}, {20, 0.0}}, 47, 0.0);
    REQUIRE(by_index[0] == 0);   // index 46, one away

    // A timed anchor outranks an untimed one however close in index.
    auto mixed = FrameAligner::chain_order({{46, 0.0}, {3, T0 + 120.0}}, 47, T0);
    REQUIRE(mixed[0] == 1);
}
