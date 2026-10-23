// Test-only: the probes' reader wrapper (stack-probe, focus-survey). Never shipped.
//   --frames N: the first N frames only (the capture is N frames long, noise model and all);
//   --blur S:   every frame blurred by a Gaussian of sigma S sensor px before the engine sees it, the
//               capture's own noise put back by level -- a synthetic defocus of a real capture.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "Engine.h"
#include "Measure.h"
#include "Noise.h"

namespace planetary::probe {

class ProbeSource : public ser::FrameSource {
public:
    // noise: the capture's noise model (FitNoiseModel of the unblurred capture), required with blur > 0.
    ProbeSource(std::unique_ptr<ser::FrameSource> s, int64_t limit, double blur, std::shared_ptr<const stack::NoiseModel> noise)
        : s_(std::move(s)), info_(s_->Info()), blur_(blur), noise_(std::move(noise)) {
        if (blur_ > 0 && !noise_) throw std::invalid_argument("ProbeSource: --blur needs the capture's noise model");
        info_.frameCount = std::min(info_.frameCount, limit);
        step_ = ser::IsBayer(info_.colorId) || stack::UndeclaredMosaic(info_) ? 2 : 1;
        maxValue_ = std::ldexp(1.0, info_.normDepth) - 1;
        sites_ = step_ == 2 ? 4 : info_.planes == 3 ? 3 : 1;
        if (blur_ > 0) {
            const double sg = blur_ / step_;  // in site-plane px
            const int r = static_cast<int>(std::ceil(3 * sg));
            double sum = 0;
            for (int k = -r; k <= r; ++k) {
                kernel_.push_back(std::exp(-0.5 * k * k / (sg * sg)));
                sum += kernel_.back();
            }
            for (double& k : kernel_) k /= sum;
        }
    }
    const ser::FileInfo& Info() const override { return info_; }
    ser::Frame ReadFrame(int64_t i) const override {
        if (i >= info_.frameCount) throw ser::SerError("ProbeSource: frame past --frames");
        ser::Frame f = s_->ReadFrame(i);
        if (blur_ > 0) Blur(f, i);
        return f;
    }
    std::optional<int64_t> FrameTimestamp(int64_t i) const override { return s_->FrameTimestamp(i); }

private:
    // Separable Gaussian over each plane of same-colour samples (stride step_ in x and y, per channel),
    // mirrored at the plane's edges; then the capture's own noise is put back (the blur removed it, a
    // defocused capture keeps it): Gaussian noise at each blurred sample's level from the capture's noise
    // model's photon-transfer line (Noise.h, Ruling P26: the noise a sensor adds there, without the
    // clipping its measured curve carries, which the rounding below re-creates; a sample of a K-sample luma
    // pixel has K times the luma's variance at its level; before P26, one level per plane, its
    // EstimatePixelNoise -- the sky's), seeded by the frame index, and the result rounded to whole DN again.
    void Blur(ser::Frame& f, int64_t index) const {
        const int W = f.width, H = f.height, C = f.channels, r = static_cast<int>(kernel_.size() / 2);
        std::vector<double> in(static_cast<size_t>(W) * H * C), tmp(in.size());
        for (size_t k = 0; k < in.size(); ++k) in[k] = f.samples[k];
        std::mt19937 rng(static_cast<unsigned>(1234567 + index));
        std::normal_distribution<double> gauss(0, 1);
        auto mirror = [](int i, int n) {
            if (n == 1) return 0;
            while (i < 0 || i >= n) i = i < 0 ? -i : 2 * (n - 1) - i;
            return i;
        };
        for (int py = 0; py < step_; ++py)
            for (int px = 0; px < step_; ++px)
                for (int c = 0; c < C; ++c) {
                    const int pw = (W - px + step_ - 1) / step_, ph = (H - py + step_ - 1) / step_;
                    auto at = [&](std::vector<double>& v, int x, int y) -> double& {
                        return v[(static_cast<size_t>(py + y * step_) * W + (px + x * step_)) * C + c];
                    };
                    for (int y = 0; y < ph; ++y)
                        for (int x = 0; x < pw; ++x) {
                            double s = 0;
                            for (int k = -r; k <= r; ++k) s += kernel_[static_cast<size_t>(k + r)] * at(in, mirror(x + k, pw), y);
                            at(tmp, x, y) = s;
                        }
                    for (int y = 0; y < ph; ++y)
                        for (int x = 0; x < pw; ++x) {
                            double s = 0;
                            for (int k = -r; k <= r; ++k) s += kernel_[static_cast<size_t>(k + r)] * at(tmp, x, mirror(y + k, ph));
                            const double v = noise_->Line(std::clamp(s / maxValue_, 0.0, 1.0)) * sites_;
                            at(in, x, y) = s + std::sqrt(v) * maxValue_ * gauss(rng);
                        }
                }
        for (size_t k = 0; k < in.size(); ++k) f.samples[k] = static_cast<uint16_t>(std::clamp(std::lround(in[k]), 0L, 65535L));
    }

    std::unique_ptr<ser::FrameSource> s_;
    ser::FileInfo info_;
    double blur_;
    std::shared_ptr<const stack::NoiseModel> noise_;
    double maxValue_ = 1, sites_ = 1;
    int step_ = 1;
    std::vector<double> kernel_;
};

// The model --blur puts back: FitNoiseModel of the same capture, unblurred (its first `limit` frames).
inline std::shared_ptr<const stack::NoiseModel> UnblurredNoise(const std::string& path, ser::BayerChoice bayer, int64_t limit,
                                                               int threads) {
    const stack::SourceFactory plain = [&]() -> std::unique_ptr<ser::FrameSource> {
        return std::make_unique<ProbeSource>(ser::OpenVideo(path, bayer), limit, 0.0, nullptr);
    };
    return std::make_shared<const stack::NoiseModel>(stack::FitNoiseModel(plain, plain()->Info(), threads, {}));
}

}  // namespace planetary::probe
