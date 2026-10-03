#include "catch_amalgamated.hpp"
#include "nukex/alignment/channel_registration.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace nukex;

namespace {

// A Gaussian star drawn at a sub-pixel position. Amplitude and sigma are
// fixed; what the tests vary is where it lands.
void draw_star(Image& img, int ch, double x, double y,
               double amplitude = 0.5, double sigma = 1.6) {
    const int r = 6;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int px = ix + dx, py = iy + dy;
            if (px < 0 || px >= img.width() || py < 0 || py >= img.height())
                continue;
            double ex = px - x, ey = py - y;
            img.at(px, py, ch) += static_cast<float>(
                amplitude * std::exp(-(ex * ex + ey * ey) / (2.0 * sigma * sigma)));
        }
    }
}

// A grid of star positions well inside the frame, avoiding the border so the
// centroid box always fits.
std::vector<std::pair<double, double>> star_grid(int w, int h, int n_side) {
    std::vector<std::pair<double, double>> out;
    for (int j = 0; j < n_side; j++)
        for (int i = 0; i < n_side; i++) {
            // The offsets keep centroids off exact integers, which is where a
            // broken centroid estimator would accidentally look correct.
            out.emplace_back(20.0 + 0.37 + i * (w - 40.0) / (n_side - 1),
                             20.0 + 0.61 + j * (h - 40.0) / (n_side - 1));
        }
    return out;
}

struct Synth {
    Image       image;
    StarCatalog catalog;
};

// Builds a 3-channel frame. Green carries stars at `pos`; red carries the same
// stars displaced by (s, tx, ty) about the image centre; blue matches green.
Synth make_frame(int w, int h, double s, double tx, double ty) {
    Synth out;
    out.image = Image(w, h, 3);
    out.image.fill(0.002f);

    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

    for (auto [x, y] : star_grid(w, h, 5)) {
        draw_star(out.image, 1, x, y);                 // green: reference
        draw_star(out.image, 2, x, y);                 // blue: agrees with green
        draw_star(out.image, 0,                        // red: displaced
                  s * (x - cx) + tx + cx,
                  s * (y - cy) + ty + cy);

        Star st;
        st.x = static_cast<float>(x);
        st.y = static_cast<float>(y);
        st.flux = 1.0f;
        out.catalog.stars.push_back(st);
    }
    return out;
}

} // namespace

TEST_CASE("measure_channel_transforms recovers a known scale and shift",
          "[channel_registration]") {
    // +500 ppm and a third of a pixel. At the corner of a 800x800 frame the
    // scale term alone is 500e-6 * 565 = 0.28 px, so both terms matter.
    const double s = 1.0005, tx = 0.33, ty = -0.21;
    Synth f = make_frame(800, 800, s, tx, ty);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    REQUIRE(ct.per_channel.size() == 3);
    REQUIRE(ct.reference_channel == 1);

    const ChannelTransform& red = ct.per_channel[0];
    REQUIRE(red.fit == ChannelTransform::Fit::Affine);
    REQUIRE(red.n_stars >= 20);

    // The acceptance the spec asks for is positional, so assert positionally:
    // the modelled position must land within 0.02 px of the truth at the
    // most demanding place, which is the field corner.
    const double R = std::hypot(399.5, 399.5);
    CHECK(std::abs(red.s - s) * R < 0.02);
    CHECK(std::abs(red.tx - tx) < 0.02);
    CHECK(std::abs(red.ty - ty) < 0.02);
    CHECK(red.residual < 0.02);
}

TEST_CASE("the reference channel is exactly identity",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0005, 0.33, -0.21);
    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    const ChannelTransform& green = ct.per_channel[1];
    REQUIRE(green.fit == ChannelTransform::Fit::Identity);
    REQUIRE(green.is_identity());
    REQUIRE(green.s == 1.0);
    REQUIRE(green.tx == 0.0);
    REQUIRE(green.ty == 0.0);
}

TEST_CASE("a channel that already agrees measures as near-identity",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0005, 0.33, -0.21);
    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    // Blue was drawn at the same positions as green. Its fit is not forced to
    // identity -- it is measured -- so it must come out small, not zero.
    const ChannelTransform& blue = ct.per_channel[2];
    const double R = std::hypot(399.5, 399.5);
    CHECK(std::abs(blue.s - 1.0) * R < 0.02);
    CHECK(std::abs(blue.tx) < 0.02);
    CHECK(std::abs(blue.ty) < 0.02);
}

TEST_CASE("a single-channel image registers nothing",
          "[channel_registration]") {
    Image mono(200, 200, 1);
    mono.fill(0.002f);
    StarCatalog cat;
    for (auto [x, y] : star_grid(200, 200, 4)) {
        draw_star(mono, 0, x, y);
        Star st; st.x = float(x); st.y = float(y); cat.stars.push_back(st);
    }

    ChannelTransforms ct = measure_channel_transforms(mono, cat, 0);
    REQUIRE(ct.empty());
}

TEST_CASE("negligible() is true only when every channel is near identity "
          "at the given radius", "[channel_registration]") {
    ChannelTransforms ct;
    ct.cx = 400; ct.cy = 400; ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[1].fit = ChannelTransform::Fit::Identity;

    // 1e-6 of scale over 565 px is 0.00056 px -- below any threshold worth
    // resampling for.
    ct.per_channel[0] = ChannelTransform{1.000001, 0.0, 0.0, 50, 0.01,
                                         ChannelTransform::Fit::Affine};
    CHECK(ct.negligible(565.0, 0.01));

    // 0.05 px of pure translation is not negligible at any radius.
    ct.per_channel[0].s  = 1.0;
    ct.per_channel[0].tx = 0.05;
    CHECK_FALSE(ct.negligible(565.0, 0.01));

    // 100 ppm is 0.056 px at the corner: negligible near the centre, not at
    // the edge. The radius argument is what makes that distinction.
    ct.per_channel[0].tx = 0.0;
    ct.per_channel[0].s  = 1.0001;
    CHECK_FALSE(ct.negligible(565.0, 0.01));
    CHECK(ct.negligible(50.0, 0.01));
}

// --- the fallback ladder -------------------------------------------------

namespace {

// A frame with exactly `n` usable stars in every channel, red displaced.
Synth make_sparse_frame(int w, int h, int n, double s, double tx, double ty) {
    Synth out;
    out.image = Image(w, h, 3);
    out.image.fill(0.002f);
    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

    for (int i = 0; i < n; i++) {
        // Spread along a diagonal so the positions are never degenerate.
        const double x = 30.0 + 0.37 + i * (w - 60.0) / std::max(1, n - 1);
        const double y = 30.0 + 0.61 + i * (h - 60.0) / std::max(1, n - 1);
        draw_star(out.image, 1, x, y);
        draw_star(out.image, 2, x, y);
        draw_star(out.image, 0, s * (x - cx) + tx + cx, s * (y - cy) + ty + cy);
        Star st; st.x = float(x); st.y = float(y); out.catalog.stars.push_back(st);
    }
    return out;
}

} // namespace

TEST_CASE("too few stars for four parameters falls back to translation only",
          "[channel_registration]") {
    // 5 stars: below min_stars_affine (8), above min_stars_translation (3).
    Synth f = make_sparse_frame(600, 600, 5, 1.0, 0.4, -0.25);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::TranslationOnly);
    CHECK(red.s == 1.0);
    CHECK(std::abs(red.tx - 0.4)  < 0.05);
    CHECK(std::abs(red.ty + 0.25) < 0.05);
}

TEST_CASE("too few stars for translation falls back to identity",
          "[channel_registration]") {
    Synth f = make_sparse_frame(600, 600, 2, 1.0, 0.4, -0.25);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::Identity);
    REQUIRE(red.is_identity());
}

TEST_CASE("no stars at all yields empty transforms",
          "[channel_registration]") {
    Image img(400, 400, 3);
    img.fill(0.002f);
    ChannelTransforms ct = measure_channel_transforms(img, StarCatalog{}, 1);
    REQUIRE(ct.empty());
}

TEST_CASE("an implausible scale is rejected rather than applied",
          "[channel_registration]") {
    // 5% is five hundred times any real lateral colour. A fit this far out is
    // a bad solve, and applying it would wreck the frame.
    Synth f = make_frame(800, 800, 1.05, 0.0, 0.0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::Identity);
    REQUIRE(red.is_identity());
}

TEST_CASE("an implausible translation is rejected rather than applied",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0, 9.0, 0.0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::Identity);
    REQUIRE(red.is_identity());
}

TEST_CASE("a star invisible in one channel is dropped from that channel's fit "
          "and kept in the others", "[channel_registration]") {
    // The dual-narrowband case: red is Ha, blue is OIII, and plenty of stars
    // are strong in one and absent from the other.
    const double s = 1.0005, tx = 0.30, ty = -0.20;
    Synth f = make_frame(800, 800, s, tx, ty);

    // Erase half the red stars. Red must still fit -- from the survivors.
    const double cx = 399.5, cy = 399.5;
    int erased = 0;
    for (size_t i = 0; i < f.catalog.stars.size(); i += 2) {
        const Star& st = f.catalog.stars[i];
        const int rx = int(std::lround(s * (st.x - cx) + tx + cx));
        const int ry = int(std::lround(s * (st.y - cy) + ty + cy));
        for (int dy = -7; dy <= 7; dy++)
            for (int dx = -7; dx <= 7; dx++) {
                int px = rx + dx, py = ry + dy;
                if (px < 0 || px >= 800 || py < 0 || py >= 800) continue;
                f.image.at(px, py, 0) = 0.002f;
            }
        erased++;
    }
    REQUIRE(erased > 0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    const ChannelTransform& red = ct.per_channel[0];
    REQUIRE(red.fit == ChannelTransform::Fit::Affine);
    CHECK(red.n_stars < int(f.catalog.stars.size()));
    CHECK(red.n_stars > 0);
    const double R = std::hypot(cx, cy);
    CHECK(std::abs(red.s - s) * R < 0.03);

    // Blue was untouched and must have used every star.
    CHECK(ct.per_channel[2].n_stars == int(f.catalog.stars.size()));
}

TEST_CASE("a star with a close neighbour is excluded from the fit",
          "[channel_registration]") {
    // StarDetector's exclusion_radius is 5 by default and the centroid box is
    // 13 wide, so the detector hands over stars whose boxes overlap. Their
    // centroids get dragged by the neighbour, and by a different amount in
    // each channel, so they have to be dropped here.
    const double s = 1.0005, tx = 0.30, ty = -0.20;
    Synth f = make_frame(800, 800, s, tx, ty);
    const double cx = 399.5, cy = 399.5;

    // Give the first three stars a companion 8 px away, in every channel, and
    // put the companions in the catalog -- that is what the detector would do.
    const size_t n_before = f.catalog.stars.size();
    std::vector<std::pair<double, double>> companions;
    for (size_t i = 0; i < 3; i++)
        companions.emplace_back(f.catalog.stars[i].x + 8.0,
                                f.catalog.stars[i].y + 0.0);
    for (auto [ox, oy] : companions) {
        draw_star(f.image, 1, ox, oy, 0.3);
        draw_star(f.image, 2, ox, oy, 0.3);
        draw_star(f.image, 0, s * (ox - cx) + tx + cx, s * (oy - cy) + ty + cy, 0.3);
        Star st; st.x = float(ox); st.y = float(oy); f.catalog.stars.push_back(st);
    }

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    // Both members of each crowded pair go: 3 originals + 3 companions.
    REQUIRE(red.n_stars == int(n_before) - 3);

    // And the fit is still good, because the isolated stars carry it.
    const double R = std::hypot(cx, cy);
    CHECK(std::abs(red.s - s) * R < 0.02);
    CHECK(std::abs(red.tx - tx) < 0.02);
}

TEST_CASE("an outlier centroid is clipped out of the fit",
          "[channel_registration]") {
    const double s = 1.0005, tx = 0.30, ty = -0.20;
    Synth f = make_frame(800, 800, s, tx, ty);

    // Move one red star far from where the model says it should be, the way a
    // cosmic ray hit or a blended neighbour would.
    const double cx = 399.5, cy = 399.5;
    const Star& st = f.catalog.stars[3];
    const int rx = int(std::lround(s * (st.x - cx) + tx + cx));
    const int ry = int(std::lround(s * (st.y - cy) + ty + cy));
    draw_star(f.image, 0, rx + 3.0, ry + 3.0, 2.0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    // Without clipping, one 4 px outlier among 25 stars drags the fit well
    // past 0.02 px. With clipping it barely registers.
    const double R = std::hypot(cx, cy);
    CHECK(std::abs(red.s - s) * R < 0.02);
    CHECK(std::abs(red.tx - tx) < 0.02);
    CHECK(red.n_stars < int(f.catalog.stars.size()));
}

// --- console reporting ---------------------------------------------------

TEST_CASE("describe_channel_transforms names the rung and the size",
          "[channel_registration]") {
    ChannelTransforms ct;
    ct.cx = 399.5; ct.cy = 399.5; ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[0] = ChannelTransform{1.000238, 0.31, -0.12, 187, 0.043,
                                         ChannelTransform::Fit::Affine};
    ct.per_channel[1].fit = ChannelTransform::Fit::Identity;
    ct.per_channel[2] = ChannelTransform{1.0, 0.02, -0.01, 190, 0.031,
                                         ChannelTransform::Fit::TranslationOnly};

    const std::string s = describe_channel_transforms(ct, 565.0);

    // The rung, the size of the correction, and the star count all have to be
    // there: a user seeing a moved golden needs to know why from the log.
    CHECK(s.find("ch0") != std::string::npos);
    CHECK(s.find("affine") != std::string::npos);
    CHECK(s.find("238 ppm") != std::string::npos);
    CHECK(s.find("n=187") != std::string::npos);
    CHECK(s.find("ch2") != std::string::npos);
    CHECK(s.find("translation") != std::string::npos);
    CHECK(s.find("ch1") == std::string::npos);   // the reference is not reported

    // ASCII only. PCL reads const char* as ISO-8859-1, so a stray UTF-8 byte
    // reaches the Process Console as mojibake.
    for (unsigned char c : s) CHECK(c < 0x80);
}

TEST_CASE("describe_channel_transforms says so when there is nothing to say",
          "[channel_registration]") {
    CHECK(describe_channel_transforms(ChannelTransforms{}, 565.0).empty());
}

TEST_CASE("describe_channel_transforms names channels the fit gave up on",
          "[channel_registration]") {
    // A frame with too few isolated stars to measure anything is the opposite
    // situation from a frame whose channels already agreed: this one needs to
    // be visible in the console too, not silently indistinguishable from the
    // near-identity skip.
    ChannelTransforms ct;
    ct.cx = 399.5; ct.cy = 399.5; ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[0].fit = ChannelTransform::Fit::Identity;
    ct.per_channel[1].fit = ChannelTransform::Fit::Identity;   // reference
    ct.per_channel[2].fit = ChannelTransform::Fit::Identity;

    const std::string s = describe_channel_transforms(ct, 565.0);

    CHECK_FALSE(s.empty());
    CHECK(s.find("ch0") != std::string::npos);
    CHECK(s.find("ch2") != std::string::npos);
    CHECK(s.find("identity") != std::string::npos);
    CHECK(s.find("ch1") == std::string::npos);   // the reference is not reported
}

// ---------------------------------------------------------------------------
// Boundary cases the earlier work left untested. Erasing a star entirely is a
// different condition from a star that is present but below threshold, and it
// is the present-but-faint case that a dual-narrowband frame actually
// produces.
// ---------------------------------------------------------------------------

namespace {

// Deterministic, reproducible noise. A fixed lattice rather than a PRNG so a
// failure is the same failure on every machine and every run.
void add_fixed_noise(Image& img, int ch, float amplitude) {
    // xorshift per pixel. A plain hash of (x, y) was tried first and was a
    // trap: along the 48-pixel border of one centroid box its low byte
    // clumped, so the ring's MAD came out near 1e-4 and the SNR gate could
    // not bite however high the threshold went. The noise a gate test uses
    // has to have real robust spread, not merely be non-constant.
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++) {
            std::uint32_t st = static_cast<std::uint32_t>(x) * 1973u
                             + static_cast<std::uint32_t>(y) * 9277u + 1u;
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            const float u = static_cast<float>((st & 0xFFFFu) / 65535.0 - 0.5);
            img.at(x, y, ch) += amplitude * u;
        }
}

// Like make_frame, but the red stars are drawn at a caller-chosen amplitude so
// a test can put them just above or just below the gate.
Synth make_faint_red_frame(int w, int h, double s, double tx, double ty,
                           double red_amplitude) {
    Synth out;
    out.image = Image(w, h, 3);
    out.image.fill(0.002f);
    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;
    for (auto [x, y] : star_grid(w, h, 5)) {
        draw_star(out.image, 1, x, y);
        draw_star(out.image, 2, x, y);
        draw_star(out.image, 0, s * (x - cx) + tx + cx, s * (y - cy) + ty + cy,
                  red_amplitude);
        Star st;
        st.x = static_cast<float>(x);
        st.y = static_cast<float>(y);
        st.flux = 1.0f;
        out.catalog.stars.push_back(st);
    }
    return out;
}

} // namespace

TEST_CASE("a present but faint star is dropped when it falls under the SNR gate",
          "[channel_registration]") {
    // Red stars exist here -- they are not erased. What decides their fate is
    // the gate, and only the gate.
    Synth f = make_faint_red_frame(800, 800, 1.0005, 0.30, -0.20,
                                   /*red_amplitude*/ 0.05);
    add_fixed_noise(f.image, 0, 0.02f);

    ChannelRegistrationConfig permissive;
    permissive.min_star_snr = 0.5f;
    ChannelTransforms lo = measure_channel_transforms(f.image, f.catalog, 1,
                                                      permissive);
    REQUIRE(lo.per_channel[0].n_stars > 0);

    ChannelRegistrationConfig strict;
    strict.min_star_snr = 500.0f;   // no real star clears this
    ChannelTransforms hi = measure_channel_transforms(f.image, f.catalog, 1,
                                                      strict);

    // Same pixels, same stars, different threshold: the gate is what moved.
    INFO("lo n_stars=" << lo.per_channel[0].n_stars
         << " fit=" << int(lo.per_channel[0].fit)
         << " | hi n_stars=" << hi.per_channel[0].n_stars
         << " fit=" << int(hi.per_channel[0].fit));
    CHECK(hi.per_channel[0].n_stars < lo.per_channel[0].n_stars);
    CHECK(hi.per_channel[0].fit == ChannelTransform::Fit::Identity);
}

TEST_CASE("a noiseless field has zero sigma and is not rejected by the gate",
          "[channel_registration]") {
    // The MAD-degenerate corner. A synthetic frame with a flat background has
    // an exactly-zero ring spread, so `peak < min_snr * sigma` would be true
    // for every star at any threshold and the fit would silently vanish. The
    // implementation guards sigma <= 0 separately; this pins that it does.
    Synth f = make_frame(800, 800, 1.0005, 0.30, -0.20);

    ChannelRegistrationConfig cfg;
    cfg.min_star_snr = 1000.0f;   // absurd, and must not matter
    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1, cfg);

    CHECK(ct.per_channel[0].n_stars > 0);
    CHECK(ct.per_channel[0].fit == ChannelTransform::Fit::Affine);
}

TEST_CASE("when the clip would drop below min_stars_affine, the unclipped "
          "affine is kept rather than descending", "[channel_registration]") {
    // The corner the earlier work left untested, and it does NOT descend --
    // fit_channel only accepts the clipped refit when the survivors still
    // reach min_stars_affine (and the clip actually removed something).
    // Otherwise it returns the affine fitted on ALL pairs. Pinning the real
    // behaviour, because a reader would reasonably guess the opposite.
    const double s = 1.0005, tx = 0.30, ty = -0.20;
    const double cx = 399.5, cy = 399.5;

    auto with_outliers = [&]() {
        Synth f = make_frame(800, 800, s, tx, ty);
        // Displace four red stars well off the true transform. They stay
        // bright, so only the residual clip can remove them.
        for (size_t i = 0; i < f.catalog.stars.size(); i += 6) {
            const Star& st = f.catalog.stars[i];
            const double rx = s * (st.x - cx) + tx + cx;
            const double ry = s * (st.y - cy) + ty + cy;
            for (int dy = -7; dy <= 7; dy++)
                for (int dx = -7; dx <= 7; dx++) {
                    int px = int(std::lround(rx)) + dx;
                    int py = int(std::lround(ry)) + dy;
                    if (px < 0 || px >= 800 || py < 0 || py >= 800) continue;
                    f.image.at(px, py, 0) = 0.002f;
                }
            draw_star(f.image, 0, rx + 2.0, ry - 1.5);
        }
        return f;
    };

    Synth f = with_outliers();
    const int n_total = static_cast<int>(f.catalog.stars.size());

    // Default bar: the survivors clear it, so the clipped refit is used and
    // the outliers are gone from the count.
    ChannelTransforms clipped = measure_channel_transforms(f.image, f.catalog, 1);
    CHECK(clipped.per_channel[0].fit == ChannelTransform::Fit::Affine);
    CHECK(clipped.per_channel[0].n_stars < n_total);

    // Bar raised to the full star count: now the clip WOULD drop below it,
    // so the unclipped affine is kept and every pair is counted.
    ChannelRegistrationConfig strict_bar;
    strict_bar.min_stars_affine = n_total;
    ChannelTransforms unclipped =
        measure_channel_transforms(f.image, f.catalog, 1, strict_bar);
    CHECK(unclipped.per_channel[0].fit == ChannelTransform::Fit::Affine);
    CHECK(unclipped.per_channel[0].n_stars > clipped.per_channel[0].n_stars);
}
