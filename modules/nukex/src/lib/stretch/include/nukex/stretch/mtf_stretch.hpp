#pragma once
#include "nukex/stretch/stretch_op.hpp"

#include <algorithm>
#include <cstddef>

namespace nukex {

class MTFStretch : public StretchOp {
public:
    float midtone    = 0.25f;
    float shadows    = 0.0f;
    float highlights = 1.0f;
    bool  luminance_only = false;
    MTFStretch() { name = "MTF"; category = StretchCategory::FINISHER; }
    void apply(Image& img) const override;
    float apply_scalar(float x) const override;

    /// Batched MTF: out[i] = apply_scalar(in[i]) for i in [0, n), bit for bit.
    /// Inline, branch-free in the loop, so the compiler can vectorise it; the
    /// per-pixel virtual / std::function path cannot be. `in == out` (in
    /// place) is allowed; any other overlap is not.
    void apply(const float* in, float* out, std::size_t n) const {
        const float range = highlights - shadows;
        const float m     = midtone;
        // The scalar path's early-outs, which do not depend on x.
        if (range < 1e-10f || m <= 0.0f) { std::fill(out, out + n, 0.0f); return; }
        if (m >= 1.0f)                   { std::fill(out, out + n, 1.0f); return; }
        const float s = shadows, a = m - 1.0f, b = 2.0f * m - 1.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float xc = std::clamp((in[i] - s) / range, 0.0f, 1.0f);
            out[i] = (a * xc) / (b * xc - m);
        }
    }

    std::map<std::string, std::pair<float, float>> param_bounds() const override;
    bool                                           set_param(const std::string&, float) override;
    std::optional<float>                           get_param(const std::string&) const override;
};
} // namespace nukex
