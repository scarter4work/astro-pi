#include "Select.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace planetary::stack {

namespace {

// Linear-interpolated quantile of v (copied, so the caller's order is kept); v must not be empty.
double Quantile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (pos - static_cast<double>(lo)) * (v[hi] - v[lo]);
}

std::string Num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string Count(int64_t v) { return std::to_string(v); }

// "1 outlier", "2 outliers".
std::string Plural(int64_t v, const std::string& one, const std::string& many) {
    return Count(v) + " " + (v == 1 ? one : many);
}

bool HasDetail(const FrameStats& s) { return std::isfinite(s.quality) && s.quality > 0; }

double Sigma(const FrameStats& s) { return std::isfinite(s.qualitySigma) && s.qualitySigma > 0 ? s.qualitySigma : 0.0; }

Selection Diagnosed(Selection sel, DiagnosisKind kind, std::string message,
                    std::vector<std::pair<std::string, double>> numbers) {
    sel.frames.clear();
    sel.threshold = 0;
    sel.diagnosis.kind = kind;
    sel.diagnosis.message = std::move(message);
    sel.diagnosis.numbers = std::move(numbers);
    sel.explanation = sel.diagnosis.message;
    return sel;
}

// The deconvolved quality distribution G of step 6: weights pi on the grid u, and each frame's
// likelihood row N(q_i; u_k, s_i), s_i = max(qualitySigma_i, h / 2). k == 0 when no frame reports
// noise (every quality is then exact).
struct Prior {
    std::vector<double> u, pi, lik;
    size_t k = 0;
};

// NPMLE of G (Kiefer-Wolfowitz) on a kPriorGrid-point grid over the observed range, by EM.
Prior FitPrior(const std::vector<const FrameStats*>& f) {
    Prior p;
    bool noisy = false;
    double lo = f.front()->quality, hi = lo;
    for (const FrameStats* s : f) {
        noisy = noisy || Sigma(*s) > 0;
        lo = std::min(lo, s->quality);
        hi = std::max(hi, s->quality);
    }
    if (!noisy || !(hi > lo)) return p;
    const size_t k = kPriorGrid, n = f.size();
    const double h = (hi - lo) / static_cast<double>(k - 1);
    p.k = k;
    p.u.resize(k);
    p.pi.assign(k, 1.0 / static_cast<double>(k));
    for (size_t j = 0; j < k; ++j) p.u[j] = lo + h * static_cast<double>(j);
    p.lik.resize(n * k);
    for (size_t i = 0; i < n; ++i) {
        const double sg = std::max(Sigma(*f[i]), h / 2);
        for (size_t j = 0; j < k; ++j) {
            const double z = (f[i]->quality - p.u[j]) / sg;
            p.lik[i * k + j] = std::exp(-0.5 * z * z) / sg;
        }
    }
    std::vector<double> acc(k);
    for (int it = 0; it < kPriorIterations; ++it) {
        std::fill(acc.begin(), acc.end(), 0.0);
        for (size_t i = 0; i < n; ++i) {
            const double* l = &p.lik[i * k];
            double d = 0;
            for (size_t j = 0; j < k; ++j) d += p.pi[j] * l[j];
            if (!(d > 0)) continue;
            for (size_t j = 0; j < k; ++j) acc[j] += p.pi[j] * l[j] / d;
        }
        for (size_t j = 0; j < k; ++j) p.pi[j] = acc[j] / static_cast<double>(n);
    }
    return p;
}

// A frame that reached the ranking: its posterior mean quality and expected detail contrast.
struct Ranked {
    const FrameStats* s;
    double mean;  // posterior mean of its true quality
    double mtf;   // posterior expectation of MTF (step 8)
};

}  // namespace

bool FillsFrame(const FrameStats& s, int lumaWidth, int lumaHeight) {
    const bool touches = s.bx0 <= 0 || s.by0 <= 0 || s.bx1 >= lumaWidth || s.by1 >= lumaHeight;
    const double fw = (s.bx1 - s.bx0) / static_cast<double>(lumaWidth);
    const double fh = (s.by1 - s.by0) / static_cast<double>(lumaHeight);
    return s.targetFound && touches && std::max(fw, fh) >= kFullFrameSpan && std::min(fw, fh) >= kFullFrameMinSide;
}

Selection SelectFrames(const std::vector<FrameStats>& stats, const ser::FileInfo& info) {
    Selection sel;
    const int64_t n = static_cast<int64_t>(stats.size());
    const double scale = LumaScale(info);
    const int lw = static_cast<int>(info.header.width / scale);
    const int lh = static_cast<int>(info.header.height / scale);
    const int64_t need = std::max<int64_t>(1, std::min<int64_t>(kMinStackFrames, (n + 1) / 2));
    const int64_t floorCount =
        std::max<int64_t>(need, static_cast<int64_t>(std::ceil(std::sqrt(static_cast<double>(n)))));

    // Frame order, whatever the input order: every sum below is then the same for any order.
    std::vector<const FrameStats*> all;
    for (const FrameStats& s : stats) all.push_back(&s);
    std::sort(all.begin(), all.end(), [](const FrameStats* a, const FrameStats* b) { return a->index < b->index; });

    // 1. Target frames, consistent in size.
    std::vector<const FrameStats*> found;
    for (const FrameStats* s : all)
        if (s->targetFound) found.push_back(s);
    std::vector<const FrameStats*> target;
    if (!found.empty()) {
        std::vector<double> ws, hs;
        for (const FrameStats* s : found) {
            ws.push_back(s->bx1 - s->bx0);
            hs.push_back(s->by1 - s->by0);
        }
        const double mw = Quantile(ws, 0.5), mh = Quantile(hs, 0.5);
        auto within = [](double v, double m) { return v >= m / kBoxTolerance && v <= m * kBoxTolerance; };
        for (const FrameStats* s : found)
            if (within(s->bx1 - s->bx0, mw) && within(s->by1 - s->by0, mh)) target.push_back(s);
    }
    const int64_t nFound = static_cast<int64_t>(found.size());
    const int64_t nTarget = static_cast<int64_t>(target.size());
    const int64_t nStray = nFound - nTarget;
    if (nTarget < floorCount) {
        std::string msg = n == 0 ? "No target: the capture has no frames."
                                 : "No target: the frames hold only background (a target in " + Count(nTarget) + " of " +
                                       Count(n) + " frames";
        if (n > 0) msg += nStray > 0 ? ", plus " + Plural(nStray, "stray detection", "stray detections") + ")." : ").";
        return Diagnosed(sel, DiagnosisKind::NoTarget, msg,
                         {{"frames", static_cast<double>(n)},
                          {"frames with a target", static_cast<double>(nTarget)},
                          {"stray detections", static_cast<double>(nStray)},
                          {"frames needed", static_cast<double>(floorCount)}});
    }

    // 2. Full-frame target or disc; a disc cut by the frame edge is never used.
    auto touches = [&](const FrameStats& s) { return s.bx0 <= 0 || s.by0 <= 0 || s.bx1 >= lw || s.by1 >= lh; };
    int64_t filling = 0, atEdge = 0;
    for (const FrameStats* s : target) {
        if (touches(*s)) ++atEdge;
        if (FillsFrame(*s, lw, lh)) ++filling;
    }
    sel.fullFrameTarget = static_cast<double>(filling) >= kFullFrameShare * static_cast<double>(nTarget);
    std::vector<const FrameStats*> placed;
    if (sel.fullFrameTarget) {
        placed = target;
        atEdge = 0;
    } else {
        for (const FrameStats* s : target)
            if (!touches(*s)) placed.push_back(s);
        const int64_t inside = static_cast<int64_t>(placed.size());
        if (inside < floorCount) {
            return Diagnosed(sel, DiagnosisKind::TargetLeavesFrame,
                             "The target leaves the frame: it touches the frame edge in " + Count(atEdge) + " of " +
                                 Count(nTarget) + " frames with a target, so only " + Count(inside) +
                                 " show it whole; keep it centred while capturing.",
                             {{"frames", static_cast<double>(n)},
                              {"frames with a target", static_cast<double>(nTarget)},
                              {"frames at the frame edge", static_cast<double>(atEdge)},
                              {"frames fully inside", static_cast<double>(inside)},
                              {"frames needed", static_cast<double>(floorCount)}});
        }
    }

    // 3. Clipped frames.
    std::vector<const FrameStats*> unclipped;
    std::vector<double> clipFractions;
    for (const FrameStats* s : placed) {
        clipFractions.push_back(s->clippedFraction);
        if (!(s->clippedFraction >= kClippedFrame)) unclipped.push_back(s);
    }
    const int64_t nClipped = static_cast<int64_t>(placed.size() - unclipped.size());
    if (static_cast<int64_t>(unclipped.size()) < floorCount) {
        const double median = Quantile(clipFractions, 0.5);
        return Diagnosed(sel, DiagnosisKind::Clipped,
                         "The target is saturated: " + Count(nClipped) + " of " +
                             Count(static_cast<int64_t>(placed.size())) + " frames have at least " +
                             Num(100 * kClippedFrame, 0) + "% of the target at full scale (median " +
                             Num(100 * median, 0) + "%); lower the gain or the exposure.",
                         {{"frames considered", static_cast<double>(placed.size())},
                          {"clipped frames", static_cast<double>(nClipped)},
                          {"median clipped fraction", median},
                          {"clipped limit", kClippedFrame},
                          {"frames needed", static_cast<double>(floorCount)}});
    }

    // 4. Frames without detail.
    std::vector<const FrameStats*> detailed;
    double qMax = 0;
    for (const FrameStats* s : unclipped)
        if (HasDetail(*s)) {
            detailed.push_back(s);
            qMax = std::max(qMax, s->quality);
        }
    const int64_t nNoDetail = static_cast<int64_t>(unclipped.size() - detailed.size());
    if (static_cast<int64_t>(detailed.size()) < floorCount) {
        return Diagnosed(sel, DiagnosisKind::OutOfFocus,
                         "Out of focus: only " + Count(static_cast<int64_t>(detailed.size())) + " of " +
                             Count(static_cast<int64_t>(unclipped.size())) +
                             " frames show any detail at all (the rest measure quality 0).",
                         {{"frames considered", static_cast<double>(unclipped.size())},
                          {"frames with detail", static_cast<double>(detailed.size())},
                          {"best quality", qMax},
                          {"frames needed", static_cast<double>(floorCount)}});
    }

    // 5. Upper fence on log quality.
    std::vector<const FrameStats*> left;
    {
        std::vector<double> lq, relNoise;
        for (const FrameStats* s : detailed) {
            lq.push_back(std::log(s->quality));
            relNoise.push_back(Sigma(*s) / s->quality);
        }
        const double med = Quantile(lq, 0.5);
        std::vector<double> dev;
        for (double v : lq) dev.push_back(std::fabs(v - med));
        const double robustSd =
            std::max({1.4826 * Quantile(dev, 0.5), Quantile(relNoise, 0.5), kFenceMinLogSd});
        const double fence = med + kFenceSigmas * robustSd;
        // Frames above the fence, sharpest first; the outliers are those above the lowest gap of at
        // least kFenceGapSigmas robust sd among them (a glitch stands apart; the top of a real mode
        // or tail is continuous and has no such gap).
        std::vector<size_t> order;
        for (size_t i = 0; i < lq.size(); ++i)
            if (lq[i] > fence) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lq[a] != lq[b] ? lq[a] > lq[b] : a < b; });
        const auto above = static_cast<int64_t>(order.size());
        const auto nDetailed = static_cast<int64_t>(detailed.size());
        const auto modeSize = static_cast<int64_t>(std::ceil(std::sqrt(static_cast<double>(nDetailed))));
        int64_t cut = 0;  // fence off the `cut` sharpest frames
        if (above > 0 && nDetailed >= kMinFenceFrames && above < modeSize && nDetailed - above >= floorCount) {
            double below = -std::numeric_limits<double>::infinity();  // sharpest frame under the fence
            for (double v : lq)
                if (!(v > fence)) below = std::max(below, v);
            for (int64_t k = above; k >= 1; --k) {
                const double next = k < above ? lq[order[static_cast<size_t>(k)]] : below;
                if (lq[order[static_cast<size_t>(k - 1)]] - next >= kFenceGapSigmas * robustSd) {
                    cut = k;
                    break;
                }
            }
        }
        std::vector<bool> out(lq.size(), false);
        for (int64_t k = 0; k < cut; ++k) out[order[static_cast<size_t>(k)]] = true;
        for (size_t i = 0; i < detailed.size(); ++i)
            if (!out[i]) left.push_back(detailed[i]);
    }
    const int64_t nOutliers = static_cast<int64_t>(detailed.size() - left.size());
    const int64_t nLeft = static_cast<int64_t>(left.size());

    // 6. Deconvolution (Ruling P5): the distribution of the true quality, and each frame's posterior.
    const Prior prior = FitPrior(left);
    const int64_t refRank = std::max<int64_t>(std::min<int64_t>(3, nLeft), (nLeft + 99) / 100);
    double qRef = 0;
    if (prior.k > 0) {
        // The quality the sharpest refRank / N of the TRUE distribution reach (no winner's curse).
        double mass = 0;
        const double topShare = static_cast<double>(refRank) / static_cast<double>(nLeft);
        for (size_t j = prior.k; j-- > 0;) {
            mass += prior.pi[j];
            qRef = prior.u[j];
            if (mass >= topShare) break;
        }
    } else {
        std::vector<double> qs;
        for (const FrameStats* s : left) qs.push_back(s->quality);
        std::nth_element(qs.begin(), qs.begin() + (refRank - 1), qs.end(), std::greater<double>());
        qRef = qs[static_cast<size_t>(refRank - 1)];
    }
    // Moments: mu, and tau^2 = the variance the noise does not explain (step 7's spread).
    double mu = 0, noise2 = 0, var = 0;
    for (const FrameStats* s : left) {
        mu += s->quality;
        noise2 += Sigma(*s) * Sigma(*s);
    }
    mu /= static_cast<double>(nLeft);
    noise2 /= static_cast<double>(nLeft);
    for (const FrameStats* s : left) var += (s->quality - mu) * (s->quality - mu);
    var /= static_cast<double>(nLeft);
    const double tau = std::sqrt(std::max(0.0, var - noise2));
    // tau^2 is an estimate: its standard error ~ sqrt(2 / N) var (sample variance, normal data).
    // Two standard errors up is the largest true spread the data cannot rule out.
    const double seTau2 = std::sqrt(2.0 / static_cast<double>(nLeft)) * var;
    const double tauHi = std::sqrt(std::max(0.0, var - noise2 + 2 * seTau2));
    const double tauLo = std::sqrt(std::max(0.0, var - noise2 - 2 * seTau2));

    // 8's weight: contrast at the detail frequency of a frame of true quality q.
    auto mtfAt = [&](double q) {
        return q > 0 ? std::exp(-kDetailDepth * std::max(1.0, std::pow(qRef / q, 4))) : 0.0;
    };
    std::vector<Ranked> ranked;
    for (size_t i = 0; i < left.size(); ++i) {
        const FrameStats* s = left[i];
        if (prior.k == 0 || Sigma(*s) <= 0) {
            ranked.push_back({s, s->quality, mtfAt(s->quality)});
            continue;
        }
        double d = 0, m = 0, w = 0;
        for (size_t j = 0; j < prior.k; ++j) {
            const double p = prior.pi[j] * prior.lik[i * prior.k + j];
            d += p;
            m += p * prior.u[j];
            w += p * mtfAt(prior.u[j]);
        }
        ranked.push_back(d > 0 ? Ranked{s, m / d, w / d} : Ranked{s, s->quality, mtfAt(s->quality)});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.mtf != b.mtf) return a.mtf > b.mtf;
        return a.mean != b.mean ? a.mean > b.mean : a.s->index < b.s->index;
    });

    // 7. Focus and seeing, on a disc.
    if (!sel.fullFrameTarget) {
        sel.focusJudged = true;
        const int d = DetectionBlock(lw, lh);
        std::vector<double> radii;
        for (const Ranked& r : ranked)
            radii.push_back(std::sqrt(static_cast<double>(r.s->bx1 - r.s->bx0) * (r.s->by1 - r.s->by0)) / 2 - d);
        const double radius = std::max(1.0, Quantile(radii, 0.5));
        const bool small = radius < kMinJudgedRadius;
        const double sqrtPi = std::sqrt(M_PI);
        auto limbBlur = [&](double q) {
            const double sigma = 1 / (sqrtPi * q * q * radius);
            return small ? std::sqrt(std::max(0.0, sigma * sigma - kSamplingBlur * kSamplingBlur)) : sigma;
        };
        const double sigmaRef = limbBlur(qRef);
        const double limit = small ? std::max(kBlurLimit * radius, kBlurFloorPixels / scale) : kBlurLimit * radius;
        if (sigmaRef >= limit) {
            // The spread of the true blur: q at mu -+ 1.2816 tau (p10 / p90 of a normal), not the
            // posterior means (those are pulled together by the noise); tauHi gives the upper bound.
            const double z = 1.2816;
            auto spreadOf = [&](double t) {
                return (limbBlur(std::max(mu - z * t, 0.01 * mu)) - limbBlur(mu + z * t)) / limbBlur(mu);
            };
            const double spread = spreadOf(tau), spreadHi = spreadOf(tauHi), spreadLo = spreadOf(tauLo);
            const std::string blur = "blur the limb by " + Num(sigmaRef * scale, 1) + " px, " +
                                     Num(100 * sigmaRef / radius, 0) + "% of the target's radius (usable: under " +
                                     Num(limit * scale, 1) + " px)";
            DiagnosisKind kind;
            std::string msg;
            if (nLeft < kMinJudgedFrames) {
                kind = DiagnosisKind::OutOfFocus;
                msg = "Too blurred: even the sharpest frames " + blur + " -- focus or seeing, too few frames to tell (" +
                      Count(nLeft) + ").";
            } else if (spreadHi < kSteadySpread) {
                kind = DiagnosisKind::OutOfFocus;
                msg = "Out of focus: even the sharpest frames " + blur +
                      ", and the blur is the same in every frame (spread " + Num(100 * spread, 0) +
                      "% beyond the measurement noise, at most " + Num(100 * spreadHi, 0) +
                      "%) -- the optics, not the air.";
            } else if (spreadLo >= kSteadySpread) {
                kind = DiagnosisKind::PoorSeeing;
                msg = "Seeing too poor: the blur changes from frame to frame (spread " + Num(100 * spread, 0) +
                      "% beyond the measurement noise, at least " + Num(100 * spreadLo, 0) +
                      "%), and even the sharpest frames " + blur + ".";
            } else {
                kind = DiagnosisKind::OutOfFocus;
                msg = "Too blurred: even the sharpest frames " + blur +
                      " -- focus or seeing, the measurement noise is too large to tell (spread " +
                      Num(100 * spreadLo, 0) + "-" + Num(100 * spreadHi, 0) + "%).";
            }
            return Diagnosed(sel, kind, msg,
                             {{"frames judged", static_cast<double>(nLeft)},
                              {"limb blur (pixels)", sigmaRef * scale},
                              {"blur limit (pixels)", limit * scale},
                              {"target radius (pixels)", radius * scale},
                              {"blur / radius", sigmaRef / radius},
                              {"blur spread", spread},
                              {"blur spread lower bound", spreadLo},
                              {"blur spread upper bound", spreadHi}});
        }
    }

    // 8. Selection: the k frames with the highest expected contrast maximising sum / sqrt(k).
    double sum = 0, bestScore = -1;
    int64_t keep = 0;
    for (int64_t k = 1; k <= nLeft; ++k) {
        sum += ranked[static_cast<size_t>(k - 1)].mtf;
        const double score = sum / std::sqrt(static_cast<double>(k));
        if (score > bestScore) {
            bestScore = score;
            keep = k;
        }
    }
    keep = std::max(keep, std::min(need, nLeft));

    sel.threshold = ranked.front().s->quality;
    for (int64_t k = 0; k < keep; ++k) {
        const FrameStats* s = ranked[static_cast<size_t>(k)].s;
        sel.frames.push_back(s->index);
        sel.threshold = std::min(sel.threshold, s->quality);
    }
    std::sort(sel.frames.begin(), sel.frames.end());

    // Rounded down, so the stated bound holds for every kept frame.
    const double rel = std::floor(100 * sel.threshold / qRef) / 100;
    std::string line = "used " + Count(keep) + " of " + Count(n) + " frames: the frames judged sharpest after allowing for measurement noise, quality >= " +
                       Num(rel, 2) + " of the best";
    std::vector<std::string> notUsed;
    if (n - nFound > 0) notUsed.push_back(Count(n - nFound) + " without a target");
    if (nStray > 0) notUsed.push_back(Plural(nStray, "stray detection", "stray detections"));
    if (atEdge > 0) notUsed.push_back(Count(atEdge) + " at the frame edge");
    if (nClipped > 0) notUsed.push_back(Count(nClipped) + " clipped");
    if (nNoDetail > 0) notUsed.push_back(Count(nNoDetail) + " without detail");
    if (nOutliers > 0) notUsed.push_back(Plural(nOutliers, "outlier", "outliers"));
    if (nLeft - keep > 0) notUsed.push_back(Count(nLeft - keep) + " below the cut");
    if (!notUsed.empty()) {
        line += "; not used: ";
        for (size_t i = 0; i < notUsed.size(); ++i) line += (i ? ", " : "") + notUsed[i];
    }
    if (sel.fullFrameTarget) line += "; frame-filling target, each region ranks its own frames";
    sel.explanation = line;
    return sel;
}

}  // namespace planetary::stack
