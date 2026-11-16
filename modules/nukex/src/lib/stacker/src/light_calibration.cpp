#include "nukex/stacker/light_calibration.hpp"
#include "nukex/core/parallel_for.hpp"
#include "nukex/fitting/huber_estimator.hpp"
#include "nukex/fitting/robust_stats.hpp"

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
          + " (" + fmt_g(r.plan.dark->header.exposure) + " s) less bias, scaled by a per-light fit seeded at " + fmt(r.plan.dark_scale, 4)
          + " (exposure " + fmt(r.plan.exposure_ratio, 4)
          + (r.plan.temperature_factor != 1.0 ? ", temperature " + fmt(r.plan.temperature_factor, 4) : std::string()) + ")";
    } else if (r.plan.bias) {
        d = "bias " + master_name(*r.plan.bias) + " only";
    }
    r.plan.description = d;
    r.ok = true;
    return r;
}

PlanResult CalibrationMatcher::flat_plan_from_darks(const CalibrationHeader& flat,
                                                    const std::vector<MasterFrame>& darks,
                                                    const MatchTolerance& tol,
                                                    MasterFrame& estimated_bias) {
    static const std::string kPrefix = "no master bias or dark-flat: ";

    // 1. A dark at the flats' exposure and temperature is their dark-flat.
    //    With no bias offered, plan() accepts only an unscaled dark.
    PlanResult same = plan(flat, {}, darks, tol, /*require_dark=*/false, /*require_bias=*/false);
    if (same.ok && same.plan.dark && !same.plan.dark_scaled) {
        same.plan.description = kPrefix + same.plan.description;
        return same;
    }

    // 2. A compatible dark scaled around its own floor.
    PlanResult r;
    const MasterFrame* best = nullptr;
    std::vector<std::string> best_warnings, refusals;
    if (!flat.has_exposure)
        refusals.push_back("the flats record no exposure, so no dark can be scaled to them");
    for (const auto& d : darks) {
        if (!flat.has_exposure) break;
        std::vector<std::string> w;
        const std::string c = compatibility(flat, d.header, &w);
        if (!c.empty()) { refusals.push_back(master_name(d) + ": " + c); continue; }
        if (!d.header.has_exposure || !(d.header.exposure > 0.0)) {
            refusals.push_back(master_name(d) + ": no positive exposure recorded, so it cannot be scaled to the flats");
            continue;
        }
        auto dt = [&](const MasterFrame& m) {
            return (flat.has_temperature && m.header.has_temperature)
                 ? std::fabs(flat.temperature - m.header.temperature) : 0.0;
        };
        if (!best || d.header.exposure < best->header.exposure ||
            (d.header.exposure == best->header.exposure && dt(d) < dt(*best))) {
            best = &d;
            best_warnings = w;
        }
    }
    if (!best) {
        r.error = "no master bias or dark-flat was given, and no master dark can stand in for one ("
                  "a dark at the flats' exposure, or a compatible dark to estimate their pedestal from) for "
                  + flat.describe() + ":";
        for (const auto& s : refusals) r.error += "\n  - " + s;
        return r;
    }

    std::vector<double> levels;
    estimated_bias = LightCalibrator::estimate_bias(*best, &levels);
    const double k = flat.exposure / best->header.exposure;
    r.plan.bias = &estimated_bias;
    r.plan.dark = best;
    r.plan.dark_scaled = true;
    r.plan.exposure_ratio = k;
    r.plan.temperature_factor = 1.0;
    r.plan.dark_scale = k;
    r.warnings = best_warnings;

    std::string lv;
    for (std::size_t i = 0; i < levels.size(); ++i) lv += (i ? ", " : "") + fmt(levels[i], 1);
    std::string d = kPrefix + "bias level estimated as the floor (median per CFA channel) of dark "
                  + master_name(*best) + ", " + lv + " ADU (it overstates the bias by that dark's median "
                  "dark signal and omits the bias's fixed pattern), plus the dark less its floor scaled by "
                  + fmt(k, 4) + " (" + fmt_g(flat.exposure) + " s / " + fmt_g(best->header.exposure) + " s";
    if (flat.has_temperature && best->header.has_temperature)
        d += "; dark " + fmt(best->header.temperature - flat.temperature) + " C from the flats, not temperature-scaled";
    d += ")";
    r.plan.description = d;
    r.ok = true;
    return r;
}

MasterFrame LightCalibrator::estimate_bias(const MasterFrame& dark, std::vector<double>* levels_adu) {
    MasterFrame b;
    b.kind = MasterKind::BIAS;
    b.header = dark.header;
    b.header.image_type = "Bias";
    b.header.has_exposure = false;
    b.header.exposure = 0.0;
    b.n_frames = 0;
    b.temperature_min = dark.temperature_min;
    b.temperature_max = dark.temperature_max;
    b.estimator = "floor of a master dark";
    b.signature = "floor(" + dark.signature + ")";
    b.data = dark.data.clone();
    if (levels_adu) levels_adu->clear();

    const int W = b.data.width(), H = b.data.height(), C = b.data.n_channels();
    if (W <= 0 || H <= 0) return b;
    const int s = dark.header.bayer_pattern.empty() ? 1 : 2;
    const double fs = dark.header.full_scale > 0.0 ? dark.header.full_scale : 1.0;
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>((W + 1) / s) * ((H + 1) / s));
    for (int c = 0; c < C; ++c) {
        const float* src = dark.data.channel_data(c);
        float* dst = b.data.channel_data(c);
        for (int py = 0; py < s; ++py)
            for (int px = 0; px < s; ++px) {
                v.clear();
                for (int y = py; y < H; y += s)
                    for (int x = px; x < W; x += s)
                        v.push_back(src[static_cast<std::size_t>(y) * W + x]);
                if (v.empty()) continue;
                const float m = median_inplace(v.data(), static_cast<int>(v.size()));
                for (int y = py; y < H; y += s)
                    for (int x = px; x < W; x += s)
                        dst[static_cast<std::size_t>(y) * W + x] = m;
                if (levels_adu) levels_adu->push_back(static_cast<double>(m) * fs);
            }
    }
    return b;
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

namespace {

/// (A - B)(p) minus the median of its eight same-colour neighbours at
/// distance `s` (an interior pixel of one channel plane), without forming the
/// difference image.
inline float neighbour_excess_diff(const float* a, const float* b, int W, int x, int y, int s) {
    float nb[8];
    int k = 0;
    for (int dy = -s; dy <= s; dy += s)
        for (int dx = -s; dx <= s; dx += s)
            if (dx || dy) {
                const std::size_t i = static_cast<std::size_t>(y + dy) * W + (x + dx);
                nb[k++] = a[i] - b[i];
            }
    const std::size_t c = static_cast<std::size_t>(y) * W + x;
    return (a[c] - b[c]) - median_inplace(nb, 8);
}

} // namespace

std::shared_ptr<const DarkStructure> LightCalibrator::dark_structure(const MasterFrame& bias,
                                                                     const MasterFrame& dark,
                                                                     float threshold_sigma) {
    auto st = std::make_shared<DarkStructure>();
    const int W = dark.data.width(), H = dark.data.height(), C = dark.data.n_channels();
    st->stride = dark.header.bayer_pattern.empty() ? 1 : 2;
    const int s = st->stride;
    if (W <= 2 * s || H <= 2 * s || bias.data.width() != W || bias.data.height() != H ||
        bias.data.n_channels() != C)
        return st;
    const std::size_t plane = static_cast<std::size_t>(W) * H;
    // Excess of every interior pixel, then its robust scale.
    std::vector<float> ex(plane * C, 0.0f);
    for (int c = 0; c < C; ++c) {
        const float* d = dark.data.channel_data(c);
        const float* b = bias.data.channel_data(c);
        float* o = ex.data() + plane * c;
        parallel_for_dynamic(H - 2 * s, 16, [&](int r, int) {
            const int y = r + s;
            for (int x = s; x < W - s; ++x)
                o[static_cast<std::size_t>(y) * W + x] = neighbour_excess_diff(d, b, W, x, y, s);
        });
    }
    std::vector<float> interior;
    interior.reserve(static_cast<std::size_t>(W - 2 * s) * (H - 2 * s) * C);
    for (int c = 0; c < C; ++c)
        for (int y = s; y < H - s; ++y)
            for (int x = s; x < W - s; ++x)
                interior.push_back(ex[plane * c + static_cast<std::size_t>(y) * W + x]);
    st->sigma = 1.4826f * mad(interior.data(), static_cast<int>(interior.size()));
    interior.clear();
    interior.shrink_to_fit();
    // Never below half an ADU: a noise-free (or heavily quantised) master has
    // a zero noise scale, and its structure is every pixel that differs from
    // its neighbours at all.
    const double fs = dark.header.full_scale > 0.0 ? dark.header.full_scale : 1.0;
    const float thr = std::max(threshold_sigma * st->sigma, static_cast<float>(0.5 / fs));
    for (int c = 0; c < C; ++c)
        for (int y = s; y < H - s; ++y)
            for (int x = s; x < W - s; ++x) {
                const std::size_t i = plane * c + static_cast<std::size_t>(y) * W + x;
                if (std::fabs(ex[i]) > thr) {
                    st->index.push_back(static_cast<std::uint32_t>(i));
                    st->excess.push_back(ex[i]);
                }
            }
    return st;
}

void LightCalibrator::prepare(CalibrationPlan& plan) {
    if (plan.dark && plan.dark_scaled && plan.bias && !plan.structure)
        plan.structure = dark_structure(*plan.bias, *plan.dark);
}

DarkScaleFit LightCalibrator::fit_dark_scale(const Image& frame, const MasterFrame& bias,
                                             const MasterFrame& dark, const DarkStructure& st,
                                             double initial, int min_pixels,
                                             double max_relative_error) {
    DarkScaleFit fit;
    fit.initial = initial;
    fit.scale = initial;
    const int W = frame.width(), H = frame.height();
    const int n = static_cast<int>(st.index.size());
    fit.n_pixels = n;
    auto refuse = [&](const std::string& why) {
        fit.well_conditioned = false;
        fit.scale = initial;
        fit.warning = why + "; the header ratio " + fmt(initial, 4) + " is used instead";
        return fit;
    };
    if (n < min_pixels)
        return refuse("only " + std::to_string(n) + " pixels of the dark carry structure above its noise "
                      "(at least " + std::to_string(min_pixels) + " are needed to fit a dark scale)");
    if (frame.width() != dark.data.width() || frame.height() != dark.data.height())
        return refuse("frame and dark geometry differ");

    // The light's excess at the structure pixels, on (L - B).
    const std::size_t plane = static_cast<std::size_t>(W) * H;
    std::vector<float> rl(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const std::size_t idx = st.index[i];
        const int c = static_cast<int>(idx / plane);
        const std::size_t off = idx % plane;
        const int y = static_cast<int>(off / W), x = static_cast<int>(off % W);
        rl[i] = neighbour_excess_diff(frame.channel_data(c), bias.data.channel_data(c), W, x, y, st.stride);
    }
    const float* rd = st.excess.data();

    // Huber IRLS for the slope through the origin, seeded at the header ratio.
    const double tuning = HuberEstimator::Config{}.tuning;
    std::vector<float> e(static_cast<std::size_t>(n));
    double k = initial, den = 0.0, sigma = 0.0;
    for (int it = 0; it < 50; ++it) {
        for (int i = 0; i < n; ++i) e[i] = rl[i] - static_cast<float>(k) * rd[i];
        sigma = 1.4826 * mad(e.data(), n);
        // sigma == 0: at least half the residuals are exactly zero (noise-free
        // data, or a quantised light whose structure k already matches) --
        // every point is an inlier and the step is least squares.
        const double delta = (sigma > 0.0) ? tuning * sigma : std::numeric_limits<double>::infinity();
        double num = 0.0;
        den = 0.0;
        for (int i = 0; i < n; ++i) {
            const double r = std::fabs(static_cast<double>(e[i]));
            const double w = (r <= delta) ? 1.0 : delta / r;
            num += w * rd[i] * rl[i];
            den += w * static_cast<double>(rd[i]) * rd[i];
        }
        if (!(den > 0.0)) break;
        const double next = num / den;
        fit.iterations = it + 1;
        const bool converged = std::fabs(next - k) <= 1e-7 * std::max(std::fabs(k), 1e-12);
        k = next;
        if (converged) break;
    }
    fit.fitted = k;
    fit.standard_error = (den > 0.0) ? sigma / std::sqrt(den) : 0.0;
    if (!(den > 0.0))
        return refuse("the dark structure has no weight in the fit");
    if (!(k > 0.0) || k < initial / 4.0 || k > initial * 4.0)
        return refuse("the fitted scale " + fmt(k, 4) + " is not within a factor 4 of the header ratio");
    if (fit.standard_error > max_relative_error * k)
        return refuse("the fitted scale " + fmt(k, 4) + " +/- " + fmt(fit.standard_error, 4)
                      + " is poorly conditioned (relative error above " + fmt(100.0 * max_relative_error, 1) + "%)");
    fit.well_conditioned = true;
    fit.scale = k;
    return fit;
}

std::string LightCalibrator::apply(Image& frame, double frame_full_scale,
                                   const CalibrationPlan& plan,
                                   const MasterFrame* master_flat,
                                   float min_flat_value,
                                   DarkScaleFit* fit_out) {
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
        std::shared_ptr<const DarkStructure> st = plan.structure;
        if (!st) st = dark_structure(*plan.bias, *plan.dark);
        const DarkScaleFit fit = fit_dark_scale(frame, *plan.bias, *plan.dark, *st, plan.dark_scale);
        if (fit_out) *fit_out = fit;
        const float* d = plan.dark->data.data();
        const float* b = plan.bias->data.data();
        const float s = static_cast<float>(fit.scale);
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
