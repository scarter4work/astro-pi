// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case; it reads a real capture (the NAS is only read). Built as calib-inject.
//
//   calib-inject <video> <dn>...; env PASS1: pass 1 alone
//
// Ruling P26 fix round 1 (I5): a texture-independent check of a capture's noise model. Gaussian noise of sd
// `dn` DN is added to every raw sample (seeded by the frame index, so both passes read the same frames),
// rounded to whole DN; the fitted curve must rise by v0 = (dn^2 + 1/12) / (maxValue^2 sites) at every level
// (sites: samples averaged per luma pixel; the 1/12 DN^2 is the rounding's).
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <random>
#include <vector>
#include "FrameSource.h"
#include "Measure.h"
#include "Noise.h"
using namespace planetary;
namespace {
class Injected : public ser::FrameSource {
public:
    Injected(std::unique_ptr<ser::FrameSource> s, double dn) : s_(std::move(s)), info_(s_->Info()), dn_(dn) {}
    const ser::FileInfo& Info() const override { return info_; }
    ser::Frame ReadFrame(int64_t i) const override {
        ser::Frame f = s_->ReadFrame(i);
        if (dn_ > 0) {
            std::mt19937_64 rng(0x9e3779b97f4a7c15ull ^ static_cast<uint64_t>(i));
            std::normal_distribution<double> n(0, dn_);
            const double mx = std::ldexp(1.0, info_.normDepth) - 1;
            for (auto& v : f.samples) v = static_cast<uint16_t>(std::clamp(std::lround(v + n(rng)), 0L, static_cast<long>(mx)));
        }
        return f;
    }
    std::optional<int64_t> FrameTimestamp(int64_t i) const override { return s_->FrameTimestamp(i); }

private:
    std::unique_ptr<ser::FrameSource> s_;
    ser::FileInfo info_;
    double dn_;
};
}  // namespace
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: calib-inject <video> <dn>...; env PASS1: pass 1 alone\n");
        return 1;
    }
    const std::string path = argv[1];
    std::vector<double> dns;
    for (int i = 2; i < argc; ++i) dns.push_back(std::atof(argv[i]));
    const ser::FileInfo info = ser::OpenVideo(path, ser::BayerChoice::Auto)->Info();
    const int sites = stack::LumaSites(info);
    const double mx = std::ldexp(1.0, info.normDepth) - 1;
    auto fit = [&](double dn) {
        const stack::SourceFactory f = [&]() -> std::unique_ptr<ser::FrameSource> {
            return std::make_unique<Injected>(ser::OpenVideo(path, ser::BayerChoice::Auto), dn);
        };
        if (!std::getenv("PASS1")) return stack::FitNoiseModel(f, info, 32, {});
        stack::NoiseSamples s1;
        auto r = f();
        for (const stack::NoiseRun& run : stack::NoiseRuns(info)) {
            std::vector<stack::NoisePlane> pl;
            for (int64_t i = 0; i < run.triples + 3; ++i) pl.emplace_back(stack::MeasurementLuma(r->ReadFrame(run.first + i), info));
            for (int64_t i = 0; i < run.triples; ++i) stack::AddTriple(pl[i], pl[i + 1], pl[i + 2], pl[i + 3], s1);
        }
        return stack::FitNoise(s1, sites);
    };
    const stack::NoiseModel m0 = fit(0);
    std::vector<stack::NoiseModel> ms;
    for (double dn : dns) ms.push_back(fit(dn));
    std::printf("%s: normDepth %d, sites %d\n", path.c_str(), info.normDepth, sites);
    std::printf("level    model      ");
    for (double dn : dns) std::printf("| +%.1f DN: v0 %.3g, rise, rise/v0 ", dn, (dn * dn + 1.0 / 12) / (mx * mx * sites));
    std::printf("\n");
    for (double x : {0.02, 0.03, 0.05, 0.075, 0.1, 0.15, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9}) {
        std::printf("%.3f  %.4g", x, m0.VarianceAtMean(x));
        for (size_t k = 0; k < dns.size(); ++k) {
            const double v0 = (dns[k] * dns[k] + 1.0 / 12) / (mx * mx * sites), rise = ms[k].VarianceAtMean(x) - m0.VarianceAtMean(x);
            std::printf(" | %.4g %.3f", rise, rise / v0);
        }
        std::printf("\n");
    }
}
