// Phase C's single row-parallel pass (post_process_record) against the five
// serial sweeps it replaced, written out here as the reference: dominant
// shape, quality score, spatial-context writeback, quality map, measured
// noise. Every voxel field and every map pixel must agree bit for bit, on a
// cube large enough to span many row chunks, in memory and file-backed.

#include "catch_amalgamated.hpp"
#include "nukex/stacker/post_process.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/io/image.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

using namespace nukex;
namespace fs = std::filesystem;

namespace {

ChannelConfig osc_config() {
    Filter f;
    f.cls  = FilterClass::BROADBAND_OSC;
    f.name = "OSC";
    return ChannelConfig::from_filter(f);
}

// Phase B's outputs, randomised, identically into every cube given.
void fill_phase_b(std::vector<Cube*> cubes, int nc, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::uniform_int_distribution<int> shape(0, 6), frames(0, 40);
    const Cube& c0 = *cubes[0];
    for (int y = 0; y < c0.height; y++)
        for (int x = 0; x < c0.width; x++) {
            const int nf = frames(rng);
            const int cloud = nf > 0 ? std::uniform_int_distribution<int>(0, nf)(rng) : 0;
            std::vector<float> vals;
            for (int c = 0; c < nc; c++)
                for (int k = 0; k < 4; k++) vals.push_back(u(rng) * (k == 3 ? 120.0f : 1.0f));
            std::vector<int> shapes;
            for (int c = 0; c < nc; c++) shapes.push_back(shape(rng));
            for (Cube* cube : cubes) {
                auto& v = cube->at(x, y);
                v.n_frames = static_cast<uint16_t>(nf);
                v.cloud_frame_count = static_cast<uint16_t>(cloud);
                for (int c = 0; c < nc; c++) {
                    auto& ch = v.channel(c);
                    ch.distribution.true_signal_estimate = vals[c * 4 + 0];
                    ch.distribution.signal_uncertainty   = vals[c * 4 + 1];
                    ch.distribution.confidence           = vals[c * 4 + 2];
                    ch.snr                               = vals[c * 4 + 3];
                    ch.distribution.shape = static_cast<DistributionShape>(shapes[c]);
                }
            }
        }
}

// The five sweeps as they were.
void reference(Cube& cube, int n_ch, const std::vector<float>& g, const std::vector<float>& b,
               const std::vector<float>& r, Image& quality, Image& measured) {
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++) {
            auto& v = cube.at(x, y);
            int counts[7] = {};
            for (int ch = 0; ch < n_ch; ch++) {
                int s = static_cast<int>(v.channel(ch).distribution.shape);
                if (s >= 0 && s < 7) counts[s]++;
            }
            int best = 0;
            for (int i = 1; i < 7; i++) if (counts[i] > counts[best]) best = i;
            v.dominant_shape = static_cast<DistributionShape>(best);
        }
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++) {
            auto& v = cube.at(x, y);
            float avg_snr = 0.0f;
            for (int ch = 0; ch < n_ch; ch++) avg_snr += v.channel(ch).snr;
            avg_snr /= n_ch;
            float cloud_fraction = (v.n_frames > 0) ?
                static_cast<float>(v.cloud_frame_count) / v.n_frames : 0.0f;
            v.quality_score = v.channel(0).distribution.confidence * (1.0f - cloud_fraction)
                            * std::min(1.0f, avg_snr / 50.0f);
            v.confidence = v.channel(0).distribution.confidence;
        }
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++) {
            auto& v = cube.at(x, y);
            const int pi = y * cube.width + x;
            v.gradient_mag = g[pi];
            v.local_background = b[pi];
            v.local_rms = r[pi];
        }
    quality = Image(cube.width, cube.height, 4);
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++) {
            const auto& v = cube.at(x, y);
            float avg_signal = 0.0f, max_unc = 0.0f, min_conf = 1.0f;
            for (int ch = 0; ch < n_ch; ch++) {
                const auto& d = v.channel(ch).distribution;
                avg_signal += d.true_signal_estimate;
                max_unc = std::max(max_unc, d.signal_uncertainty);
                min_conf = std::min(min_conf, d.confidence);
            }
            avg_signal /= n_ch;
            quality.at(x, y, 0) = avg_signal;
            quality.at(x, y, 1) = max_unc;
            quality.at(x, y, 2) = min_conf;
            quality.at(x, y, 3) = static_cast<float>(static_cast<uint8_t>(v.dominant_shape));
        }
    measured = Image(cube.width, cube.height, 1);
    for (int y = 0; y < cube.height; y++)
        for (int x = 0; x < cube.width; x++) measured.at(x, y, 0) = cube.at(x, y).local_rms;
}

bool same_image(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height() || a.n_channels() != b.n_channels())
        return false;
    const std::size_t n = static_cast<std::size_t>(a.width()) * a.height() * a.n_channels();
    return std::memcmp(a.data(), b.data(), n * sizeof(float)) == 0;
}

void require_same_record(const Cube& a, const Cube& b) {
    long mismatches = 0;
    for (int y = 0; y < a.height; y++)
        for (int x = 0; x < a.width; x++) {
            const auto& va = a.at(x, y);
            const auto& vb = b.at(x, y);
            auto ne = [&](float p, float q) { if (std::memcmp(&p, &q, sizeof p) != 0) ++mismatches; };
            if (va.dominant_shape != vb.dominant_shape) ++mismatches;
            ne(va.quality_score, vb.quality_score);
            ne(va.confidence, vb.confidence);
            ne(va.gradient_mag, vb.gradient_mag);
            ne(va.local_background, vb.local_background);
            ne(va.local_rms, vb.local_rms);
        }
    REQUIRE(mismatches == 0);
}

void run_case(Cube& ref, Cube& got) {
    const ChannelConfig cfg = ref.channel_config;
    const int nc = cfg.n_channels;
    fill_phase_b({&ref, &got}, nc, 31337u);
    const std::size_t npix = static_cast<std::size_t>(ref.width) * ref.height;
    std::vector<float> g(npix), bg(npix), r(npix);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (std::size_t i = 0; i < npix; i++) { g[i] = u(rng); bg[i] = u(rng); r[i] = u(rng); }

    Image q_ref, m_ref, q_got, m_got;
    reference(ref, nc, g, bg, r, q_ref, m_ref);
    post_process_record(got, nc, g, bg, r, q_got, m_got);

    require_same_record(ref, got);
    REQUIRE(same_image(q_ref, q_got));
    REQUIRE(same_image(m_ref, m_got));
}

} // namespace

TEST_CASE("post_process_record equals the five serial sweeps it replaced",
          "[post_process]") {
    const ChannelConfig cfg = osc_config();
    Cube ref(61, 47, cfg), got(61, 47, cfg);   // 12 row chunks, odd width
    run_case(ref, got);
}

TEST_CASE("post_process_record on a FILE-BACKED record equals the serial sweeps",
          "[post_process][file_backed]") {
#if defined(_WIN32)
    SKIP("Cube file backing is not implemented on Windows");
#else
    const ChannelConfig cfg = osc_config();
    Cube ref(45, 33, cfg);
    Cube got(45, 33, cfg, fs::temp_directory_path().string());
    REQUIRE(got.file_backed());
    run_case(ref, got);
#endif
}

TEST_CASE("post_process_record refuses spatial context of the wrong size",
          "[post_process]") {
    const ChannelConfig cfg = osc_config();
    Cube cube(8, 6, cfg);
    std::vector<float> ok(48, 0.0f), short_(47, 0.0f);
    Image q, m;
    REQUIRE_THROWS_AS(post_process_record(cube, cfg.n_channels, ok, short_, ok, q, m),
                      std::invalid_argument);
    REQUIRE_NOTHROW(post_process_record(cube, cfg.n_channels, ok, ok, ok, q, m));
    REQUIRE(q.n_channels() == 4);
    REQUIRE(m.n_channels() == 1);
}
