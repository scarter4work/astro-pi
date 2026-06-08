#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

// ── Base transformation T(x) ──
// For x >= 0: T(0) = 0, T'(0) = D.
// The shape parameter b selects the curve family.

float GHSStretch::base_transform(float x) const {
    if (D < 1e-10f) return x;  // Identity when D ≈ 0
    float Dx = D * x;

    if (std::abs(b - (-1.0f)) < 1e-6f) {
        // b = -1: Logarithmic
        return std::log1p(Dx);
    }
    if (b < -1e-6f) {
        // b < 0 (and b != -1): Integral family
        // T(x) = [1 - (1 - b*D*x)^((b+1)/b)] / [D*(b+1)]
        float base = 1.0f - b * Dx;
        if (base <= 0.0f) base = 1e-10f;  // Clamp to avoid negative base
        float exponent = (b + 1.0f) / b;
        return (1.0f - std::pow(base, exponent)) / (D * (b + 1.0f));
    }
    if (std::abs(b) < 1e-6f) {
        // b = 0: Exponential
        return 1.0f - std::exp(-Dx);
    }
    if (std::abs(b - 1.0f) < 1e-6f) {
        // b = 1: Harmonic
        return 1.0f - 1.0f / (1.0f + Dx);
    }
    // b > 0 (and b != 1): Hyperbolic family
    // T(x) = 1 - (1 + b*D*x)^(-1/b)
    return 1.0f - std::pow(1.0f + b * Dx, -1.0f / b);
}

float GHSStretch::base_transform_deriv(float x) const {
    if (D < 1e-10f) return 1.0f;
    float Dx = D * x;

    if (std::abs(b - (-1.0f)) < 1e-6f) {
        // b = -1: T'(x) = D / (1 + D*x)
        return D / (1.0f + Dx);
    }
    if (b < -1e-6f) {
        // b < 0: T'(x) = (1 - b*D*x)^(1/b)
        float base = 1.0f - b * Dx;
        if (base <= 0.0f) base = 1e-10f;
        return std::pow(base, 1.0f / b);
    }
    if (std::abs(b) < 1e-6f) {
        // b = 0: T'(x) = D * exp(-D*x)
        return D * std::exp(-Dx);
    }
    if (std::abs(b - 1.0f) < 1e-6f) {
        // b = 1: T'(x) = D * (1 + D*x)^(-2)
        float t = 1.0f + Dx;
        return D / (t * t);
    }
    // b > 0: T'(x) = D * (1 + b*D*x)^(-(1+b)/b)
    return D * std::pow(1.0f + b * Dx, -(1.0f + b) / b);
}

float GHSStretch::apply_scalar(float x) const {
    if (D < 1e-10f) return x;  // Identity

    // Clamp SP, LP, HP to valid ranges
    float sp = std::clamp(SP, 0.0f, 1.0f);
    float lp = std::clamp(LP, 0.0f, sp);
    float hp = std::clamp(HP, sp, 1.0f);

    // Compute the 4 piecewise regions:
    //   Region 1: [0, LP)     — linear extension below LP
    //   Region 2: [LP, SP)    — reflected base curve
    //   Region 3: [SP, HP]    — base curve
    //   Region 4: (HP, 1]     — linear extension above HP

    // Helper: T3(x) = T(x - sp) for x >= sp
    // Helper: T2(x) = -T(sp - x) for x <= sp

    float raw;
    if (x <= lp) {
        // Region 1: linear with slope = T2'(LP)
        float t2_lp = -base_transform(sp - lp);
        float t2_deriv_lp = base_transform_deriv(sp - lp);
        raw = t2_lp + t2_deriv_lp * (x - lp);
    } else if (x < sp) {
        // Region 2: reflected curve
        raw = -base_transform(sp - x);
    } else if (x <= hp) {
        // Region 3: base curve shifted to SP
        raw = base_transform(x - sp);
    } else {
        // Region 4: linear with slope = T3'(HP)
        float t3_hp = base_transform(hp - sp);
        float t3_deriv_hp = base_transform_deriv(hp - sp);
        raw = t3_hp + t3_deriv_hp * (x - hp);
    }

    // Normalize to [0, 1]: compute T1(0) and T4(1)
    float t1_0;  // Value at x = 0
    {
        float t2_lp = -base_transform(sp - lp);
        float t2_deriv_lp = base_transform_deriv(sp - lp);
        t1_0 = t2_lp + t2_deriv_lp * (0.0f - lp);
    }

    float t4_1;  // Value at x = 1
    {
        float t3_hp = base_transform(hp - sp);
        float t3_deriv_hp = base_transform_deriv(hp - sp);
        t4_1 = t3_hp + t3_deriv_hp * (1.0f - hp);
    }

    float range = t4_1 - t1_0;
    if (std::abs(range) < 1e-10f) return x;

    return std::clamp((raw - t1_0) / range, 0.0f, 1.0f);
}

void GHSStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

std::map<std::string, std::pair<float, float>> GHSStretch::param_bounds() const {
    return {
        {"D",  {0.0f, 15.0f}},
        {"b",  {-5.0f, 5.0f}},
        {"SP", {0.0f, 1.0f}},
        {"LP", {0.0f, 1.0f}},
        {"HP", {0.0f, 1.0f}},
    };
}

bool GHSStretch::set_param(const std::string& n, float v) {
    if (n == "D")  { D  = v; return true; }
    if (n == "b")  { b  = v; return true; }
    if (n == "SP") { SP = v; return true; }
    if (n == "LP") { LP = v; return true; }
    if (n == "HP") { HP = v; return true; }
    return false;
}

std::optional<float> GHSStretch::get_param(const std::string& n) const {
    if (n == "D")  return D;
    if (n == "b")  return b;
    if (n == "SP") return SP;
    if (n == "LP") return LP;
    if (n == "HP") return HP;
    return std::nullopt;
}

} // namespace nukex
