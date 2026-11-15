// The parallel triangle matcher and the parallel warp give exactly what the
// serial versions gave.
//
// The matcher's golden values were recorded from the serial implementation
// (commit 68e33c55) before it was parallelised: the match LIST -- order
// included, because the homography consumes it in order -- hashed, plus its
// length. The warp is compared against the serial loop it replaced, written
// out below, pixel for pixel and coverage bit for coverage bit.

#include "catch_amalgamated.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/channel_registration.hpp"
#include "nukex/core/coverage_mask.hpp"
#include "nukex/io/image.hpp"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using namespace nukex;

namespace {

std::uint32_t xs(std::uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

// A reference catalogue and a rotated, shifted source view of it, with some
// stars lost and some spurious ones added -- what two exposures give.
void make_catalogues(StarCatalog& src, StarCatalog& ref) {
    std::uint32_t s = 20261115u;
    std::vector<Star> sky;
    for (int i = 0; i < 180; i++) {
        Star st;
        st.x = static_cast<float>(xs(s) % 600000) / 100.0f;
        st.y = static_cast<float>(xs(s) % 400000) / 100.0f;
        st.flux = 100.0f + static_cast<float>(xs(s) % 100000) / 10.0f;
        sky.push_back(st);
    }
    ref.stars = sky;
    const float a = 0.3f * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a);
    for (std::size_t i = 0; i < sky.size(); i++) {
        if (i % 12 == 5) continue;                       // lost in this frame
        Star st = sky[i];
        const float x = sky[i].x, y = sky[i].y;
        st.x = c * x - sn * y + 12.5f + static_cast<float>(xs(s) % 100) / 1000.0f;
        st.y = sn * x + c * y - 7.25f + static_cast<float>(xs(s) % 100) / 1000.0f;
        st.flux *= 0.9f + static_cast<float>(xs(s) % 200) / 1000.0f;
        src.stars.push_back(st);
    }
    for (int i = 0; i < 20; i++) {                       // spurious detections
        Star st;
        st.x = static_cast<float>(xs(s) % 600000) / 100.0f;
        st.y = static_cast<float>(xs(s) % 400000) / 100.0f;
        st.flux = 100.0f + static_cast<float>(xs(s) % 100000) / 10.0f;
        src.stars.push_back(st);
    }
}

std::uint64_t fnv1a(const std::vector<std::pair<int, int>>& m) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& [a, b] : m)
        for (int v : {a, b})
            for (int k = 0; k < 4; k++) { h ^= static_cast<std::uint8_t>(v >> (8 * k)); h *= 1099511628211ULL; }
    return h;
}

// The serial warp loop, as it was before the rows were parallelised.
Image reference_warp(const Image& source, const HomographyMatrix& H, int ow, int oh,
                     const ChannelTransforms& channels, CoverageMask* coverage) {
    Image output(ow, oh, source.n_channels());
    if (coverage) *coverage = CoverageMask(ow, oh, source.n_channels());
    Eigen::Matrix3f He;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) He(r, c) = H(r, c);
    Eigen::Matrix3f Hi = He.inverse();
    const int sw = source.width(), sh = source.height();
    for (int ch = 0; ch < source.n_channels(); ch++) {
        const bool has_ct = ch < static_cast<int>(channels.per_channel.size());
        const ChannelTransform ct = has_ct ? channels.per_channel[ch] : ChannelTransform{};
        const bool apply_ct = has_ct && !ct.is_identity();
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                float w = Hi(2, 0) * x + Hi(2, 1) * y + Hi(2, 2);
                if (std::abs(w) < 1e-10f) continue;
                float sx = (Hi(0, 0) * x + Hi(0, 1) * y + Hi(0, 2)) / w;
                float sy = (Hi(1, 0) * x + Hi(1, 1) * y + Hi(1, 2)) / w;
                if (apply_ct) ct.apply(channels.cx, channels.cy, sx, sy);
                if (!std::isfinite(sx) || !std::isfinite(sy)) continue;
                if (sx < 0 || sx > sw - 1 || sy < 0 || sy > sh - 1) continue;
                const int ix = std::min(static_cast<int>(sx), sw - 2);
                const int iy = std::min(static_cast<int>(sy), sh - 2);
                const float fx = sx - ix, fy = sy - iy;
                float v00 = source.at(ix, iy, ch), v10 = source.at(ix + 1, iy, ch);
                float v01 = source.at(ix, iy + 1, ch), v11 = source.at(ix + 1, iy + 1, ch);
                output.at(x, y, ch) = v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy)
                                    + v01 * (1 - fx) * fy + v11 * fx * fy;
                if (coverage) coverage->set_covered(ch, y, x, true);
            }
    }
    return output;
}

} // namespace

TEST_CASE("StarMatcher::match reproduces the serial matcher's list exactly",
          "[star_matcher][parallel]") {
    StarCatalog src, ref;
    make_catalogues(src, ref);
    const auto m = StarMatcher::match(src, ref, StarMatcher::Config{});
    auto sorted = m;
    std::sort(sorted.begin(), sorted.end());
    CAPTURE(m.size(), fnv1a(m), fnv1a(sorted));
    REQUIRE(m.size() == 88u);
    REQUIRE(fnv1a(sorted) == 0x03c3368f5927d6f6ULL);   // the set, on any standard library
#if defined(__GLIBCXX__)
    // The order comes out of std::unordered_map iteration, so it is pinned only
    // on the standard library it was recorded with.
    REQUIRE(fnv1a(m) == 0xb38f3e6ab6b7f1f6ULL);
#endif
}

TEST_CASE("HomographyComputer::warp equals the serial loop, pixels and coverage",
          "[homography][parallel]") {
    // Odd sizes and three channels, so the 8-row blocks of the global
    // (channel, row) index straddle channel boundaries mid-block and no row
    // starts on a byte of the coverage mask.
    // oh shares a factor with the channel count, so no (channel, row) mix-up
    // can happen to land on a permutation of the rows.
    const int sw = 211, sh = 157, ow = 203, oh = 150;
    Image src(sw, sh, 3);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) src.at(x, y, c) = u(rng);
    HomographyMatrix H;
    const float a = 2.0f * 3.14159265f / 180.0f;
    H.H = {{{std::cos(a) * 1.01f, -std::sin(a), 6.3f},
            {std::sin(a), std::cos(a) * 0.99f, -4.1f},
            {1e-5f, -2e-5f, 1.0f}}};
    ChannelTransforms ct;
    ct.cx = sw / 2.0; ct.cy = sh / 2.0;
    ct.per_channel.resize(3);
    ct.per_channel[0].tx = 0.7; ct.per_channel[0].ty = -0.4;   // red displaced
    ct.per_channel[0].fit = ChannelTransform::Fit::TranslationOnly;
    ct.per_channel[2].tx = -0.5; ct.per_channel[2].ty = 0.3;   // and blue, differently
    ct.per_channel[2].fit = ChannelTransform::Fit::TranslationOnly;

    for (bool with_ct : {false, true}) {
        INFO("channel transforms " << with_ct);
        const ChannelTransforms& use = with_ct ? ct : ChannelTransforms{};
        CoverageMask cov_ref, cov_got;
        Image ref = reference_warp(src, H, ow, oh, use, &cov_ref);
        Image got = HomographyComputer::warp(src, H, ow, oh, use, cov_got);
        REQUIRE(got.width() == ow);
        REQUIRE(got.n_channels() == 3);
        REQUIRE(std::memcmp(ref.data(), got.data(), sizeof(float) * ow * oh * 3) == 0);
        long bit_mismatch = 0, covered = 0, uncovered = 0;
        for (int c = 0; c < 3; c++)
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++) {
                    if (cov_ref.covered(c, y, x) != cov_got.covered(c, y, x)) ++bit_mismatch;
                    (cov_got.covered(c, y, x) ? covered : uncovered)++;
                }
        REQUIRE(bit_mismatch == 0);
        REQUIRE(covered > 0);
        REQUIRE(uncovered > 0);   // the transform really leaves the source
        // And the overload without a mask gives the same pixels.
        Image nomask = HomographyComputer::warp(src, H, ow, oh, use);
        REQUIRE(std::memcmp(ref.data(), nomask.data(), sizeof(float) * ow * oh * 3) == 0);
    }
}
