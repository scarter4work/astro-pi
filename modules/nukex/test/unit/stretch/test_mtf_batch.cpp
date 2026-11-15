#include "catch_amalgamated.hpp"
#include "nukex/io/image.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#include <chrono>
#include <cstdio>
#include <random>

using namespace nukex;

namespace {
Image random_image(int w, int h, int c, unsigned seed) {
    Image im(w, h, c);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (std::size_t i = 0; i < im.data_size(); ++i) im.data()[i] = u(rng);
    return im;
}

double best_of(int reps, const Image& src, const MTFStretch& op) {
    double best = 1e30;
    for (int k = 0; k < reps; ++k) {
        Image work = src.clone();
        const auto t0 = std::chrono::steady_clock::now();
        op.apply(work);
        best = std::min(best, std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}
} // namespace

namespace {
// Distance in units in the last place; NaN == NaN counts as 0, NaN vs not-NaN
// as "infinitely far".
std::int64_t ulp_distance(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) return (std::isnan(a) && std::isnan(b)) ? 0 : INT64_MAX;
    auto key = [](float f) {
        const std::int32_t i = std::bit_cast<std::int32_t>(f);
        return i < 0 ? static_cast<std::int64_t>(INT32_MIN) - i : static_cast<std::int64_t>(i);
    };
    const std::int64_t d = key(a) - key(b);
    return d < 0 ? -d : d;
}

std::vector<float> probe_inputs() {
    std::vector<float> v;
    for (int i = -20000; i <= 220000; ++i) v.push_back(static_cast<float>(i) / 200000.0f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-0.2f, 1.2f);
    for (int i = 0; i < 100000; ++i) v.push_back(u(rng));
    const float specials[] = {0.0f, -0.0f, 1.0f, std::numeric_limits<float>::denorm_min(),
                              std::numeric_limits<float>::min(), 1.0f - 1e-7f,
                              std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()};
    for (float f : specials) v.push_back(f);
    return v;
}

struct Params { float m, s, h; };
const Params kParams[] = {
    {0.25f, 0.0f, 1.0f}, {0.12f, 0.01f, 0.98f}, {0.5f, 0.0f, 1.0f}, {0.9f, 0.05f, 0.95f},
    {0.001f, 0.0f, 1.0f}, {0.999f, 0.1f, 0.9f},
    {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {-0.5f, 0.0f, 1.0f}, {1.5f, 0.0f, 1.0f}, // m edge cases
    {0.25f, 0.5f, 0.5f}, {0.25f, 0.6f, 0.4f},                                       // empty range
    {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f},
};

MTFStretch make(const Params& p, bool lum) {
    MTFStretch op;
    op.midtone = p.m; op.shadows = p.s; op.highlights = p.h; op.luminance_only = lum;
    return op;
}

bool same_bits(const Image& a, const Image& b) {
    return a.data_size() == b.data_size() &&
           std::memcmp(a.data(), b.data(), a.data_size() * sizeof(float)) == 0;
}
} // namespace

TEST_CASE("MTF: batched apply(in, out, n) matches apply_scalar bit for bit",
          "[mtf][batch]") {
    const std::vector<float> in = probe_inputs();
    std::vector<float> out(in.size()), inplace;
    for (const auto& p : kParams) {
        INFO("midtone " << p.m << " shadows " << p.s << " highlights " << p.h);
        const MTFStretch op = make(p, false);
        op.apply(in.data(), out.data(), in.size());
        inplace = in;
        op.apply(inplace.data(), inplace.data(), inplace.size());
        std::int64_t worst = 0;
        std::size_t differing = 0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            const std::int64_t d = ulp_distance(out[i], op.apply_scalar(in[i]));
            worst = std::max(worst, d);
            differing += (std::bit_cast<std::uint32_t>(out[i]) !=
                          std::bit_cast<std::uint32_t>(op.apply_scalar(in[i])));
        }
        CHECK(worst <= 1);       // the contract: within 1 ULP ...
        CHECK(differing == 0);   // ... and in fact bit-identical
        CHECK(std::memcmp(inplace.data(), out.data(), out.size() * sizeof(float)) == 0);
    }
}

TEST_CASE("MTF: apply(Image&) is bit-identical to the per-pixel path",
          "[mtf][batch]") {
    for (int nch : {1, 2, 3}) {
        // 5000 x 3 pixels: more than one 4096-pixel luminance block, plus a
        // tail; a few black (L < 1e-10) and non-finite pixels.
        Image src = random_image(5000, 3, nch, 11 + nch);
        src.data()[17] = 0.0f;
        src.data()[4096] = std::numeric_limits<float>::quiet_NaN();
        src.data()[src.data_size() - 1] = std::numeric_limits<float>::infinity();
        for (const auto& p : kParams) {
            for (bool lum : {false, true}) {
                INFO("nch " << nch << " lum " << lum << " midtone " << p.m
                     << " shadows " << p.s << " highlights " << p.h);
                const MTFStretch op = make(p, lum);
                Image got = src.clone();
                op.apply(got);

                // The pre-batch route: per-pixel std::function over apply_scalar.
                Image want = src.clone();
                auto fn = [&op](float x) { return op.apply_scalar(x); };
                if (lum) apply_luminance_only(want, fn);
                else     apply_per_channel(want, fn);
                clamp_image(want);

                CHECK(same_bits(got, want));
            }
        }
    }
}

// Opt-in: `test_mtf_batch "[.bench]"`. 6074 x 4074 RGB, the size of the IC1805
// stack the bridge was measured on.
TEST_CASE("MTF: apply() throughput, 25 MP RGB", "[.bench]") {
    const Image src = random_image(6074, 4074, 3, 1);
    MTFStretch op;
    op.midtone = 0.12f; op.shadows = 0.01f; op.highlights = 0.98f;
    op.luminance_only = false;
    const double per_ch = best_of(5, src, op);
    op.luminance_only = true;
    const double lum = best_of(5, src, op);
    const double mpx = 6074.0 * 4074.0 / 1e6;
    std::printf("MTF per-channel: %.1f ms (%.0f Msample/s)\n", per_ch * 1e3, 3 * mpx / per_ch);
    std::printf("MTF luminance:   %.1f ms (%.0f Mpixel/s)\n", lum * 1e3, mpx / lum);
}
