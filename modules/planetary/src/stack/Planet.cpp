#include "Planet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "Measure.h"
#include "MineralInternal.h"

namespace planetary::stack {

using namespace mineral_internal;

// ---- Colour calibration ----

std::optional<BodyColour> PlanetReflectance(ser::Target t) {
    // Mallama, Krobusek & Pavlov 2017, "Comprehensive wide-band magnitudes and albedos for the planets", Icarus 282
    // (arXiv:1609.05048), Table 7: geometric albedos A_B, A_V, A_Rc; B/G = A_B / A_V, R/G = A_Rc / A_V as spec call 3
    // tabulates them.
    switch (t) {
        case ser::Target::Mercury: return BodyColour{0.739, 1.113};  // 0.105 0.142 0.158
        case ser::Target::Venus: return BodyColour{0.955, 0.955};    // 0.658 0.689 0.658
        case ser::Target::Mars: return BodyColour{0.518, 1.471};     // 0.088 0.170 0.250
        case ser::Target::Jupiter: return BodyColour{0.823, 0.954};  // 0.443 0.538 0.513
        case ser::Target::Saturn: return BodyColour{0.679, 1.295};   // 0.339 0.499 0.646 (ring inclination 0)
        case ser::Target::Uranus: return BodyColour{1.150, 0.541};   // 0.561 0.488 0.264
        case ser::Target::Neptune: return BodyColour{1.271, 0.511};  // 0.562 0.442 0.226
        case ser::Target::Moon:
        case ser::Target::Other: break;
    }
    return std::nullopt;
}

namespace {

// CalibrateColour's background rule: a sky plane needs 1% of the frame.
constexpr double kMinSkyShare = 0.01;

// The disc's sums less the background. With `use` empty: over the disc's pixels with no saturated sample, which
// `use` then flags; else over the pixels `use` flags (the same pixels after the scaling).
struct DiscSums {
    double sum[3] = {0, 0, 0};
    size_t used = 0, saturated = 0;
};
DiscSums SumDisc(const ser::FloatImage& rgb, const LunarMasks& m, const Plane* bg, std::vector<uint8_t>& use) {
    const int w = rgb.width, h = rgb.height;
    const bool pick = use.empty();
    if (pick) use.assign(m.disk.size(), 0);
    DiscSums s;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!m.disk[i]) continue;
            const float* p = &rgb.data[i * 3];
            if (pick) {
                if (p[0] >= kClippedLuma || p[1] >= kClippedLuma || p[2] >= kClippedLuma) {
                    ++s.saturated;
                    continue;
                }
                use[i] = 1;
            } else if (!use[i]) {
                ++s.saturated;
                continue;
            }
            for (int c = 0; c < 3; ++c) s.sum[c] += p[c] - bg[c].At(x, y, w, h);
            ++s.used;
        }
    return s;
}

}  // namespace

StepRecord CalibratePlanetColour(ser::FloatImage& rgb, const LunarMasks& m, ser::Target t) {
    RequireMasks(rgb, m, "CalibratePlanetColour");
    const int w = rgb.width, h = rgb.height;
    const size_t n = Pixels(rgb);
    StepRecord rec;
    rec.skipped = true;
    const std::optional<BodyColour> ref = PlanetReflectance(t);
    if (!ref) {
        rec.history = std::string("Colour calibration skipped: no catalogue colour for the target ") + ser::TargetName(t) +
                      " (the planet path calibrates to a planet's reflectance); unchanged";
        return rec;
    }
    if (m.diskCount == 0) {
        rec.history = "Colour calibration skipped: no planet disc (" + m.reason + "); unchanged";
        return rec;
    }
    // Background reference: the sky's plane per channel, as CalibrateColour, so a gradient or pedestal does not bias
    // the disc's colour.
    const bool haveSky = m.skyCount >= kMinSkyShare * n;
    Plane bg[3];
    if (haveSky)
        for (int c = 0; c < 3; ++c) bg[c] = SkyPlane(rgb, m, c);
    std::vector<uint8_t> use;
    const DiscSums before = SumDisc(rgb, m, bg, use);
    if (before.used < kPlanetMinDiscPx || !(before.sum[0] > 0 && before.sum[1] > 0 && before.sum[2] > 0)) {
        rec.history = "Colour calibration skipped: " + std::to_string(before.used) + " unsaturated disc px (" +
                      std::to_string(before.saturated) + " with a saturated sample left out)" +
                      (before.used < kPlanetMinDiscPx ? ", under the " + std::to_string(kPlanetMinDiscPx) +
                                                            " a planet's colour reference needs"
                                                      : std::string(", and a channel's level is not positive")) +
                      "; unchanged";
        return rec;
    }
    // R and B scaled about the background so the disc's mean has the catalogue's B/G and R/G; G keeps its level.
    const double gain[3] = {ref->rOverG * before.sum[1] / before.sum[0], 1.0, ref->bOverG * before.sum[1] / before.sum[2]};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = &rgb.data[(static_cast<size_t>(y) * w + x) * 3];
            for (int c : {0, 2}) {
                const double b = bg[c].At(x, y, w, h);
                p[c] = static_cast<float>(b + (p[c] - b) * gain[c]);
            }
        }
    // Measured again on the same pixels.
    const DiscSums after = SumDisc(rgb, m, bg, use);
    rec.skipped = false;
    rec.history = std::string("Colour calibration: ") + ser::TargetName(t) +
                  Fmt("'s reflectance B/G %.3f R/G %.3f", ref->bOverG, ref->rOverG) +
                  " (Mallama, Krobusek & Pavlov 2017, Icarus 282, Table 7; camera B, G, R taken as Johnson B, Johnson V, "
                  "Cousins Rc); disc " + std::to_string(before.used) + " px (" + std::to_string(before.saturated) +
                  " with a saturated sample left out)" +
                  Fmt(", B/G %.3f R/G %.3f before, ", before.sum[2] / before.sum[1], before.sum[0] / before.sum[1]) +
                  Fmt("%.3f and %.3f after", after.sum[2] / after.sum[1], after.sum[0] / after.sum[1]) +
                  "; background = " +
                  (haveSky ? std::string("sky plane") : "none (sky " + Percent(m.skyCount, n) + " of the frame, under 1%)") +
                  Fmt("; gains R %.4f G 1 B %.4f", gain[0], gain[2]);
    return rec;
}


// ---- Channel alignment for small discs ----

namespace {

constexpr int kLimbBand = 4;          // px of sky outside the disc in the fit (the limb's outer flank)
constexpr int kLimbInner = 4;         // px of disc inside its edge in the fit (the limb's inner flank)
constexpr double kSectorArc = 4;      // px of limb per sector (8 to 64 sectors)
constexpr int kJackGroups = 8;        // the jackknife deletes one of 8 runs of neighbouring sectors at a time
constexpr int kMinSectors = 4;        // sectors with data needed
constexpr size_t kMinSectorPx = 8;    // px a sector needs to count
constexpr double kPreSmooth = 1.0;    // AlignChannels' measurement smoothing (px)
constexpr double kNormalSmooth = 2.0; // the smoothing of G whose gradient gives the limb's normal (px)
constexpr double kMaxShift = 8;       // no convergence beyond this (px)
constexpr int kCropMargin = 4 + 8 + 4;  // around the band: the smoothing's reach + kMaxShift + taps and gradient
constexpr double kResampleMin = 0.1, kResampleSigmas = 3.0;  // AlignChannels' gate
constexpr double kCutMaxSigma = 0.05;  // a disc the frame cuts: corrected only when 1 sigma is within this (px)

// The fit's frame: a crop around the disc's limb.
struct FitData {
    ser::FloatImage smooth;       // the crop, smoothed by kPreSmooth
    std::vector<uint8_t> region;  // the limb band
    std::vector<int> sector;      // per pixel, 0 .. sectors - 1, by the normal's direction
    std::vector<float> nx, ny;    // per pixel, the limb's outward normal
    int sectors = 8;
};

// One sector's normal equations for (sx, sy, e), its gain and offset eliminated.
struct Sector {
    double Hs[9] = {}, gs[3] = {};
    double rss = 0;
    size_t n = 0;
};

// x = A^-1 b for a system of up to 3 unknowns (A and b are copied). False when singular or not finite.
bool SolveCopy(int n, const double* A, const double* b, double* x) {
    double M[9], v[3];
    std::copy(A, A + n * n, M);
    std::copy(b, b + n, v);
    if (!Solve(n, M, v, x)) return false;
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(x[i])) return false;
    return true;
}

// Per sector, at the shift s and the normal offset e: G ~ a_k (C(x + s) + e dC/dn) + b_k, with the least-squares
// a_k and b_k, and the normal equations of (sx, sy, e) with a_k and b_k eliminated (MeasureShift's Schur complement).
// Over the limb band's pixels whose Catmull-Rom taps and gradient stay inside the crop for shifts up to `bound` px.
// The Jacobian of position is the derivative along the limb's normal only: structure along the limb (bands that
// cross it, the rings' arms) carries colour as well as position, and is not used. One thread, row order.
std::vector<Sector> Accumulate(const FitData& f, int c, double sx, double sy, double e, int bound, int threads) {
    const int w = f.smooth.width, h = f.smooth.height, K = f.sectors;
    const std::vector<float> C = ShiftedPlane(f.smooth, c, sx, sy, threads);
    std::vector<Sector> sec(static_cast<size_t>(K));
    const int x0 = 2 + bound, x1 = w - 4 - bound, y0 = 2 + bound, y1 = h - 4 - bound;
    auto normal = [&](size_t i) {
        return 0.5 * (C[i + 1] - C[i - 1]) * f.nx[i] + 0.5 * (C[i + w] - C[i - w]) * f.ny[i];
    };
    std::vector<double> sc(K), scc(K), sg(K), scg(K), a(K, 1.0), b(K, 0.0);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!f.region[i]) continue;
            const int k = f.sector[i];
            const double cv = C[i] + e * normal(i), gv = f.smooth.data[i * 3 + 1];
            sc[k] += cv;
            scc[k] += cv * cv;
            sg[k] += gv;
            scg[k] += cv * gv;
            ++sec[k].n;
        }
    for (int k = 0; k < K; ++k) {
        if (sec[k].n == 0) continue;
        const double n = static_cast<double>(sec[k].n), var = scc[k] - sc[k] * sc[k] / n, cov = scg[k] - sc[k] * sg[k] / n;
        a[k] = var > 1e-12 * n ? cov / var : 1.0;
        b[k] = (sg[k] - a[k] * sc[k]) / n;
    }
    std::vector<std::array<double, 25>> H(K);
    std::vector<std::array<double, 5>> g(K);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!f.region[i]) continue;
            const int k = f.sector[i];
            const double gn = normal(i), cv = C[i] + e * gn;
            const double r = f.smooth.data[i * 3 + 1] - (a[k] * cv + b[k]);
            const double J[5] = {a[k] * gn * f.nx[i], a[k] * gn * f.ny[i], a[k] * gn, cv, 1.0};
            sec[k].rss += r * r;
            for (int p = 0; p < 5; ++p) {
                g[k][p] += J[p] * r;
                for (int q = 0; q < 5; ++q) H[k][p * 5 + q] += J[p] * J[q];
            }
        }
    for (int k = 0; k < K; ++k) {
        Sector& S = sec[k];
        const double* Hk = H[k].data();
        const double t00 = Hk[18], t01 = Hk[19], t11 = Hk[24], tdet = t00 * t11 - t01 * t01;
        const double i00 = tdet > 0 ? t11 / tdet : 0, i01 = tdet > 0 ? -t01 / tdet : 0, i11 = tdet > 0 ? t00 / tdet : 0;
        for (int p = 0; p < 3; ++p) {
            const double hp3 = Hk[p * 5 + 3], hp4 = Hk[p * 5 + 4];
            const double m0 = hp3 * i00 + hp4 * i01, m1 = hp3 * i01 + hp4 * i11;  // H_p,theta H_theta^-1
            S.gs[p] = g[k][p] - (m0 * g[k][3] + m1 * g[k][4]);
            for (int q = 0; q < 3; ++q) S.Hs[p * 3 + q] = Hk[p * 5 + q] - (m0 * Hk[3 * 5 + q] + m1 * Hk[4 * 5 + q]);
        }
    }
    return sec;
}

// The jackknife's group of sector k of K: runs of neighbouring directions.
int GroupOf(size_t k, size_t K) { return static_cast<int>(k * kJackGroups / K); }

// The equations summed over the sectors with kMinSectorPx, less group `skip` (-1: none).
void Sum(const std::vector<Sector>& sec, int skip, double H[9], double g[3]) {
    std::fill(H, H + 9, 0.0);
    std::fill(g, g + 3, 0.0);
    for (size_t k = 0; k < sec.size(); ++k)
        if (sec[k].n >= kMinSectorPx && GroupOf(k, sec.size()) != skip) {
            for (int p = 0; p < 9; ++p) H[p] += sec[k].Hs[p];
            for (int p = 0; p < 3; ++p) g[p] += sec[k].gs[p];
        }
}

// Channel c's shift against G (measured false and reason set when it cannot be measured).
DispersionShift MeasureChannel(const FitData& f, int c, int threads, int& sectorsUsed, size_t& pixels, std::string& reason) {
    DispersionShift out;
    double sx = 0, sy = 0, e = 0;
    // Passes: the pixels used are those whose taps stay inside for shifts up to `bound`; when the solution moves past
    // it, the next pass starts there with the wider bound.
    int bound = 1;
    bool converged = false;
    for (int pass = 0; pass < 4 && !converged; ++pass) {
        for (int round = 0; round < 60; ++round) {
            const std::vector<Sector> sec = Accumulate(f, c, sx, sy, e, bound, threads);
            double H[9], g[3], d[3];
            Sum(sec, -1, H, g);
            if (!SolveCopy(3, H, g, d)) {
                reason = "the limb does not constrain the shift (singular fit)";
                return out;
            }
            sx += d[0];
            sy += d[1];
            e += d[2];
            if (!std::isfinite(sx) || !std::isfinite(sy) || std::hypot(sx, sy) > kMaxShift || std::fabs(e) > kMaxShift) {
                reason = Fmt("no convergence within %.0f px", kMaxShift);
                return out;
            }
            if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < 1e-4) {
                converged = true;
                break;
            }
        }
        if (!converged) {
            reason = "no convergence within 60 rounds";
            return out;
        }
        const int need = static_cast<int>(std::ceil(std::max(std::fabs(sx), std::fabs(sy))));
        if (need > bound) {
            bound = need;
            converged = false;
        }
    }
    if (!converged) {
        reason = "no convergence";
        return out;
    }
    // At the solution: the formal 1-sigma, the grouped jackknife and the dependence on e.
    const std::vector<Sector> sec = Accumulate(f, c, sx, sy, e, bound, threads);
    double H[9], g[3], rss = 0;
    Sum(sec, -1, H, g);
    size_t npx = 0;
    int used = 0;
    for (const Sector& S : sec)
        if (S.n >= kMinSectorPx) {
            rss += S.rss;
            npx += S.n;
            ++used;
        }
    sectorsUsed = used;
    pixels = npx;
    if (used < kMinSectors) {
        reason = std::to_string(used) + " sector(s) of the limb with data, under the " + std::to_string(kMinSectors) +
                 " needed (the disc is mostly outside the frame)";
        return out;
    }
    const double inf = std::numeric_limits<double>::infinity();
    // Smoothed residuals are correlated over ~4 pi sigma^2 px: the independent-pixel variance is inflated by that area.
    const double corrArea = std::max(1.0, 4 * M_PI * kPreSmooth * kPreSmooth);
    const double dof = static_cast<double>(npx) - 3 - 2.0 * used;
    const double var = dof > 0 ? corrArea * rss / dof : inf;
    double cx[3], cy[3];
    const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
    const bool inv = SolveCopy(3, H, ex, cx) && SolveCopy(3, H, ey, cy);
    const double fx = inv && cx[0] > 0 ? std::sqrt(var * cx[0]) : inf, fy = inv && cy[1] > 0 ? std::sqrt(var * cy[1]) : inf;
    // Delete-one-group jackknife (linearised at the solution): a colour that differs around the limb moves the
    // groups' solutions apart, a true dispersion moves every group alike. Groups of neighbouring sectors, not single
    // sectors: a band's colour spans several, and deleting one alone left the others to repeat its bias.
    std::vector<double> jx, jy;
    for (int grp = 0; grp < kJackGroups; ++grp) {
        bool have = false;
        for (size_t k = 0; k < sec.size(); ++k) have = have || (GroupOf(k, sec.size()) == grp && sec[k].n >= kMinSectorPx);
        if (!have) continue;
        double Hk[9], gk[3], d[3];
        Sum(sec, grp, Hk, gk);
        if (!SolveCopy(3, Hk, gk, d)) {
            jx.assign(1, inf);
            jy.assign(1, inf);
            break;
        }
        jx.push_back(sx + d[0]);
        jy.push_back(sy + d[1]);
    }
    auto jack = [](const std::vector<double>& v) {
        if (v.size() < 2) return std::numeric_limits<double>::infinity();
        double mean = 0;
        for (double x : v) mean += x;
        mean /= static_cast<double>(v.size());
        double ss = 0;
        for (double x : v) ss += (x - mean) * (x - mean);
        return std::sqrt(ss * (static_cast<double>(v.size()) - 1) / static_cast<double>(v.size()));
    };
    // How far the shift depends on e: with e held at 0 the shift moves by H_ss^-1 H_se e (linearised). Nil on a whole
    // disc, where e and the shift are orthogonal; on a limb the frame cuts they trade off, and this says by how much.
    const double det = H[0] * H[4] - H[1] * H[3], vx = H[2] * e, vy = H[5] * e;
    const double mx = det > 0 ? std::fabs((H[4] * vx - H[1] * vy) / det) : inf;
    const double my = det > 0 ? std::fabs((H[0] * vy - H[3] * vx) / det) : inf;
    out.dx = sx;
    out.dy = sy;
    out.ux = std::max({fx, jack(jx), mx});
    out.uy = std::max({fy, jack(jy), my});
    out.measured = std::isfinite(out.ux) && std::isfinite(out.uy);
    if (!out.measured) reason = "its uncertainty could not be measured (one run of the limb carries the fit)";
    return out;
}

}  // namespace

PlanetDispersion MeasurePlanetDispersion(const ser::FloatImage& rgb, const LunarMasks& m, int threads) {
    RequireMasks(rgb, m, "MeasurePlanetDispersion");
    PlanetDispersion out;
    if (m.diskCount == 0) {
        out.reason = "no planet disc (" + m.reason + ")";
        return out;
    }
    if (m.diskCount < kPlanetMinDiscPx) {
        out.reason = "the disc is " + std::to_string(m.diskCount) + " px, under the " + std::to_string(kPlanetMinDiscPx) +
                     " a dispersion measurement needs";
        return out;
    }
    const int w = rgb.width, h = rgb.height;
    // The limb band: within kLimbBand px outside the disc and kLimbInner px inside its edge. The disc's interior is
    // left out: its colour structure (bands, albedo features) reads as a shift (whole disc, one gain per sector: 0.16
    // px on a 40 px disc and 0.47 px on a 200 px one with 3% band colour; this band: 0.05 and 0.02). The frame's edge
    // is not a limb: disc pixels beside it are in the band only when near the limb.
    std::vector<uint8_t> region = m.disk, inner(m.disk.size());
    Dilate(region, w, h, kLimbBand);
    for (size_t i = 0; i < inner.size(); ++i) inner[i] = !m.disk[i];
    Dilate(inner, w, h, kLimbInner);
    for (size_t i = 0; i < region.size(); ++i) region[i] = region[i] && inner[i];
    double cx = 0, cy = 0;
    int bx0 = w, bx1 = -1, by0 = h, by1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (m.disk[i]) {
                cx += x;
                cy += y;
                out.frameCut = out.frameCut || x == 0 || y == 0 || x == w - 1 || y == h - 1;
            }
            if (region[i]) {
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
        }
    if (bx1 < 0) {
        out.reason = "the disc has no limb inside the frame (it fills the frame)";
        return out;
    }
    cx /= static_cast<double>(m.diskCount);
    cy /= static_cast<double>(m.diskCount);
    const int x0 = std::max(0, bx0 - kCropMargin), x1 = std::min(w - 1, bx1 + kCropMargin);
    const int y0 = std::max(0, by0 - kCropMargin), y1 = std::min(h - 1, by1 + kCropMargin);
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    const size_t cn = static_cast<size_t>(cw) * ch;
    FitData f;
    // Sectors of about kSectorArc px of limb (the disc's equivalent circumference), 8 to 64, each with its own gain:
    // the limb's colour may vary along it.
    f.sectors = std::clamp(static_cast<int>(2 * M_PI * std::sqrt(m.diskCount / M_PI) / kSectorArc), 8, 64);
    ser::FloatImage crop{cw, ch, 3, std::vector<float>(cn * 3)};
    f.region.assign(cn, 0);
    f.sector.assign(cn, 0);
    f.nx.assign(cn, 0.0f);
    f.ny.assign(cn, 0.0f);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const size_t i = static_cast<size_t>(y) * cw + x, j = static_cast<size_t>(y + y0) * w + (x + x0);
            for (int c = 0; c < 3; ++c) crop.data[i * 3 + c] = rgb.data[j * 3 + c];
            f.region[i] = region[j];
        }
    // The limb's outward normal: down the gradient of G smoothed by kNormalSmooth px, which the limb's edge dominates,
    // so it holds for a disc the frame cuts (the centroid is not the disc's centre there), a phase, an oblate globe
    // and the rings' edges alike; where that gradient is under 1% of the band's largest, away from the centroid.
    {
        const ser::FloatImage gs = ChromaLayer(crop, kNormalSmooth, threads);
        std::vector<double> gx(cn, 0.0), gy(cn, 0.0);
        double gmax = 0;
        for (int y = 1; y + 1 < ch; ++y)
            for (int x = 1; x + 1 < cw; ++x) {
                const size_t i = static_cast<size_t>(y) * cw + x;
                gx[i] = -0.5 * (gs.data[(i + 1) * 3 + 1] - gs.data[(i - 1) * 3 + 1]);
                gy[i] = -0.5 * (gs.data[(i + cw) * 3 + 1] - gs.data[(i - cw) * 3 + 1]);
                if (f.region[i]) gmax = std::max(gmax, std::hypot(gx[i], gy[i]));
            }
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const size_t i = static_cast<size_t>(y) * cw + x;
                const double th = std::hypot(gx[i], gy[i]) > 0.01 * gmax ? std::atan2(gy[i], gx[i])
                                                                          : std::atan2(y + y0 - cy, x + x0 - cx);
                f.nx[i] = static_cast<float>(std::cos(th));
                f.ny[i] = static_cast<float>(std::sin(th));
                f.sector[i] = std::clamp(static_cast<int>(std::floor((th + M_PI) / (2 * M_PI) * f.sectors)), 0, f.sectors - 1);
            }
    }
    // AlignChannels' reason for smoothing first: Catmull-Rom resampling cuts white noise's variance most at half
    // pixels, which pulls a weak fit toward them; smoothed, the noise has little left at those frequencies.
    f.smooth = ChromaLayer(crop, kPreSmooth, threads);
    std::string reasonR, reasonB;
    int secR = 0, secB = 0;
    size_t pxR = 0, pxB = 0;
    out.r = MeasureChannel(f, 0, threads, secR, pxR, reasonR);
    out.b = MeasureChannel(f, 2, threads, secB, pxB, reasonB);
    out.sectors = std::min(secR, secB);
    out.pixels = std::min(pxR, pxB);
    if (!out.r.measured) out.reason = "the R shift against G could not be measured: " + reasonR;
    else if (!out.b.measured) out.reason = "the B shift against G could not be measured: " + reasonB;
    return out;
}

StepRecord AlignPlanetChannels(ser::FloatImage& rgb, const LunarMasks& m, int threads, PlanetDispersion* result) {
    RequireMasks(rgb, m, "AlignPlanetChannels");
    PlanetDispersion d = MeasurePlanetDispersion(rgb, m, threads);
    StepRecord rec;
    rec.skipped = true;
    if (!d.reason.empty()) {
        rec.history = "Channel alignment skipped: " + d.reason + "; nothing resampled";
        if (result) *result = d;
        return rec;
    }
    const double ur = std::hypot(d.r.ux, d.r.uy), ub = std::hypot(d.b.ux, d.b.uy);
    const double mr = std::hypot(d.r.dx, d.r.dy), mb = std::hypot(d.b.dx, d.b.dy);
    rec.history = Fmt("Channel alignment (small disc): R %+.3f, %+.3f px (+-%.3f), B %+.3f, %+.3f px (+-%.3f) against G, from ",
                      d.r.dx, d.r.dy, ur, d.b.dx, d.b.dy, ub) +
                  std::to_string(d.pixels) + " px of the limb band (" + std::to_string(kLimbInner) + " px inside to " +
                  std::to_string(kLimbBand) + " px outside the disc's edge) in " + std::to_string(d.sectors) + " sectors";
    // Each channel on its own: corrected when its shift exceeds 0.1 px and 3 x its uncertainty; on a disc the frame
    // cuts, only when that uncertainty is also within kCutMaxSigma.
    auto gate = [&](double mag, double u) {
        return mag > kResampleMin && mag > kResampleSigmas * u && (!d.frameCut || u <= kCutMaxSigma);
    };
    d.r.applied = gate(mr, ur);
    d.b.applied = gate(mb, ub);
    if (result) *result = d;
    if (!d.r.applied && !d.b.applied) {
        rec.history += "; within 0.1 px or within 3 sigma of its uncertainty";
        if (d.frameCut) rec.history += Fmt(", or (the frame cuts the disc) its uncertainty over %.2f px", kCutMaxSigma);
        rec.history += ", not applied";
        return rec;
    }
    for (int c : {0, 2}) {
        const DispersionShift& s = c == 0 ? d.r : d.b;
        if (!s.applied) continue;
        std::vector<float> P = ShiftedPlane(rgb, c, s.dx, s.dy, threads);
        FillFrameEdge(P, rgb, s.dx, s.dy);
        for (size_t i = 0; i < P.size(); ++i) rgb.data[i * 3 + static_cast<size_t>(c)] = P[i];
    }
    rec.skipped = false;
    rec.history += d.r.applied && d.b.applied
                       ? "; R and B resampled onto G (Catmull-Rom)"
                       : (d.r.applied ? "; R resampled onto G (Catmull-Rom), B not" : "; B resampled onto G (Catmull-Rom), R not");
    rec.history += "; at the frame's edge, the channel's ratio to G from the nearest pixels inside";
    return rec;
}

}  // namespace planetary::stack
