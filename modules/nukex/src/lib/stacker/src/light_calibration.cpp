#include "nukex/stacker/light_calibration.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <tuple>

namespace nukex {

namespace {

std::string fmt(double v, int prec = 1) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

std::string fmt_g(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

std::string trimmed_upper(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    std::string out = s.substr(a, b - a);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool same_setting(double a, double b) {
    return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(a));
}

std::string master_name(const MasterFrame& m) {
    if (!m.path.empty()) return std::filesystem::path(m.path).filename().string();
    return std::string("master ") + master_kind_name(m.kind) + " (" + std::to_string(m.n_frames) + " frames)";
}

/// One keyword compared between a frame and a master. Appends to `why`.
void compare_setting(const char* name, bool fa, double a, bool fb, double b,
                     std::vector<std::string>& why, std::vector<std::string>* warnings) {
    if (fa && fb) {
        if (!same_setting(a, b))
            why.push_back(std::string(name) + " " + fmt_g(a) + " vs " + fmt_g(b));
    } else if (fa != fb) {
        why.push_back(std::string(name) + " is in only one header (" + (fa ? "frame " + fmt_g(a) : "master " + fmt_g(b))
                      + "), so it cannot be checked");
    } else if (warnings) {
        warnings->push_back(std::string("neither frame nor master records ") + name + "; it could not be checked");
    }
}

} // namespace

std::string CalibrationMatcher::compatibility(const CalibrationHeader& f, const CalibrationHeader& m,
                                              std::vector<std::string>* warnings) {
    std::vector<std::string> why;
    const std::string ci = trimmed_upper(f.instrument), mi = trimmed_upper(m.instrument);
    if (!ci.empty() && !mi.empty()) {
        if (ci != mi) why.push_back("camera '" + f.instrument + "' vs '" + m.instrument + "'");
    } else if (ci.empty() != mi.empty()) {
        why.push_back("camera (INSTRUME) is in only one header, so it cannot be checked");
    } else if (warnings) {
        warnings->push_back("neither frame nor master records the camera (INSTRUME); it could not be checked");
    }
    if (f.width != m.width || f.height != m.height || f.channels != m.channels)
        why.push_back("geometry " + std::to_string(f.width) + "x" + std::to_string(f.height) + "x" + std::to_string(f.channels)
                      + " vs " + std::to_string(m.width) + "x" + std::to_string(m.height) + "x" + std::to_string(m.channels));
    if (trimmed_upper(f.bayer_pattern) != trimmed_upper(m.bayer_pattern))
        why.push_back("CFA pattern '" + f.bayer_pattern + "' vs '" + m.bayer_pattern + "'");
    if (f.xbin != m.xbin || f.ybin != m.ybin)
        why.push_back("binning " + std::to_string(f.xbin) + "x" + std::to_string(f.ybin) + " vs "
                      + std::to_string(m.xbin) + "x" + std::to_string(m.ybin));
    compare_setting("gain", f.has_gain, f.gain, m.has_gain, m.gain, why, warnings);
    compare_setting("offset", f.has_offset, f.offset, m.has_offset, m.offset, why, warnings);
    if (f.full_scale > 0 && m.full_scale > 0 && f.full_scale != m.full_scale)
        why.push_back("data scale (full scale " + fmt_g(f.full_scale) + " vs " + fmt_g(m.full_scale) + ")");
    std::string out;
    for (std::size_t i = 0; i < why.size(); ++i) out += (i ? "; " : "") + why[i];
    return out;
}

PlanResult CalibrationMatcher::plan(const CalibrationHeader& frame,
                                    const std::vector<MasterFrame>& biases,
                                    const std::vector<MasterFrame>& darks,
                                    const MatchTolerance& tol,
                                    bool require_dark, bool require_bias) {
    PlanResult r;

    // ── Bias: compatible camera configuration; nearest temperature ──
    const MasterFrame* bias = nullptr;
    std::vector<std::string> bias_refusals;
    double bias_dt = std::numeric_limits<double>::infinity();
    for (const auto& b : biases) {
        std::vector<std::string> w;
        const std::string c = compatibility(frame, b.header, &w);
        if (!c.empty()) { bias_refusals.push_back(master_name(b) + ": " + c); continue; }
        const double dt = (frame.has_temperature && b.header.has_temperature)
                        ? std::fabs(frame.temperature - b.header.temperature) : 0.0;
        if (!bias || dt < bias_dt) {
            bias = &b; bias_dt = dt;
            r.warnings = w;
        }
    }
    if (require_bias && !biases.empty() && !bias) {
        r.error = "No master bias matches " + frame.describe() + ":";
        for (const auto& s : bias_refusals) r.error += "\n  - " + s;
        return r;
    }

    // ── Dark: the candidate needing the least correction ────────────
    struct Candidate {
        const MasterFrame* m; bool scaled; double ratio, tf, dt; std::vector<std::string> warnings;
    };
    std::vector<Candidate> ok;
    std::vector<std::string> dark_refusals;
    for (const auto& d : darks) {
        std::vector<std::string> w;
        const std::string c = compatibility(frame, d.header, &w);
        if (!c.empty()) { dark_refusals.push_back(master_name(d) + ": " + c); continue; }
        if (!frame.has_exposure || !d.header.has_exposure) {
            dark_refusals.push_back(master_name(d) + ": exposure is not recorded in "
                                    + std::string(frame.has_exposure ? "the dark" : "the frame")
                                    + ", so the dark signal cannot be matched");
            continue;
        }
        if (!(d.header.exposure > 0.0)) {
            dark_refusals.push_back(master_name(d) + ": dark exposure " + fmt_g(d.header.exposure) + " s");
            continue;
        }
        const double ratio = frame.exposure / d.header.exposure;
        const bool same_exposure = std::fabs(frame.exposure - d.header.exposure)
                                   <= tol.exposure_rel * std::max(frame.exposure, 1e-9);
        double dt = 0.0;
        if (frame.has_temperature && d.header.has_temperature) {
            dt = frame.temperature - d.header.temperature;
        } else if (frame.has_temperature != d.header.has_temperature) {
            dark_refusals.push_back(master_name(d) + ": temperature is recorded in only "
                                    + std::string(frame.has_temperature ? "the frame" : "the dark")
                                    + ", so it cannot be checked");
            continue;
        } else {
            w.push_back("neither frame nor dark records a temperature; it could not be checked");
        }
        double tf = 1.0;
        const bool temp_scaling = tol.dark_doubling_temperature_c > 0.0;
        if (temp_scaling) {
            if (std::fabs(dt) > tol.temperature_scaling_limit_c) {
                dark_refusals.push_back(master_name(d) + ": temperature " + fmt(d.header.temperature) + " C is "
                                        + fmt(std::fabs(dt)) + " C from the frame's " + fmt(frame.temperature)
                                        + " C, beyond the " + fmt(tol.temperature_scaling_limit_c)
                                        + " C dark-current scaling limit");
                continue;
            }
            tf = std::exp2(dt / tol.dark_doubling_temperature_c);
        } else if (std::fabs(dt) > tol.temperature_c) {
            dark_refusals.push_back(master_name(d) + ": temperature " + fmt(d.header.temperature) + " C is "
                                    + fmt(std::fabs(dt)) + " C from the frame's " + fmt(frame.temperature)
                                    + " C, more than the " + fmt(tol.temperature_c)
                                    + " C tolerance (dark-current temperature scaling is off)");
            continue;
        }
        const bool scaled = !same_exposure || tf != 1.0;
        if (!same_exposure && !tol.allow_exposure_scaling) {
            dark_refusals.push_back(master_name(d) + ": exposure " + fmt_g(d.header.exposure) + " s vs the frame's "
                                    + fmt_g(frame.exposure) + " s, and exposure scaling is off");
            continue;
        }
        if (scaled && !bias) {
            dark_refusals.push_back(master_name(d) + ": would have to be scaled by "
                                    + fmt(ratio * tf, 4) + " (" + fmt_g(frame.exposure) + " s / "
                                    + fmt_g(d.header.exposure) + " s"
                                    + (tf != 1.0 ? ", temperature x" + fmt(tf, 4) : std::string())
                                    + "), which needs a compatible master bias to separate the pedestal "
                                    "from the dark signal, and none was given");
            continue;
        }
        ok.push_back({&d, scaled, same_exposure ? 1.0 : ratio, tf, dt, w});
    }
    if (!ok.empty()) {
        auto rank = [](const Candidate& c) {
            return std::make_tuple(c.scaled ? 1 : 0, std::fabs(std::log(c.ratio)), std::fabs(c.dt));
        };
        const Candidate& best = *std::min_element(ok.begin(), ok.end(),
            [&](const Candidate& a, const Candidate& b) { return rank(a) < rank(b); });
        r.plan.dark = best.m;
        r.plan.dark_scaled = best.scaled;
        r.plan.exposure_ratio = best.ratio;
        r.plan.temperature_factor = best.tf;
        r.plan.dark_scale = best.ratio * best.tf;
        r.warnings.insert(r.warnings.end(), best.warnings.begin(), best.warnings.end());
    } else if (require_dark && !darks.empty()) {
        r.error = "No master dark matches " + frame.describe() + ":";
        for (const auto& s : dark_refusals) r.error += "\n  - " + s;
        return r;
    }

    // A scaled dark subtracts the bias separately; an unscaled one contains it.
    r.plan.bias = (r.plan.dark && !r.plan.dark_scaled) ? nullptr : bias;

    std::string d;
    if (r.plan.dark && !r.plan.dark_scaled) {
        d = "dark " + master_name(*r.plan.dark) + " (" + fmt_g(r.plan.dark->header.exposure) + " s, "
          + fmt(r.plan.dark->header.temperature) + " C) subtracted as is: same exposure"
          + (r.plan.dark->header.has_temperature ? ", " + fmt(frame.temperature - r.plan.dark->header.temperature)
                                                   + " C from the frame" : std::string());
    } else if (r.plan.dark) {
        d = "bias " + master_name(*r.plan.bias) + ", then dark " + master_name(*r.plan.dark)
          + " (" + fmt_g(r.plan.dark->header.exposure) + " s) less bias, scaled by " + fmt(r.plan.dark_scale, 4)
          + " (exposure " + fmt(r.plan.exposure_ratio, 4)
          + (r.plan.temperature_factor != 1.0 ? ", temperature " + fmt(r.plan.temperature_factor, 4) : std::string()) + ")";
        // Accepted, but said out loud: the scale is the header ratio, and
        // dark signal is not exactly proportional to it. Measured on an
        // ASI2400MC Pro, a 180 s dark scaled to 300 s left the hot pixels
        // ~5 ADU over-subtracted (their 300/180 ratio fits at 1.19, not 1.67).
        r.warnings.push_back("dark " + master_name(*r.plan.dark) + " is scaled by "
                             + fmt(r.plan.dark_scale, 4) + " from its header values; dark signal (hot "
                             "pixels especially) is not exactly linear in exposure, and a dark of the "
                             "light's own exposure needs no scaling");
    } else if (r.plan.bias) {
        d = "bias " + master_name(*r.plan.bias) + " only";
    }
    r.plan.description = d;
    r.ok = true;
    return r;
}

Image LightCalibrator::pedestal(const CalibrationPlan& plan) {
    if (plan.dark && !plan.dark_scaled) return plan.dark->data.clone();
    if (plan.dark) {
        Image p = plan.bias->data.clone();
        const float s = static_cast<float>(plan.dark_scale);
        float* o = p.data();
        const float* d = plan.dark->data.data();
        const float* b = plan.bias->data.data();
        for (std::size_t i = 0; i < p.data_size(); ++i) o[i] = b[i] + s * (d[i] - b[i]);
        return p;
    }
    if (plan.bias) return plan.bias->data.clone();
    return Image();
}

std::string LightCalibrator::apply(Image& frame, double frame_full_scale,
                                   const CalibrationPlan& plan,
                                   const MasterFrame* master_flat,
                                   float min_flat_value) {
    auto check = [&](const MasterFrame* m) -> std::string {
        if (!m) return {};
        if (m->data.width() != frame.width() || m->data.height() != frame.height() ||
            m->data.n_channels() != frame.n_channels())
            return std::string("master ") + master_kind_name(m->kind) + " is "
                 + std::to_string(m->data.width()) + "x" + std::to_string(m->data.height()) + "x"
                 + std::to_string(m->data.n_channels()) + ", frame is " + std::to_string(frame.width()) + "x"
                 + std::to_string(frame.height()) + "x" + std::to_string(frame.n_channels());
        if (m->kind != MasterKind::FLAT && m->header.full_scale != frame_full_scale)
            return std::string("master ") + master_kind_name(m->kind) + " is on full scale "
                 + fmt_g(m->header.full_scale) + ", frame on " + fmt_g(frame_full_scale);
        return {};
    };
    for (const MasterFrame* m : {plan.bias, plan.dark, master_flat}) {
        const std::string e = check(m);
        if (!e.empty()) return e;
    }

    float* v = frame.data();
    const std::size_t n = frame.data_size();
    if (plan.dark && !plan.dark_scaled) {
        const float* d = plan.dark->data.data();
        for (std::size_t i = 0; i < n; ++i) v[i] -= d[i];
    } else if (plan.dark) {
        const float* d = plan.dark->data.data();
        const float* b = plan.bias->data.data();
        const float s = static_cast<float>(plan.dark_scale);
        for (std::size_t i = 0; i < n; ++i) v[i] = v[i] - b[i] - s * (d[i] - b[i]);
    } else if (plan.bias) {
        const float* b = plan.bias->data.data();
        for (std::size_t i = 0; i < n; ++i) v[i] -= b[i];
    }
    if (master_flat) {
        const float* f = master_flat->data.data();
        for (std::size_t i = 0; i < n; ++i) {
            const float fv = f[i] < min_flat_value ? min_flat_value : f[i];
            v[i] /= fv;
        }
    }
    return {};
}

} // namespace nukex
