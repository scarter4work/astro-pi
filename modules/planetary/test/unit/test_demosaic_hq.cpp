#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "Demosaic.h"
#include "StackTypes.h"

using namespace planetary;
using ser::ColorId;
using ser::FloatImage;
using ser::Frame;

namespace {
const ColorId kPatterns[4] = {ColorId::BayerRGGB, ColorId::BayerGRBG, ColorId::BayerGBRG, ColorId::BayerBGGR};

// Channel index (0=R,1=G,2=B) at site (x,y) -- written independently of the engine.
int SiteColor(ColorId p, int x, int y) {
    static const int rggb[4] = {0, 1, 1, 2}, grbg[4] = {1, 0, 2, 1},
                     gbrg[4] = {1, 2, 0, 1}, bggr[4] = {2, 1, 1, 0};
    const int* t = p == ColorId::BayerRGGB ? rggb : p == ColorId::BayerGRBG ? grbg
                   : p == ColorId::BayerGBRG ? gbrg : bggr;
    return t[(y & 1) * 2 + (x & 1)];
}

// Truth image in [0,1], interleaved RGB, quantised to 16 bits so the CFA holds it exactly.
FloatImage MakeTruth(int w, int h, const std::function<void(double, double, float*)>& rgbAt) {
    FloatImage t{w, h, 3, std::vector<float>(static_cast<size_t>(w) * h * 3)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* px = &t.data[(static_cast<size_t>(y) * w + x) * 3];
            rgbAt(x, y, px);
            for (int c = 0; c < 3; ++c) px[c] = static_cast<float>(std::lround(px[c] * 65535.0)) / 65535.0f;
        }
    return t;
}

Frame Mosaic(const FloatImage& truth, ColorId p) {
    Frame f{truth.width, truth.height, 1, std::vector<uint16_t>(static_cast<size_t>(truth.width) * truth.height)};
    for (int y = 0; y < truth.height; ++y)
        for (int x = 0; x < truth.width; ++x)
            f.samples[static_cast<size_t>(y) * truth.width + x] = static_cast<uint16_t>(
                std::lround(truth.data[(static_cast<size_t>(y) * truth.width + x) * 3 + SiteColor(p, x, y)] * 65535.0f));
    return f;
}

// Per-pixel colour error: the largest |out - truth| over the three channels.
std::vector<float> ErrorMap(const FloatImage& out, const FloatImage& truth) {
    REQUIRE(out.width == truth.width);
    REQUIRE(out.height == truth.height);
    REQUIRE(out.channels == 3);
    std::vector<float> e(static_cast<size_t>(out.width) * out.height);
    for (size_t i = 0; i < e.size(); ++i)
        for (int c = 0; c < 3; ++c) e[i] = std::max(e[i], std::fabs(out.data[i * 3 + c] - truth.data[i * 3 + c]));
    return e;
}

float MaxOf(const std::vector<float>& v) { return *std::max_element(v.begin(), v.end()); }

// Zipper = error that alternates pixel to pixel along an edge. With the signed per-channel
// error e = out - truth, the second difference along the edge direction,
// |e(p) - (e(p-1) + e(p+1))/2|, is ~0 for an error that varies smoothly along the edge and
// large for a zipper. `alongY` walks down columns (a near-vertical edge), else along rows.
float Zipper(const FloatImage& out, const FloatImage& truth, bool alongY) {
    const int w = out.width, h = out.height;
    auto err = [&](int x, int y, int c) {
        const size_t i = (static_cast<size_t>(y) * w + x) * 3 + c;
        return out.data[i] - truth.data[i];
    };
    float worst = 0;
    for (int y = alongY ? 1 : 0; y < (alongY ? h - 1 : h); ++y)
        for (int x = alongY ? 0 : 1; x < (alongY ? w : w - 1); ++x)
            for (int c = 0; c < 3; ++c) {
                const float before = alongY ? err(x, y - 1, c) : err(x - 1, y, c);
                const float after = alongY ? err(x, y + 1, c) : err(x + 1, y, c);
                worst = std::max(worst, std::fabs(err(x, y, c) - 0.5f * (before + after)));
            }
    return worst;
}

// Area-sampled (8x8 supersampled) coverage of a half-plane / disc: an anti-aliased limb.
double Coverage(double x, double y, const std::function<bool(double, double)>& inside) {
    int n = 0;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i)
            if (inside(x + (i + 0.5) / 8.0, y + (j + 0.5) / 8.0)) ++n;
    return n / 64.0;
}

struct EdgeCase {
    const char* name;
    std::function<void(double, double, float*)> rgbAt;
    enum class Along { Columns, Rows, None } zipper;  // edge direction; None for a disc (it turns)
};

std::vector<EdgeCase> GreyEdges() {
    auto grey = [](double v, float* px) { px[0] = px[1] = px[2] = static_cast<float>(v); };
    return {
        {"sharp vertical edge", [=](double x, double, float* px) { grey(x >= 15 ? 1.0 : 0.0, px); }, EdgeCase::Along::Columns},
        {"sharp horizontal edge", [=](double, double y, float* px) { grey(y >= 15 ? 1.0 : 0.0, px); }, EdgeCase::Along::Rows},
        {"anti-aliased slanted edge (1:6)",
         [=](double x, double y, float* px) {
             grey(Coverage(x, y, [](double u, double v) { return u >= 12.3 + v / 6.0; }), px);
         },
         EdgeCase::Along::Columns},
        {"anti-aliased slanted edge (6:1)",
         [=](double x, double y, float* px) {
             grey(Coverage(x, y, [](double u, double v) { return v >= 13.6 + u / 6.0; }), px);
         },
         EdgeCase::Along::Rows},
        {"sharp disc r=10",
         [=](double x, double y, float* px) {
             grey((x - 16) * (x - 16) + (y - 16) * (y - 16) <= 100 ? 1.0 : 0.0, px);
         },
         EdgeCase::Along::None},
        {"anti-aliased disc r=10.4",
         [=](double x, double y, float* px) {
             grey(Coverage(x, y, [](double u, double v) {
                      return (u - 16.2) * (u - 16.2) + (v - 15.7) * (v - 15.7) <= 10.4 * 10.4;
                  }),
                  px);
         },
         EdgeCase::Along::None},
    };
}
}  // namespace

TEST_CASE("Progress and StackCancelled are usable", "[stack][types]") {
    stack::Progress progress;
    CHECK_FALSE(progress.report);
    CHECK_FALSE(progress.cancelled);
    int calls = 0;
    progress.report = [&](const std::string& phase, int64_t done, int64_t total) {
        CHECK(phase == "measure");
        CHECK(done <= total);
        ++calls;
    };
    progress.cancelled = [] { return true; };
    progress.report("measure", 3, 10);
    CHECK(calls == 1);
    CHECK(progress.cancelled());
    CHECK_THROWS_AS(throw stack::StackCancelled("cancelled"), std::runtime_error);
}

TEST_CASE("measurement luma of a CFA is the 2x2 cell mean at half resolution", "[stack][luma]") {
    // 5x5, depth 8: the trailing odd row and column are dropped -> 2x2 output.
    const int m[5][5] = {{ 10, 200,  35, 140,  99},
                         { 90,  17, 230,  60,  99},
                         {170,  45,  80, 250,  99},
                         { 25, 120,  66, 190,  99},
                         { 99,  99,  99,  99,  99}};
    Frame cfa{5, 5, 1, std::vector<uint16_t>(25)};
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) cfa.samples[y * 5 + x] = static_cast<uint16_t>(m[y][x]);
    // Hand sums: (10+200+90+17)=317, (35+140+230+60)=465, (170+45+25+120)=360, (80+250+66+190)=586.
    const float expected[4] = {317.0f / 1020, 465.0f / 1020, 360.0f / 1020, 586.0f / 1020};
    for (ColorId p : kPatterns) {
        FloatImage l = stack::LumaForMeasure(cfa, p, 8);
        REQUIRE(l.width == 2);
        REQUIRE(l.height == 2);
        REQUIRE(l.channels == 1);
        REQUIRE(l.data.size() == 4);
        for (int i = 0; i < 4; ++i) CHECK(l.data[i] == Catch::Approx(expected[i]).epsilon(1e-6));
    }
}

TEST_CASE("measurement luma of mono is the frame, of RGB the channel mean", "[stack][luma]") {
    Frame mono{3, 1, 1, {0, 2048, 4095}};
    FloatImage l = stack::LumaForMeasure(mono, ColorId::Mono, 12);
    REQUIRE(l.width == 3);
    REQUIRE(l.height == 1);
    REQUIRE(l.channels == 1);
    CHECK(l.data[0] == 0.0f);
    CHECK(l.data[1] == Catch::Approx(2048.0f / 4095));
    CHECK(l.data[2] == 1.0f);

    Frame rgb{2, 1, 3, {255, 0, 0, 30, 60, 90}};
    for (ColorId c : {ColorId::RGB, ColorId::BGR}) {
        FloatImage r = stack::LumaForMeasure(rgb, c, 8);
        REQUIRE(r.width == 2);
        REQUIRE(r.height == 1);
        REQUIRE(r.channels == 1);
        CHECK(r.data[0] == Catch::Approx(85.0f / 255));
        CHECK(r.data[1] == Catch::Approx(60.0f / 255));
    }
}

TEST_CASE("luma and demosaic reject malformed input", "[stack][luma]") {
    Frame tiny{1, 1, 1, {5}};
    CHECK_THROWS_AS(stack::LumaForMeasure(tiny, ColorId::BayerRGGB, 8), ser::SerError);
    CHECK_THROWS_AS(stack::DemosaicHQ(tiny, ColorId::BayerRGGB, 8), ser::SerError);
    Frame threeCh{2, 2, 3, std::vector<uint16_t>(12)};
    CHECK_THROWS_AS(stack::LumaForMeasure(threeCh, ColorId::BayerGRBG, 8), ser::SerError);
    CHECK_THROWS_AS(stack::DemosaicHQ(threeCh, ColorId::BayerGRBG, 8), ser::SerError);
    Frame shortFrame{4, 4, 1, std::vector<uint16_t>(15)};
    CHECK_THROWS_AS(stack::LumaForMeasure(shortFrame, ColorId::Mono, 8), ser::SerError);
    CHECK_THROWS_AS(stack::DemosaicHQ(shortFrame, ColorId::BayerBGGR, 8), ser::SerError);
    Frame twoCh{2, 2, 2, std::vector<uint16_t>(8)};
    CHECK_THROWS_AS(stack::LumaForMeasure(twoCh, ColorId::Mono, 8), ser::SerError);
    Frame ok{4, 4, 1, std::vector<uint16_t>(16)};
    CHECK_THROWS_AS(stack::LumaForMeasure(ok, ColorId::Mono, 0), ser::SerError);
    CHECK_THROWS_AS(stack::DemosaicHQ(ok, ColorId::BayerRGGB, 17), ser::SerError);
}

TEST_CASE("DemosaicHQ passes mono and RGB through normalised", "[stack][demosaic]") {
    Frame mono{2, 2, 1, {0, 100, 200, 255}};
    FloatImage m = stack::DemosaicHQ(mono, ColorId::Mono, 8);
    CHECK(m.channels == 1);
    CHECK(m.data == ser::Normalize(mono, 8).data);
    Frame rgb{1, 2, 3, {1, 2, 3, 4095, 0, 7}};
    FloatImage r = stack::DemosaicHQ(rgb, ColorId::RGB, 12);
    CHECK(r.channels == 3);
    CHECK(r.width == 1);
    CHECK(r.height == 2);
    CHECK(r.data == ser::Normalize(rgb, 12).data);
}

TEST_CASE("DemosaicHQ is exact on flat fields, borders included", "[stack][demosaic]") {
    const float colours[2][3] = {{0.5f, 0.5f, 0.5f}, {0.8f, 0.5f, 0.2f}};
    for (const auto& rgb : colours)
        for (ColorId p : kPatterns)
            for (auto [w, h] : {std::pair{2, 2}, std::pair{3, 3}, std::pair{7, 5}, std::pair{16, 9}}) {
                FloatImage truth = MakeTruth(w, h, [&](double, double, float* px) {
                    for (int c = 0; c < 3; ++c) px[c] = rgb[c];
                });
                FloatImage out = stack::DemosaicHQ(Mosaic(truth, p), p, 16);
                REQUIRE(out.channels == 3);
                INFO(ser::ColorIdName(p) << " " << w << "x" << h << " rgb " << rgb[0] << "," << rgb[1] << "," << rgb[2]);
                CHECK(MaxOf(ErrorMap(out, truth)) <= 1e-5f);
            }
}

TEST_CASE("DemosaicHQ is exact on linear colour ramps away from the border", "[stack][demosaic]") {
    const int w = 24, h = 20;
    for (ColorId p : kPatterns) {
        FloatImage truth = MakeTruth(w, h, [](double x, double y, float* px) {
            px[0] = static_cast<float>(0.10 + 0.020 * x + 0.010 * y);
            px[1] = static_cast<float>(0.20 + 0.015 * x - 0.005 * y + 0.1);
            px[2] = static_cast<float>(0.05 + 0.010 * x + 0.020 * y);
        });
        std::vector<float> e = ErrorMap(stack::DemosaicHQ(Mosaic(truth, p), p, 16), truth);
        float worst = 0;
        for (int y = 3; y < h - 3; ++y)
            for (int x = 3; x < w - 3; ++x) worst = std::max(worst, e[static_cast<size_t>(y) * w + x]);
        INFO(ser::ColorIdName(p) << ": worst interior error " << worst);
        CHECK(worst <= 1e-4f);  // 16-bit quantisation of the ramp is ~1.5e-5 per sample
    }
}

TEST_CASE("DemosaicHQ halves bilinear's colour error at sharp edges, with no zipper", "[stack][demosaic]") {
    const int w = 32, h = 32;
    for (const EdgeCase& ec : GreyEdges())
        for (ColorId p : kPatterns) {
            FloatImage truth = MakeTruth(w, h, ec.rgbAt);
            Frame cfa = Mosaic(truth, p);
            const FloatImage hqImage = stack::DemosaicHQ(cfa, p, 16);
            const FloatImage blImage = ser::DebayerBilinear(cfa, p, 16);
            std::vector<float> eHq = ErrorMap(hqImage, truth);
            const float hq = MaxOf(eHq), bl = MaxOf(ErrorMap(blImage, truth));
            INFO(ec.name << " " << ser::ColorIdName(p) << ": max colour error HQ " << hq << " bilinear " << bl);
            REQUIRE(bl > 0.1f);  // the case really stresses bilinear
            CHECK(hq <= 0.5f * bl);

            // Zipper: alternation of the error along the edge direction.
            if (ec.zipper != EdgeCase::Along::None) {
                const bool alongY = ec.zipper == EdgeCase::Along::Columns;
                const float zHq = Zipper(hqImage, truth, alongY), zBl = Zipper(blImage, truth, alongY);
                INFO("zipper HQ " << zHq << " bilinear " << zBl);
                REQUIRE(zBl > 0.1f);  // bilinear really zippers here
                CHECK(zHq <= 0.5f * zBl);
            }

            // Flat area: wherever the truth is constant over the 7x7 window, HQ is exact.
            float flatWorst = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    bool flat = true;
                    const float v = truth.data[(static_cast<size_t>(y) * w + x) * 3];
                    for (int dy = -3; dy <= 3 && flat; ++dy)
                        for (int dx = -3; dx <= 3 && flat; ++dx) {
                            const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
                            flat = truth.data[(static_cast<size_t>(yy) * w + xx) * 3] == v;
                        }
                    if (flat) flatWorst = std::max(flatWorst, eHq[static_cast<size_t>(y) * w + x]);
                }
            CHECK(flatWorst <= 1e-5f);
        }
}

TEST_CASE("DemosaicHQ halves bilinear's colour error on a coloured disc limb", "[stack][demosaic]") {
    // An orange planet on black sky: colour differences jump at the limb, the hard case
    // for any colour-difference method.
    const int w = 40, h = 40;
    for (ColorId p : kPatterns) {
        FloatImage truth = MakeTruth(w, h, [](double x, double y, float* px) {
            const double c = Coverage(x, y, [](double u, double v) {
                return (u - 20.3) * (u - 20.3) + (v - 19.6) * (v - 19.6) <= 13.2 * 13.2;
            });
            px[0] = static_cast<float>(0.9 * c);
            px[1] = static_cast<float>(0.6 * c);
            px[2] = static_cast<float>(0.3 * c);
        });
        Frame cfa = Mosaic(truth, p);
        const float hq = MaxOf(ErrorMap(stack::DemosaicHQ(cfa, p, 16), truth));
        const float bl = MaxOf(ErrorMap(ser::DebayerBilinear(cfa, p, 16), truth));
        INFO(ser::ColorIdName(p) << ": max colour error HQ " << hq << " bilinear " << bl);
        CHECK(hq <= 0.5f * bl);
    }
}
