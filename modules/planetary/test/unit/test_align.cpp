#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Align.h"
#include "AviWriter.h"
#include "Demosaic.h"
#include "Fft.h"
#include "Measure.h"
#include "Select.h"
#include "StackTypes.h"
#include "TestUtil.h"

using namespace planetary;
using ser::FloatImage;
using stack::Complex;
using stack::FrameStats;
using stack::GlobalAligner;
using stack::GlobalShift;
using stack::Shift;

namespace {

// ---- scenes whose sub-pixel shifts are exact -------------------------------------------------------

// A planet's inside, relative to its centre: limb darkening, belts, an oblique band and spots.
double DiscInside(double dx, double dy, double r) {
    const double rr = std::min(1.0, std::sqrt(dx * dx + dy * dy) / r);
    double v = 0.6 * (0.55 + 0.45 * std::sqrt(1 - rr * rr));
    v *= 1 + 0.2 * std::sin(2 * M_PI * dy / 11.0 + 0.4) + 0.1 * std::sin(2 * M_PI * (dx + 0.6 * dy) / 7.0);
    const double spots[3][2] = {{-0.4, 0.2}, {0.3, -0.35}, {0.1, 0.5}};
    for (const auto& s : spots) {
        const double sx = dx - s[0] * r, sy = dy - s[1] * r;
        v += 0.25 * std::exp(-(sx * sx + sy * sy) / (2 * 2.5 * 2.5));
    }
    return v;
}

// The disc scene box-filtered by 8x8 supersampling: the same kernel at any centre, so moving the
// centre by d moves the rendered image by exactly d.
FloatImage RenderDisc(int w, int h, double cx, double cy, double r, double sky) {
    FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, static_cast<float>(sky))};
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 2)));
    const int x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + r + 2)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 2)));
    const int y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + r + 2)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            double acc = 0;
            for (int sy = 0; sy < 8; ++sy)
                for (int sx = 0; sx < 8; ++sx) {
                    const double dx = x + (sx + 0.5) / 8 - 0.5 - cx, dy = y + (sy + 0.5) / 8 - 0.5 - cy;
                    acc += dx * dx + dy * dy < r * r ? DiscInside(dx, dy, r) : sky;
                }
            im.data[static_cast<size_t>(y) * w + x] = static_cast<float>(acc / 64);
        }
    return im;
}

// A frame-filling lunar-like surface: a red spectrum of waves plus craters (bowl and rim), a smooth
// function of position, so evaluating it at (x - dx, y - dy) is an exact shift.
struct Lunar {
    struct Wave { double u, v, a, phi; };
    struct Crater { double x, y, r; };
    std::vector<Wave> waves;
    std::vector<Crater> craters;
    double Eval(double x, double y) const {
        double v = 0.4;
        for (const Wave& w : waves) v += w.a * std::sin(2 * M_PI * (w.u * x + w.v * y) + w.phi);
        for (const Crater& c : craters) {
            const double dx = x - c.x, dy = y - c.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 > 9 * c.r * c.r) continue;
            const double d = std::sqrt(d2), rim = (d - c.r) / (0.3 * c.r);
            v += -0.12 * std::exp(-d2 / (c.r * c.r)) + 0.09 * std::exp(-rim * rim);
        }
        return v;
    }
};

Lunar MakeLunar(unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0, 1);
    Lunar l;
    for (int i = 0; i < 60; ++i) {
        const double f = 0.004 * std::pow(0.12 / 0.004, uni(rng)), th = 2 * M_PI * uni(rng);
        l.waves.push_back({f * std::cos(th), f * std::sin(th), 0.025 * std::sqrt(0.01 / f), 2 * M_PI * uni(rng)});
    }
    for (int i = 0; i < 90; ++i) l.craters.push_back({-150 + 600 * uni(rng), -150 + 500 * uni(rng), 3 + 20 * uni(rng)});
    return l;
}

FloatImage RenderLunar(int w, int h, const Lunar& l, double dx, double dy) {
    FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) im.data[static_cast<size_t>(y) * w + x] = static_cast<float>(l.Eval(x - dx, y - dy));
    return im;
}

FloatImage GaussianBlur(const FloatImage& in, double sigma) {
    if (sigma <= 0) return in;
    const int rad = static_cast<int>(std::ceil(4 * sigma));
    std::vector<double> k(2 * rad + 1);
    double sum = 0;
    for (int i = -rad; i <= rad; ++i) sum += k[i + rad] = std::exp(-0.5 * i * i / (sigma * sigma));
    for (double& v : k) v /= sum;
    FloatImage tmp = in, out = in;
    auto clampi = [](int v, int n) { return std::min(std::max(v, 0), n - 1); };
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            double a = 0;
            for (int i = -rad; i <= rad; ++i) a += k[i + rad] * in.data[static_cast<size_t>(y) * in.width + clampi(x + i, in.width)];
            tmp.data[static_cast<size_t>(y) * in.width + x] = static_cast<float>(a);
        }
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            double a = 0;
            for (int i = -rad; i <= rad; ++i) a += k[i + rad] * tmp.data[static_cast<size_t>(clampi(y + i, in.height)) * in.width + x];
            out.data[static_cast<size_t>(y) * in.width + x] = static_cast<float>(a);
        }
    return out;
}

void AddNoise(FloatImage& im, double sigma, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(0, sigma);
    for (float& v : im.data) v = static_cast<float>(v + n(rng));
}

// ---- naive DFT -----------------------------------------------------------------------------------

std::vector<Complex> NaiveDft(const std::vector<Complex>& x, bool inverse) {
    const size_t n = x.size();
    std::vector<Complex> out(n);
    const double sign = inverse ? 1 : -1;
    for (size_t k = 0; k < n; ++k) {
        Complex s = 0;
        for (size_t j = 0; j < n; ++j)
            s += x[j] * std::polar(1.0, sign * 2 * M_PI * static_cast<double>((j * k) % n) / static_cast<double>(n));
        out[k] = s;
    }
    return out;
}

std::vector<Complex> RandomSignal(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    std::vector<Complex> x(n);
    for (Complex& c : x) c = Complex(g(rng), g(rng));
    return x;
}

double MaxAbsDiff(const std::vector<Complex>& a, const std::vector<Complex>& b) {
    double m = 0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
    return m;
}

constexpr double kTol = 0.1;  // the brief's sub-pixel accuracy, luma px

}  // namespace

// ---- FFT ------------------------------------------------------------------------------------------

TEST_CASE("Fft matches a naive DFT, forward and inverse, for 2-3-5-smooth sizes") {
    for (int n : {1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 25, 27, 30, 32, 60, 64, 100, 120, 240, 256}) {
        INFO("n = " << n);
        const stack::Fft fft(n);
        CHECK(fft.Size() == n);
        const std::vector<Complex> x = RandomSignal(static_cast<size_t>(n), 7u + n);
        std::vector<Complex> fwd(static_cast<size_t>(n)), inv(static_cast<size_t>(n));
        fft.Forward(x.data(), fwd.data());
        fft.Inverse(x.data(), inv.data());
        const double scale = std::sqrt(static_cast<double>(n));
        CHECK(MaxAbsDiff(fwd, NaiveDft(x, false)) <= 1e-11 * scale * n);
        CHECK(MaxAbsDiff(inv, NaiveDft(x, true)) <= 1e-11 * scale * n);
        // Strided input (a column of a row-major image).
        std::vector<Complex> strided(static_cast<size_t>(n) * 3, Complex(99, 99));
        for (int i = 0; i < n; ++i) strided[static_cast<size_t>(i) * 3] = x[static_cast<size_t>(i)];
        std::vector<Complex> fs(static_cast<size_t>(n));
        fft.Forward(strided.data(), fs.data(), 3);
        CHECK(MaxAbsDiff(fs, fwd) == 0.0);
    }
}

TEST_CASE("Fft rejects sizes with a prime factor above 5; fast-size helpers") {
    CHECK_THROWS_AS(stack::Fft(0), std::invalid_argument);
    CHECK_THROWS_AS(stack::Fft(7), std::invalid_argument);
    CHECK_THROWS_AS(stack::Fft(22), std::invalid_argument);
    CHECK(stack::IsFastSize(480));
    CHECK_FALSE(stack::IsFastSize(14));
    CHECK(stack::NextFastSize(7) == 8);
    CHECK(stack::NextFastSize(13) == 15);
    CHECK(stack::NextFastSize(97) == 100);
    CHECK(stack::NextFastSize(1) == 1);
    CHECK(stack::PrevFastSize(511) == 500);
    CHECK(stack::PrevFastSize(242) == 240);
    CHECK(stack::PrevFastSize(1) == 1);
}

TEST_CASE("Fft2D matches a naive 2-D DFT") {
    const int w = 10, h = 6;
    const std::vector<Complex> x = RandomSignal(static_cast<size_t>(w) * h, 3);
    std::vector<Complex> naive(x.size());
    for (int ky = 0; ky < h; ++ky)
        for (int kx = 0; kx < w; ++kx) {
            Complex s = 0;
            for (int y = 0; y < h; ++y)
                for (int xx = 0; xx < w; ++xx)
                    s += x[static_cast<size_t>(y) * w + xx] *
                         std::polar(1.0, -2 * M_PI * (static_cast<double>(kx * xx) / w + static_cast<double>(ky * y) / h));
            naive[static_cast<size_t>(ky) * w + kx] = s;
        }
    const stack::Fft2D fft(w, h);
    std::vector<Complex> d = x;
    fft.Forward(d);
    CHECK(MaxAbsDiff(d, naive) <= 1e-10);
    fft.Inverse(d);
    for (Complex& c : d) c /= static_cast<double>(w * h);
    CHECK(MaxAbsDiff(d, x) <= 1e-12);
}

// ---- global shift: disc ---------------------------------------------------------------------------

TEST_CASE("GlobalShift recovers integer shifts of a textured disc") {
    const int w = 240, h = 180;
    const FloatImage ref = GaussianBlur(RenderDisc(w, h, 110.3, 88.6, 34, 0.03), 1.0);
    const GlobalAligner aligner(ref, false);
    for (auto d : std::vector<std::pair<int, int>>{{0, 0}, {7, -4}, {-12, 9}, {1, 0}, {0, -1}, {25, 18}}) {
        const FloatImage frame = GaussianBlur(RenderDisc(w, h, 110.3 + d.first, 88.6 + d.second, 34, 0.03), 1.0);
        const FrameStats st = stack::MeasureFrame(frame, 0);
        REQUIRE(st.targetFound);
        const Shift s = aligner.Align(frame, st);
        INFO("true shift " << d.first << ", " << d.second << "; got " << s.dx << ", " << s.dy);
        CHECK(std::fabs(s.dx - d.first) <= 0.02);
        CHECK(std::fabs(s.dy - d.second) <= 0.02);
        CHECK(s.confidence >= 0.99);
        CHECK(s.confidence <= 1.0);
    }
}

TEST_CASE("GlobalShift recovers sub-pixel shifts of a textured disc within 0.1 px") {
    const int w = 240, h = 180;
    const double cx = 104.0, cy = 91.0;
    const FloatImage ref = GaussianBlur(RenderDisc(w, h, cx, cy, 34, 0.03), 1.0);
    const GlobalAligner aligner(ref, false);
    double worst = 0;
    for (double dx : {-3.7, -0.5, 0.25, 0.5, 1.3, 2.9})
        for (double dy : {-2.45, -0.1, 0.5, 0.75, 4.6}) {
            const FloatImage frame = GaussianBlur(RenderDisc(w, h, cx + dx, cy + dy, 34, 0.03), 1.0);
            const Shift s = aligner.Align(frame, stack::MeasureFrame(frame, 0));
            INFO("true " << dx << ", " << dy << "; got " << s.dx << ", " << s.dy);
            CHECK(std::fabs(s.dx - dx) <= 0.03);  // brief: 0.1; measured 2026-10-21: <= 0.0075
            CHECK(std::fabs(s.dy - dy) <= 0.03);
            CHECK(s.confidence >= 0.99);
            worst = std::max({worst, std::fabs(s.dx - dx), std::fabs(s.dy - dy)});
        }
    WARN("disc sub-pixel: worst error " << worst << " px");
}

TEST_CASE("GlobalShift follows a disc drifting far across the frame") {
    const int w = 320, h = 240;
    const FloatImage ref = GaussianBlur(RenderDisc(w, h, 80.2, 70.4, 30, 0.02), 1.0);
    const GlobalAligner aligner(ref, false);
    for (auto d : std::vector<std::pair<double, double>>{{150.35, 95.8}, {-30.6, 120.25}, {170.0, 0.4}}) {
        const FloatImage frame = GaussianBlur(RenderDisc(w, h, 80.2 + d.first, 70.4 + d.second, 30, 0.02), 1.0);
        const Shift s = aligner.Align(frame, stack::MeasureFrame(frame, 0));
        INFO("true " << d.first << ", " << d.second << "; got " << s.dx << ", " << s.dy);
        CHECK(std::fabs(s.dx - d.first) <= kTol);
        CHECK(std::fabs(s.dy - d.second) <= kTol);
        CHECK(s.confidence >= 0.99);
    }
}

TEST_CASE("GlobalShift on a disc is robust to noise and to a different blur (seeing)") {
    const int w = 240, h = 180;
    const double cx = 118.4, cy = 87.7;
    FloatImage ref = GaussianBlur(RenderDisc(w, h, cx, cy, 34, 0.03), 1.0);
    AddNoise(ref, 0.005, 1);  // a reference averaged from a few frames: low noise
    const GlobalAligner aligner(ref, false);
    for (double noise : {0.02, 0.05}) {
        double sum2 = 0, worst = 0;
        int n = 0;
        for (unsigned seed = 0; seed < 12; ++seed) {
            const double dx = -4.0 + 0.71 * seed, dy = 3.0 - 0.53 * seed;
            const double blur = 1.0 + 0.15 * (seed % 5);  // 1.0 .. 1.6 px
            FloatImage frame = GaussianBlur(RenderDisc(w, h, cx + dx, cy + dy, 34, 0.03), blur);
            AddNoise(frame, noise, 100 + seed);
            const Shift s = aligner.Align(frame, stack::MeasureFrame(frame, seed));
            const double ex = s.dx - dx, ey = s.dy - dy;
            sum2 += ex * ex + ey * ey;
            worst = std::max({worst, std::fabs(ex), std::fabs(ey)});
            ++n;
            INFO("noise " << noise << " seed " << seed << ": error " << ex << ", " << ey << " conf " << s.confidence);
            CHECK(s.confidence >= stack::kMinAlignConfidence);
        }
        const double rms = std::sqrt(sum2 / (2 * n));
        INFO("noise " << noise << ": rms " << rms << " worst " << worst);
        CHECK(worst <= kTol);
        WARN("disc, noise " << noise << " (signal ~0.6), blur 1.0-1.6 vs 1.0: rms error " << rms << ", worst " << worst);
    }
}

// ---- global shift: frame-filling surface ------------------------------------------------------------

TEST_CASE("GlobalShift recovers integer and sub-pixel shifts of a frame-filling lunar-like surface") {
    const int w = 256, h = 192;
    const Lunar moon = MakeLunar(11);
    const FloatImage ref = GaussianBlur(RenderLunar(w, h, moon, 0, 0), 0.8);
    const GlobalAligner aligner(ref, true);
    double worst = 0;
    for (auto d : std::vector<std::pair<double, double>>{
             {0, 0}, {4, -6}, {0.3, -0.7}, {5.5, 3.25}, {-17.4, 11.6}, {-0.5, 0.5}, {31.2, -22.9}}) {
        const FloatImage frame = GaussianBlur(RenderLunar(w, h, moon, d.first, d.second), 0.8);
        const Shift s = aligner.Align(frame, stack::MeasureFrame(frame, 0));
        INFO("true " << d.first << ", " << d.second << "; got " << s.dx << ", " << s.dy << " conf " << s.confidence);
        CHECK(std::fabs(s.dx - d.first) <= 0.03);  // brief: 0.1; measured 2026-10-21: <= 0.015
        CHECK(std::fabs(s.dy - d.second) <= 0.03);
        CHECK(s.confidence >= 0.95);
        worst = std::max({worst, std::fabs(s.dx - d.first), std::fabs(s.dy - d.second)});
    }
    WARN("lunar: worst error " << worst << " px");
}

TEST_CASE("GlobalShift on a frame-filling surface is robust to noise and blur") {
    const int w = 256, h = 192;
    const Lunar moon = MakeLunar(5);
    FloatImage ref = GaussianBlur(RenderLunar(w, h, moon, 0, 0), 0.8);
    AddNoise(ref, 0.004, 2);
    const GlobalAligner aligner(ref, true);
    double worst = 0;
    for (unsigned seed = 0; seed < 10; ++seed) {
        const double dx = -6.3 + 1.37 * seed, dy = 4.1 - 0.91 * seed;
        FloatImage frame = GaussianBlur(RenderLunar(w, h, moon, dx, dy), 0.8 + 0.2 * (seed % 4));
        AddNoise(frame, 0.02, 300 + seed);
        const Shift s = aligner.Align(frame, stack::MeasureFrame(frame, seed));
        INFO("seed " << seed << ": error " << s.dx - dx << ", " << s.dy - dy << " conf " << s.confidence);
        CHECK(std::fabs(s.dx - dx) <= kTol);
        CHECK(std::fabs(s.dy - dy) <= kTol);
        CHECK(s.confidence >= stack::kMinAlignConfidence);
        worst = std::max({worst, std::fabs(s.dx - dx), std::fabs(s.dy - dy)});
    }
    WARN("lunar, noise 0.02, blur 0.8-1.4: worst error " << worst << " px");
}

// ---- confidence -------------------------------------------------------------------------------------

TEST_CASE("a frame of a different scene gets a low confidence") {
    const int w = 256, h = 192;
    const FloatImage disc = GaussianBlur(RenderDisc(w, h, 120, 95, 34, 0.03), 1.0);
    const FloatImage moonA = GaussianBlur(RenderLunar(w, h, MakeLunar(21), 0, 0), 0.8);
    const FloatImage moonB = GaussianBlur(RenderLunar(w, h, MakeLunar(22), 0, 0), 0.8);
    FloatImage noise{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.05f)};
    AddNoise(noise, 0.02, 9);

    const GlobalAligner discAligner(disc, false), moonAligner(moonA, true);
    std::vector<std::pair<std::string, Shift>> cases = {
        {"lunar frame vs disc reference", discAligner.Align(moonB, stack::MeasureFrame(moonB, 0))},
        {"noise frame vs disc reference", discAligner.Align(noise, stack::MeasureFrame(noise, 0))},
        {"another lunar field vs lunar reference", moonAligner.Align(moonB, stack::MeasureFrame(moonB, 0))},
        {"disc frame vs lunar reference", moonAligner.Align(disc, stack::MeasureFrame(disc, 0))},
        {"noise frame vs lunar reference", moonAligner.Align(noise, stack::MeasureFrame(noise, 0))},
    };
    for (const auto& c : cases) {
        INFO(c.first << ": confidence " << c.second.confidence);
        CHECK(c.second.confidence >= 0.0);
        CHECK(c.second.confidence < stack::kMinAlignConfidence);
    }
    // A flat frame has nothing to align on.
    const FloatImage flat{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.2f)};
    CHECK(moonAligner.Align(flat, stack::MeasureFrame(flat, 0)).confidence == 0.0);
}

TEST_CASE("a torn frame (part of it displaced) scores below the cut; a few pixels of wobble do not") {
    const int w = 256, h = 192;
    const Lunar moon = MakeLunar(17);
    const FloatImage ref = GaussianBlur(RenderLunar(w, h, moon, 0, 0), 0.8);
    const GlobalAligner aligner(ref, true);
    // Rows from `tearRow` down show the scene displaced by `jump` px more than the rows above.
    auto torn = [&](int tearRow, double jump, unsigned seed) {
        const FloatImage top = RenderLunar(w, h, moon, 2.3, -1.6), bottom = RenderLunar(w, h, moon, 2.3 + jump, -1.6);
        FloatImage f = top;
        std::copy(bottom.data.begin() + static_cast<size_t>(tearRow) * w, bottom.data.end(),
                  f.data.begin() + static_cast<size_t>(tearRow) * w);
        f = GaussianBlur(f, 0.8);
        AddNoise(f, 0.01, seed);
        return f;
    };
    std::vector<Shift> capture;
    for (unsigned i = 0; i < 20; ++i) {
        const FloatImage f = torn(h, 0, 40 + i);  // intact
        capture.push_back(aligner.Align(f, stack::MeasureFrame(f, i)));
    }
    // Torn through the middle, and with the larger part (the lower 60%) displaced: either way the
    // match is cut by about the torn part's share. A 1 px wobble is local alignment's to take out.
    // (Measured 2026-10-21: a half-frame tear of only 4 px still scores 0.89 of an intact frame --
    // after the 1.5 px low pass the large-scale structure correlates across 4 px -- so moderate tears
    // are left to the region stage, like the real 1.2-2.5 px wobble frames.)
    const FloatImage half = torn(h / 2, 8, 1), most = torn(h * 2 / 5, 8, 2), wobble = torn(h / 2, 1.0, 3);
    const Shift sHalf = aligner.Align(half, stack::MeasureFrame(half, 0));
    const Shift sMost = aligner.Align(most, stack::MeasureFrame(most, 0));
    const Shift sWobble = aligner.Align(wobble, stack::MeasureFrame(wobble, 0));
    capture.push_back(sHalf);
    capture.push_back(sMost);
    capture.push_back(sWobble);
    const double cut = stack::AlignmentCut(capture);
    INFO("cut " << cut << "; half torn " << sHalf.confidence << ", 60% torn " << sMost.confidence << ", 1 px wobble "
                << sWobble.confidence << ", intact " << capture[0].confidence);
    CHECK(sHalf.confidence < cut);
    CHECK(sMost.confidence < cut);
    CHECK(sWobble.confidence >= cut);
    for (int i = 0; i < 20; ++i) CHECK(capture[static_cast<size_t>(i)].confidence >= cut);
}

TEST_CASE("AlignmentCut: relative to the capture's median, never below the absolute floor") {
    auto shifts = [](std::vector<double> confs) {
        std::vector<Shift> s;
        for (double c : confs) s.push_back(Shift{0, 0, c});
        return s;
    };
    using stack::AlignmentCut;
    using stack::kAlignRelative;
    using stack::kMinAlignConfidence;
    CHECK(AlignmentCut({}) == kMinAlignConfidence);
    CHECK(AlignmentCut(shifts({0.1, 0.2, 0.3})) == kMinAlignConfidence);
    // A steady capture with a few torn frames and two foreign ones: the cut follows the bulk.
    std::vector<double> c;
    for (int i = 0; i < 100; ++i) c.push_back(0.94 + 0.0004 * i);
    for (double t : {0.6, 0.7, 0.78, 0.1, 0.15}) c.push_back(t);
    const double cut = AlignmentCut(shifts(c));
    CHECK(cut > 0.78);
    CHECK(cut < 0.94);
    CHECK(cut == Catch::Approx(kAlignRelative * 0.9596).epsilon(1e-3));
    // A poor night: every frame matches less well, and they all stay.
    std::vector<double> poor;
    for (int i = 0; i < 50; ++i) poor.push_back(0.78 + 0.002 * i);
    CHECK(AlignmentCut(shifts(poor)) <= 0.78);
    // Order does not matter.
    std::vector<double> rev(c.rbegin(), c.rend());
    CHECK(AlignmentCut(shifts(rev)) == cut);
}

TEST_CASE("GlobalShift (one call) equals GlobalAligner, and Align is deterministic across threads") {
    const int w = 240, h = 180;
    const FloatImage ref = GaussianBlur(RenderDisc(w, h, 100, 90, 34, 0.03), 1.0);
    std::vector<FloatImage> frames;
    std::vector<FrameStats> stats;
    for (int i = 0; i < 16; ++i) {
        FloatImage f = GaussianBlur(RenderDisc(w, h, 100 + 0.37 * i, 90 - 0.21 * i, 34, 0.03), 1.0);
        AddNoise(f, 0.01, 50 + i);
        stats.push_back(stack::MeasureFrame(f, i));
        frames.push_back(std::move(f));
    }
    const GlobalAligner aligner(ref, false);
    std::vector<Shift> serial;
    for (int i = 0; i < 16; ++i) serial.push_back(aligner.Align(frames[i], stats[i]));
    for (int i = 0; i < 16; ++i) {
        const Shift one = GlobalShift(ref, frames[i], stats[i]);
        CHECK(one.dx == serial[i].dx);
        CHECK(one.dy == serial[i].dy);
        CHECK(one.confidence == serial[i].confidence);
    }
    std::vector<Shift> parallel(16);
    std::vector<std::thread> pool;
    for (int t = 0; t < 8; ++t)
        pool.emplace_back([&, t] {
            for (int i = t; i < 16; i += 8) parallel[i] = aligner.Align(frames[i], stats[i]);
        });
    for (std::thread& t : pool) t.join();
    for (int i = 0; i < 16; ++i) {
        CHECK(parallel[i].dx == serial[i].dx);
        CHECK(parallel[i].dy == serial[i].dy);
        CHECK(parallel[i].confidence == serial[i].confidence);
    }
}

TEST_CASE("GlobalShift's one-call form registers a frame-filling reference as a whole frame") {
    const int w = 256, h = 192;
    const Lunar moon = MakeLunar(31);
    const FloatImage ref = GaussianBlur(RenderLunar(w, h, moon, 0, 0), 0.8);
    const FloatImage frame = GaussianBlur(RenderLunar(w, h, moon, -9.35, 6.6), 0.8);
    const Shift s = GlobalShift(ref, frame, stack::MeasureFrame(frame, 0));
    const Shift a = GlobalAligner(ref, true).Align(frame, stack::MeasureFrame(frame, 0));
    CHECK(std::fabs(s.dx + 9.35) <= kTol);
    CHECK(std::fabs(s.dy - 6.6) <= kTol);
    CHECK(s.dx == a.dx);
    CHECK(s.dy == a.dy);
}

TEST_CASE("GlobalAligner rejects malformed input") {
    const FloatImage ref = GaussianBlur(RenderDisc(120, 100, 60, 50, 20, 0.03), 1.0);
    CHECK_THROWS_AS(GlobalAligner(FloatImage{}, false), std::invalid_argument);
    CHECK_THROWS_AS(GlobalAligner(FloatImage{4, 4, 3, std::vector<float>(48, 0.f)}, true), std::invalid_argument);
    CHECK_THROWS_AS(GlobalAligner(FloatImage{4, 4, 1, std::vector<float>(15, 0.f)}, true), std::invalid_argument);
    // A disc reference must have a target.
    CHECK_THROWS_AS(GlobalAligner(FloatImage{64, 64, 1, std::vector<float>(64 * 64, 0.1f)}, false), std::invalid_argument);
    const GlobalAligner aligner(ref, false);
    const FloatImage other = RenderDisc(100, 100, 50, 50, 20, 0.03);
    CHECK_THROWS_AS(aligner.Align(other, stack::MeasureFrame(other, 0)), std::invalid_argument);
}

// ---- BuildReference ---------------------------------------------------------------------------------

namespace {

constexpr int kRefW = 200, kRefH = 160, kRefFrames = 40;

struct Truth { double cx, cy; };

Truth RefTruth(int f) { return {70 + 0.9 * f + 1.3 * std::sin(0.7 * f), 70 + 0.37 * f + 0.8 * std::cos(1.1 * f)}; }

const std::string& DriftingDiscAvi() {
    static testing::TempDir dir;
    static const std::string path = [] {
        const std::string p = dir.NewPath(".avi");
        std::vector<std::vector<uint8_t>> frames;
        for (int f = 0; f < kRefFrames; ++f) {
            const Truth t = RefTruth(f);
            FloatImage im = GaussianBlur(RenderDisc(kRefW, kRefH, t.cx, t.cy, 30, 0.04), 0.9 + 0.15 * ((f * 7) % 5));
            AddNoise(im, 0.01, 700u + f);
            std::vector<uint8_t> bytes(im.data.size());
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<uint8_t>(std::clamp(std::lround(im.data[i] * 255.0), 0L, 255L));
            frames.push_back(std::move(bytes));
        }
        testing::AviSpec spec;
        spec.width = kRefW;
        spec.height = kRefH;
        testing::WriteAvi(p, spec, frames);
        return p;
    }();
    return path;
}

double PatchStd(const FloatImage& im, int x0, int y0, int n) {
    double s = 0, s2 = 0;
    for (int y = y0; y < y0 + n; ++y)
        for (int x = x0; x < x0 + n; ++x) {
            const double v = im.data[static_cast<size_t>(y) * im.width + x];
            s += v;
            s2 += v * v;
        }
    const double m = s / (n * n);
    return std::sqrt(std::max(0.0, s2 / (n * n) - m * m));
}

}  // namespace

TEST_CASE("BuildReference: a sharp, low-noise luma reference centred on the best frames' mean position") {
    const std::string& path = DriftingDiscAvi();
    for (ser::BayerChoice choice : {ser::BayerChoice::None, ser::BayerChoice::RGGB}) {
        INFO("bayer choice " << static_cast<int>(choice));
        auto probe = ser::OpenVideo(path, choice);
        const ser::FileInfo info = probe->Info();
        const double scale = stack::LumaScale(info);
        const stack::SourceFactory factory = [&] { return ser::OpenVideo(path, choice); };
        const std::vector<FrameStats> stats = stack::MeasureAll(factory, info, 4, {});
        const stack::Selection sel = stack::SelectFrames(stats, info);
        REQUIRE(sel.diagnosis.kind == stack::DiagnosisKind::None);
        REQUIRE_FALSE(sel.fullFrameTarget);
        REQUIRE(sel.frames.size() >= static_cast<size_t>(stack::kReferenceFrames));

        std::vector<std::string> phases;
        std::vector<int64_t> dones, totals;
        stack::Progress progress;
        progress.report = [&](const std::string& p, int64_t d, int64_t t) {
            phases.push_back(p);
            dones.push_back(d);
            totals.push_back(t);
        };
        const FloatImage ref = stack::BuildReference(factory, info, sel, stats, progress);
        REQUIRE(ref.channels == 1);
        REQUIRE(ref.width == static_cast<int>(kRefW / scale));
        REQUIRE(ref.height == static_cast<int>(kRefH / scale));
        REQUIRE_FALSE(phases.empty());
        for (const std::string& p : phases) CHECK(p == "Building reference");
        CHECK(std::is_sorted(dones.begin(), dones.end()));
        CHECK(dones.back() == totals.back());

        const FrameStats rs = stack::MeasureFrame(ref, 0);
        REQUIRE(rs.targetFound);

        // Which frames the reference is built from: the kReferenceFrames sharpest selected frames.
        std::vector<int64_t> ranked = sel.frames;
        std::sort(ranked.begin(), ranked.end(), [&](int64_t a, int64_t b) {
            return stats[a].quality != stats[b].quality ? stats[a].quality > stats[b].quality : a < b;
        });
        ranked.resize(static_cast<size_t>(stack::kReferenceFrames));
        double mx = 0, my = 0;
        double bestQ = 0;
        for (int64_t i : ranked) {
            mx += RefTruth(static_cast<int>(i)).cx / scale;
            my += RefTruth(static_cast<int>(i)).cy / scale;
            bestQ += stats[i].quality;
        }
        mx /= static_cast<double>(ranked.size());
        my /= static_cast<double>(ranked.size());
        bestQ /= static_cast<double>(ranked.size());

        // Every selected frame aligns to it; shift = the frame's true position - the reference's,
        // and the reference sits at the mean true position of the frames it was built from.
        const GlobalAligner aligner(ref, false);
        double ox = 0, oy = 0;
        std::vector<std::pair<double, double>> offsets;
        for (int64_t i : sel.frames) {
            const FloatImage luma = stack::LumaForMeasure(probe->ReadFrame(i), info.colorId, info.normDepth);
            const Shift s = aligner.Align(luma, stats[i]);
            CHECK(s.confidence >= stack::kMinAlignConfidence);
            const Truth t = RefTruth(static_cast<int>(i));
            offsets.emplace_back(t.cx / scale - s.dx, t.cy / scale - s.dy);  // = reference position
            ox += offsets.back().first;
            oy += offsets.back().second;
        }
        ox /= static_cast<double>(offsets.size());
        oy /= static_cast<double>(offsets.size());
        double spread = 0;
        for (const auto& o : offsets) spread = std::max({spread, std::fabs(o.first - ox), std::fabs(o.second - oy)});
        INFO("reference at " << ox << ", " << oy << "; best frames' mean " << mx << ", " << my << "; spread " << spread);
        CHECK(spread <= kTol * 1.5);  // 8-bit frames with noise: each frame's own error enters
        CHECK(std::fabs(ox - mx) <= kTol);
        CHECK(std::fabs(oy - my) <= kTol);

        // Lower noise than a frame (sky patch away from the disc), and no blur from misregistration.
        const FloatImage frame0 = stack::LumaForMeasure(probe->ReadFrame(ranked[0]), info.colorId, info.normDepth);
        const int n = static_cast<int>(24 / scale);
        CHECK(PatchStd(ref, 2, ref.height - n - 2, n) < 0.5 * PatchStd(frame0, 2, ref.height - n - 2, n));
        INFO("reference quality " << rs.quality << " vs best frames' mean " << bestQ);
        CHECK(rs.quality >= 0.95 * bestQ);
    }
}

TEST_CASE("BuildReference on a frame-filling surface") {
    static testing::TempDir dir;
    const std::string path = dir.NewPath(".avi");
    const int w = 192, h = 144, n = 12;
    const Lunar moon = MakeLunar(41);
    std::vector<std::pair<double, double>> truth;
    {
        std::vector<std::vector<uint8_t>> frames;
        for (int f = 0; f < n; ++f) {
            truth.emplace_back(1.7 * f - 9 + 0.6 * std::sin(f), -1.1 * f + 4 + 0.5 * std::cos(2 * f));
            FloatImage im = GaussianBlur(RenderLunar(w, h, moon, truth.back().first, truth.back().second), 0.8);
            AddNoise(im, 0.008, 900u + f);
            std::vector<uint8_t> bytes(im.data.size());
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<uint8_t>(std::clamp(std::lround(im.data[i] * 255.0), 0L, 255L));
            frames.push_back(std::move(bytes));
        }
        testing::AviSpec spec;
        spec.width = w;
        spec.height = h;
        testing::WriteAvi(path, spec, frames);
    }
    auto probe = ser::OpenVideo(path, ser::BayerChoice::None);
    const ser::FileInfo info = probe->Info();
    const stack::SourceFactory factory = [&] { return ser::OpenVideo(path, ser::BayerChoice::None); };
    const std::vector<FrameStats> stats = stack::MeasureAll(factory, info, 2, {});
    stack::Selection sel;  // a full-frame selection of every frame (selection itself is Task 3's)
    sel.fullFrameTarget = true;
    for (int f = 0; f < n; ++f) sel.frames.push_back(f);
    const FloatImage ref = stack::BuildReference(factory, info, sel, stats, {});
    REQUIRE(ref.width == w);
    REQUIRE(ref.height == h);

    const GlobalAligner aligner(ref, true);
    std::vector<double> ox, oy;
    for (int f = 0; f < n; ++f) {
        const FloatImage luma = stack::LumaForMeasure(probe->ReadFrame(f), info.colorId, info.normDepth);
        const Shift s = aligner.Align(luma, stats[f]);
        CHECK(s.confidence >= stack::kMinAlignConfidence);
        ox.push_back(truth[f].first - s.dx);
        oy.push_back(truth[f].second - s.dy);
    }
    const auto [xmin, xmax] = std::minmax_element(ox.begin(), ox.end());
    const auto [ymin, ymax] = std::minmax_element(oy.begin(), oy.end());
    INFO("reference position spread x " << *xmax - *xmin << " y " << *ymax - *ymin);
    CHECK(*xmax - *xmin <= 2 * kTol);
    CHECK(*ymax - *ymin <= 2 * kTol);
}

TEST_CASE("BuildReference: cancel, diagnosed selections and missing stats") {
    const std::string& path = DriftingDiscAvi();
    auto probe = ser::OpenVideo(path, ser::BayerChoice::None);
    const ser::FileInfo info = probe->Info();
    const stack::SourceFactory factory = [&] { return ser::OpenVideo(path, ser::BayerChoice::None); };
    const std::vector<FrameStats> stats = stack::MeasureAll(factory, info, 4, {});
    const stack::Selection sel = stack::SelectFrames(stats, info);
    REQUIRE(sel.diagnosis.kind == stack::DiagnosisKind::None);

    int polls = 0;
    stack::Progress cancelLater;
    cancelLater.cancelled = [&] { return ++polls > 3; };
    CHECK_THROWS_AS(stack::BuildReference(factory, info, sel, stats, cancelLater), stack::StackCancelled);

    stack::Selection none;
    none.diagnosis.kind = stack::DiagnosisKind::NoTarget;
    CHECK_THROWS_AS(stack::BuildReference(factory, info, none, stats, {}), std::invalid_argument);

    std::vector<FrameStats> partial(stats.begin(), stats.begin() + 5);
    CHECK_THROWS_AS(stack::BuildReference(factory, info, sel, partial, {}), std::invalid_argument);

    const stack::SourceFactory noReader = []() -> std::unique_ptr<ser::FrameSource> { return nullptr; };
    CHECK_THROWS(stack::BuildReference(noReader, info, sel, stats, {}));
}

// ---- review round 1 ---------------------------------------------------------------------------------

namespace {

// A square of lunar-like terrain on a dark sky: a target, but not the planet.
FloatImage ForeignScene(int w, int h) {
    const Lunar l = MakeLunar(77);
    FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h, 0.04f)};
    for (int y = h / 2 - 50; y < h / 2 + 50; ++y)
        for (int x = w / 2 - 60; x < w / 2 + 60; ++x) im.data[static_cast<size_t>(y) * w + x] = static_cast<float>(l.Eval(x, y));
    return GaussianBlur(im, 0.9);
}

std::vector<uint8_t> To8(const FloatImage& im) {
    std::vector<uint8_t> b(im.data.size());
    for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(std::clamp(std::lround(im.data[i] * 255.0), 0L, 255L));
    return b;
}

}  // namespace

TEST_CASE("BuildReference: a different-scene frame at the top quality rank does not become the reference") {
    static testing::TempDir dir;
    const std::string path = dir.NewPath(".avi");
    constexpr int kForeign = 7, n = 30;
    {
        std::vector<std::vector<uint8_t>> frames;
        for (int f = 0; f < n; ++f) {
            FloatImage im = f == kForeign ? ForeignScene(kRefW, kRefH)
                                          : GaussianBlur(RenderDisc(kRefW, kRefH, RefTruth(f).cx, RefTruth(f).cy, 30, 0.04), 1.0);
            AddNoise(im, 0.01, 1300u + f);
            frames.push_back(To8(im));
        }
        testing::AviSpec spec;
        spec.width = kRefW;
        spec.height = kRefH;
        testing::WriteAvi(path, spec, frames);
    }
    auto probe = ser::OpenVideo(path, ser::BayerChoice::None);
    const ser::FileInfo info = probe->Info();
    const stack::SourceFactory factory = [&] { return ser::OpenVideo(path, ser::BayerChoice::None); };
    std::vector<FrameStats> stats = stack::MeasureAll(factory, info, 4, {});
    REQUIRE(stats[kForeign].targetFound);
    double qmax = 0;
    for (const FrameStats& s : stats) qmax = std::max(qmax, s.quality);
    stats[kForeign].quality = 10 * qmax;  // the sharpest frame by far
    stack::Selection sel;
    for (int f = 0; f < n; ++f) sel.frames.push_back(f);

    std::vector<int64_t> dones, totals;
    stack::Progress progress;
    progress.report = [&](const std::string&, int64_t d, int64_t t) {
        dones.push_back(d);
        totals.push_back(t);
    };
    const FloatImage ref = stack::BuildReference(factory, info, sel, stats, progress);
    CHECK(std::is_sorted(dones.begin(), dones.end()));
    CHECK(dones.back() == totals.back());
    CHECK(totals.back() == 4 * stack::kReferenceFrames);  // one anchor turned down
    const FrameStats rs = stack::MeasureFrame(ref, 0);
    REQUIRE(rs.targetFound);
    const FrameStats& disc = stats[0];
    INFO("reference box " << rs.bx1 - rs.bx0 << "x" << rs.by1 - rs.by0 << ", a disc frame's " << disc.bx1 - disc.bx0
                          << "x" << disc.by1 - disc.by0);
    CHECK(std::abs((rs.bx1 - rs.bx0) - (disc.bx1 - disc.bx0)) <= 8);
    CHECK(std::abs((rs.by1 - rs.by0) - (disc.by1 - disc.by0)) <= 8);
    const GlobalAligner aligner(ref, false);
    double ox0 = 0, oy0 = 0;
    bool first = true;
    for (int f = 0; f < n; ++f) {
        if (f == kForeign) continue;
        const Shift s = aligner.Align(stack::LumaForMeasure(probe->ReadFrame(f), info.colorId, info.normDepth), stats[f]);
        INFO("frame " << f << " confidence " << s.confidence);
        CHECK(s.confidence >= 0.9);
        const double ox = RefTruth(f).cx - s.dx, oy = RefTruth(f).cy - s.dy;
        if (first) {
            ox0 = ox;
            oy0 = oy;
            first = false;
        }
        CHECK(std::fabs(ox - ox0) <= 0.2);
        CHECK(std::fabs(oy - oy0) <= 0.2);
    }
}

TEST_CASE("blurred frames across the range selection keeps (1-3x the best blur) all pass the cut") {
    struct Scene { const char* name; bool full; double best; };
    for (const Scene& sc : {Scene{"disc", false, 1.0}, Scene{"disc", false, 2.0}, Scene{"lunar", true, 1.0},
                            Scene{"lunar", true, 2.0}}) {
        const int w = 256, h = 192;
        const Lunar moon = MakeLunar(91);
        auto render = [&](double dx, double dy, double blur) {
            return GaussianBlur(sc.full ? RenderLunar(w, h, moon, dx, dy) : RenderDisc(w, h, 120 + dx, 95 + dy, 40, 0.03), blur);
        };
        FloatImage ref = render(0, 0, sc.best);
        AddNoise(ref, 0.003, 5);
        const GlobalAligner aligner(ref, sc.full);
        // Evenly spread over 1x .. 3x, and skewed: most frames near the best, a tail at 2.5-3x (the
        // median is then a sharp frame's, the hardest case for a relative cut).
        for (bool skewed : {false, true}) {
            std::vector<Shift> shifts;
            std::vector<double> blurs;
            for (int i = 0; i < 16; ++i) {
                const double blur = skewed ? sc.best * (i < 12 ? 1 + 0.2 * i / 11 : 2.5 + 0.5 * (i - 12) / 3)
                                           : sc.best * (1 + 2.0 * i / 15);
                FloatImage f = render(0.37 * i - 4, 2.9 - 0.21 * i, blur);
                AddNoise(f, 0.01, 2000u + i);
                shifts.push_back(aligner.Align(f, stack::MeasureFrame(f, i)));
                blurs.push_back(blur);
            }
            const double cut = stack::AlignmentCut(shifts);
            for (size_t i = 0; i < shifts.size(); ++i) {
                INFO(sc.name << (skewed ? " skewed" : " even") << ", best blur " << sc.best << ": frame blur " << blurs[i]
                             << " confidence " << shifts[i].confidence << " cut " << cut);
                CHECK(shifts[i].confidence >= cut);
                CHECK(shifts[i].confidence >= 0.95);  // blur alone costs (almost) nothing
            }
        }
    }
}

TEST_CASE("a frame-filling crescent wider than the window: the window sits on the lit part") {
    const int w = 1024, h = 384;
    const Lunar moon = MakeLunar(55);
    // Lit surface left of a curved terminator (x < 300 + 60 sin), dark sky to the right.
    auto render = [&](double dx, double dy) {
        FloatImage im{w, h, 1, std::vector<float>(static_cast<size_t>(w) * h)};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double sx = x - dx, sy = y - dy;
                const double edge = 300 + 60 * std::sin(sy / 120.0);
                const double lit = 1 / (1 + std::exp((sx - edge) / 2.0));
                im.data[static_cast<size_t>(y) * w + x] = static_cast<float>(0.02 + lit * moon.Eval(sx, sy));
            }
        return GaussianBlur(im, 0.8);
    };
    const FloatImage ref = render(0, 0);
    const GlobalAligner aligner(ref, true);
    INFO("window centre " << aligner.WindowCentreX() << ", " << aligner.WindowCentreY() << " size " << aligner.WindowWidth());
    CHECK(aligner.WindowCentreX() - aligner.WindowWidth() / 2 <= 40);  // the window starts at the lit left side
    for (auto d : std::vector<std::pair<double, double>>{{3.4, -2.2}, {-11.7, 6.25}, {25.5, 9.1}}) {
        FloatImage f = render(d.first, d.second);
        AddNoise(f, 0.01, 77);
        const Shift s = aligner.Align(f, stack::MeasureFrame(f, 0));
        INFO("true " << d.first << ", " << d.second << " got " << s.dx << ", " << s.dy << " conf " << s.confidence);
        CHECK(std::fabs(s.dx - d.first) <= 0.05);
        CHECK(std::fabs(s.dy - d.second) <= 0.05);
        CHECK(s.confidence >= 0.95);
    }
}
