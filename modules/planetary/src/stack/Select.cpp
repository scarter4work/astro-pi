#include "Select.h"

#include "Align.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
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

// 1.4826 MAD: the standard deviation of a normal sample, robust to a minority of strays.
double RobustSd(const std::vector<double>& v) {
    const double m = Quantile(v, 0.5);
    std::vector<double> dev;
    for (double x : v) dev.push_back(std::fabs(x - m));
    return 1.4826 * Quantile(dev, 0.5);
}

std::string Num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string Count(int64_t v) { return std::to_string(v); }

// "1 suspect frame", "2 suspect frames".
std::string Plural(int64_t v, const std::string& one, const std::string& many) {
    return Count(v) + " " + (v == 1 ? one : many);
}

bool HasDetail(const FrameStats& s) { return std::isfinite(s.quality) && s.quality > 0; }

double Sigma(const FrameStats& s) { return std::isfinite(s.qualitySigma) && s.qualitySigma > 0 ? s.qualitySigma : 0.0; }

// "used 612 of 4088 frames: <why>; not used: <groups>[<suspects' outcome>][<note>]".
std::string ComposeLine(int64_t used, const Selection::Line& parts, const std::vector<std::string>& notUsed,
                        const std::string& outcome) {
    std::string line = "used " + Count(used) + " of " + Count(parts.measured) + " frames: " + parts.why;
    if (!notUsed.empty()) {
        line += "; not used: ";
        for (size_t i = 0; i < notUsed.size(); ++i) line += (i ? ", " : "") + notUsed[i];
    }
    return line + outcome + parts.note;
}

Selection Diagnosed(Selection sel, DiagnosisKind kind, std::string message,
                    std::vector<std::pair<std::string, double>> numbers) {
    sel.frames.clear();
    sel.suspects.clear();
    sel.line = {};
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
    uint64_t work = 0;  // grid evaluations: the likelihood table plus two per sample and point per EM step
};

// NPMLE of G (Kiefer-Wolfowitz) on a kPriorGrid-point grid over the observed range, by EM.
Prior FitPrior(const std::vector<double>& q, const std::vector<double>& sigma) {
    Prior p;
    bool noisy = false;
    double lo = q.front(), hi = lo;
    for (size_t i = 0; i < q.size(); ++i) {
        noisy = noisy || sigma[i] > 0;
        lo = std::min(lo, q[i]);
        hi = std::max(hi, q[i]);
    }
    if (!noisy || !(hi > lo)) return p;
    const size_t k = kPriorGrid, n = q.size();
    const double h = (hi - lo) / static_cast<double>(k - 1);
    p.k = k;
    p.u.resize(k);
    p.pi.assign(k, 1.0 / static_cast<double>(k));
    for (size_t j = 0; j < k; ++j) p.u[j] = lo + h * static_cast<double>(j);
    p.lik.resize(n * k);
    for (size_t i = 0; i < n; ++i) {
        const double sg = std::max(sigma[i], h / 2);
        for (size_t j = 0; j < k; ++j) {
            const double z = (q[i] - p.u[j]) / sg;
            p.lik[i * k + j] = std::exp(-0.5 * z * z) / sg;
        }
        p.work += k;
    }
    std::vector<double> acc(k);
    for (int it = 0; it < kPriorIterations; ++it) {
        std::fill(acc.begin(), acc.end(), 0.0);
        for (size_t i = 0; i < n; ++i) {
            const double* l = &p.lik[i * k];
            double d = 0;
            for (size_t j = 0; j < k; ++j) d += p.pi[j] * l[j];
            p.work += k;
            if (!(d > 0)) continue;
            for (size_t j = 0; j < k; ++j) acc[j] += p.pi[j] * l[j] / d;
            p.work += k;
        }
        for (size_t j = 0; j < k; ++j) p.pi[j] = acc[j] / static_cast<double>(n);
    }
    return p;
}

// A sample that reached the ranking: its posterior mean quality and expected detail contrast.
struct Ranked {
    size_t pos;   // position in the input
    double mean;  // posterior mean of its true quality
    double mtf;   // posterior expectation of MTF (step 8)
};

}  // namespace

DetailRanking RankByDetail(const std::vector<double>& quality, const std::vector<double>& sigma, int64_t need,
                           int64_t floorCount, bool applyFence) {
    if (sigma.size() != quality.size())
        throw std::invalid_argument("RankByDetail: " + std::to_string(sigma.size()) + " sigmas for " +
                                    std::to_string(quality.size()) + " qualities");
    for (double q : quality)
        if (!(std::isfinite(q) && q > 0)) throw std::invalid_argument("RankByDetail: a quality is not finite and > 0");
    DetailRanking out;
    const size_t nd = quality.size();
    if (nd == 0) return out;
    std::vector<double> sg(nd);
    for (size_t i = 0; i < nd; ++i) sg[i] = std::isfinite(sigma[i]) && sigma[i] > 0 ? sigma[i] : 0.0;

    // 5. Upper fence on log quality.
    std::vector<size_t> left;
    {
        std::vector<double> lq, relNoise;
        for (size_t i = 0; i < nd; ++i) {
            lq.push_back(std::log(quality[i]));
            relNoise.push_back(sg[i] / quality[i]);
        }
        const double med = Quantile(lq, 0.5);
        std::vector<double> dev;
        for (double v : lq) dev.push_back(std::fabs(v - med));
        const double robustSd =
            std::max({1.4826 * Quantile(dev, 0.5), Quantile(relNoise, 0.5), kFenceMinLogSd});
        const double fence = med + kFenceSigmas * robustSd;
        out.fence = std::exp(fence);
        // Samples above the fence, sharpest first; the fenced ones are those above the lowest gap of
        // at least kFenceGapSigmas robust sd among them (a glitch stands apart; the top of a real mode
        // or tail is continuous and has no such gap).
        std::vector<size_t> order;
        for (size_t i = 0; i < lq.size(); ++i)
            if (lq[i] > fence) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lq[a] != lq[b] ? lq[a] > lq[b] : a < b; });
        const auto above = static_cast<int64_t>(order.size());
        const auto nDetailed = static_cast<int64_t>(nd);
        const auto modeSize = static_cast<int64_t>(std::ceil(std::sqrt(static_cast<double>(nDetailed))));
        int64_t cut = 0;  // fence off the `cut` sharpest samples
        if (applyFence && above > 0 && nDetailed >= kMinFenceFrames && above < modeSize && nDetailed - above >= floorCount) {
            double below = -std::numeric_limits<double>::infinity();  // sharpest sample under the fence
            int64_t band = 0;  // samples in the kFenceBandSigmas robust sd just under the fence
            for (double v : lq)
                if (!(v > fence)) {
                    below = std::max(below, v);
                    band += v > fence - kFenceBandSigmas * robustSd;
                }
            // Not the top of a population declining through the fence: every candidate goes.
            if (band < above) {
                cut = above;
                out.fenceRule = FenceRule::Band;
                out.fenceBand = band;
            }
            for (int64_t k = above; k >= 1 && cut == 0; --k) {
                const double next = k < above ? lq[order[static_cast<size_t>(k)]] : below;
                if (lq[order[static_cast<size_t>(k - 1)]] - next >= kFenceGapSigmas * robustSd) {
                    cut = k;
                    out.fenceRule = FenceRule::Gap;
                    break;
                }
            }
        }
        std::vector<bool> fenced(lq.size(), false);
        for (int64_t k = 0; k < cut; ++k) fenced[order[static_cast<size_t>(k)]] = true;
        for (size_t i = 0; i < nd; ++i)
            (fenced[i] ? out.fenced : left).push_back(i);
    }
    out.outliers = nd - left.size();
    out.left = left;
    const int64_t nLeft = static_cast<int64_t>(left.size());

    // 6. Deconvolution (Ruling P5): the distribution of the true quality, and each sample's posterior.
    std::vector<double> qLeft, sLeft;
    for (size_t i : left) {
        qLeft.push_back(quality[i]);
        sLeft.push_back(sg[i]);
    }
    const Prior prior = FitPrior(qLeft, sLeft);
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
        std::vector<double> qs = qLeft;
        std::nth_element(qs.begin(), qs.begin() + (refRank - 1), qs.end(), std::greater<double>());
        qRef = qs[static_cast<size_t>(refRank - 1)];
    }
    out.qRef = qRef;
    out.work = prior.work;

    // 8's weight: contrast at the detail frequency of a sample of true quality q.
    auto mtfAt = [&](double q) {
        return q > 0 ? std::exp(-kDetailDepth * std::max(1.0, std::pow(qRef / q, 4))) : 0.0;
    };
    std::vector<Ranked> ranked;
    for (size_t i = 0; i < left.size(); ++i) {
        const size_t pos = left[i];
        if (prior.k == 0 || sg[pos] <= 0) {
            ranked.push_back({pos, quality[pos], mtfAt(quality[pos])});
            continue;
        }
        double d = 0, m = 0, w = 0;
        for (size_t j = 0; j < prior.k; ++j) {
            const double p = prior.pi[j] * prior.lik[i * prior.k + j];
            d += p;
            m += p * prior.u[j];
            w += p * mtfAt(prior.u[j]);
        }
        out.work += prior.k;
        ranked.push_back(d > 0 ? Ranked{pos, m / d, w / d} : Ranked{pos, quality[pos], mtfAt(quality[pos])});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.mtf != b.mtf) return a.mtf > b.mtf;
        return a.mean != b.mean ? a.mean > b.mean : a.pos < b.pos;
    });
    for (const Ranked& r : ranked) {
        out.ranked.push_back(r.pos);
        out.mean.push_back(r.mean);
        out.mtf.push_back(r.mtf);
    }

    // 8. The k samples with the highest expected contrast maximising sum / sqrt(k).
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
    out.keep = static_cast<size_t>(std::max(keep, std::min(need, nLeft)));
    return out;
}

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
    std::vector<double> centreX, centreY;  // the frame-filling boxes' centres, in frame widths / heights
    for (const FrameStats* s : target) {
        if (touches(*s)) ++atEdge;
        if (FillsFrame(*s, lw, lh)) {
            ++filling;
            centreX.push_back((s->bx0 + s->bx1) / (2.0 * lw));
            centreY.push_back((s->by0 + s->by1) / (2.0 * lh));
        }
    }
    const bool fillsShare = static_cast<double>(filling) >= kFullFrameShare * static_cast<double>(nTarget);
    const bool staysPut = filling > 0 && RobustSd(centreX) <= kFullFrameMaxDrift && RobustSd(centreY) <= kFullFrameMaxDrift;
    const bool present = static_cast<double>(nTarget) >= kFullFramePresence * static_cast<double>(n);
    sel.fullFrameTarget = fillsShare && staysPut && present;
    if (fillsShare && staysPut && !present) {
        // A surface that stays put, seen too rarely: it is not leaving the frame, it is hidden.
        return Diagnosed(sel, DiagnosisKind::NoTarget,
                         "Too few frames show the target: a frame-filling target is seen in only " + Count(nTarget) +
                             " of " + Count(n) + " frames (cloud or gaps?); at least " +
                             Num(100 * kFullFramePresence, 0) + "% of the frames are needed.",
                         {{"frames", static_cast<double>(n)},
                          {"frames with a target", static_cast<double>(nTarget)},
                          {"stray detections", static_cast<double>(nStray)},
                          {"presence needed", kFullFramePresence}});
    }
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

    // 5, 6 and 8's ranking (RankByDetail): the fence, the deconvolution and each frame's expected
    // contrast; `detailed` is in frame order, so its positions break ties as frame indices did.
    std::vector<double> dq, ds;
    for (const FrameStats* s : detailed) {
        dq.push_back(s->quality);
        ds.push_back(Sigma(*s));
    }
    const DetailRanking rank = RankByDetail(dq, ds, need, floorCount);
    std::vector<const FrameStats*> left;
    for (size_t i : rank.left) left.push_back(detailed[i]);
    // 5. The frames RankByDetail fenced off are suspects (Ruling P6): they cannot be told from
    // glitches by quality, so they are held out of the ranking until alignment decides (AdmitSuspects).
    if (!rank.fenced.empty()) {
        const std::string why =
            rank.fenceRule == FenceRule::Band
                ? "too few frames just under the fence (" + Count(rank.fenceBand) + " within " +
                      Num(kFenceBandSigmas, 0) + " robust sd) to be the top of the capture's own spread"
                : "standing apart from the rest by a gap of at least " + Num(kFenceGapSigmas, 0) + " robust sd";
        const std::string fenceText = Num(rank.fence, 4);
        for (size_t i : rank.fenced)
            sel.suspects.push_back({detailed[i]->index, detailed[i]->quality,
                                    "quality " + Num(detailed[i]->quality, 4) + " above the fence " + fenceText + ", " + why});
    }
    const int64_t nSuspects = static_cast<int64_t>(sel.suspects.size());  // ascending: `detailed` is in frame order
    const int64_t nLeft = static_cast<int64_t>(left.size());
    const double qRef = rank.qRef;

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

    // 7. Focus and seeing, on a disc.
    if (!sel.fullFrameTarget) {
        sel.focusJudged = true;
        const int d = DetectionBlock(lw, lh);
        std::vector<double> radii, halfRadii;
        for (size_t i : rank.ranked) {
            const FrameStats* s = detailed[i];
            radii.push_back(std::sqrt(static_cast<double>(s->bx1 - s->bx0) * (s->by1 - s->by0)) / 2 - d);
            halfRadii.push_back(s->halfRadius);
        }
        // The size class and a large disc's judgement go by the half-contrast radius: the box grows with
        // the blur being judged (a small disc blurred far enough would otherwise turn "large").
        const double boxRadius = std::max(1.0, Quantile(radii, 0.5));
        const double halfRadius = std::max(1.0, Quantile(halfRadii, 0.5));
        sel.targetRadius = halfRadius;
        const bool small = halfRadius < kMinJudgedRadius;
        const double radius = small ? boxRadius : halfRadius;
        const double sqrtPi = std::sqrt(M_PI);
        auto limbBlur = [&](double q) {
            const double sigma = 1 / (sqrtPi * q * q * radius);
            return small ? std::sqrt(std::max(0.0, sigma * sigma - kSamplingBlur * kSamplingBlur)) : sigma;
        };
        const double sigmaRef = limbBlur(qRef);
        const double limit = small ? std::max(kBlurLimit * radius, kBlurFloorPixels / scale) : kGrossBlurLimit * radius;
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
    const auto keep = static_cast<int64_t>(rank.keep);
    sel.threshold = detailed[rank.ranked.front()]->quality;
    for (int64_t k = 0; k < keep; ++k) {
        const FrameStats* s = detailed[rank.ranked[static_cast<size_t>(k)]];
        sel.frames.push_back(s->index);
        sel.threshold = std::min(sel.threshold, s->quality);
    }
    std::sort(sel.frames.begin(), sel.frames.end());

    // Rounded down, so the stated bound holds for every kept frame.
    const double rel = std::floor(100 * sel.threshold / qRef) / 100;
    Selection::Line& parts = sel.line;
    parts.measured = n;
    parts.why = "the frames judged sharpest after allowing for measurement noise, quality >= " + Num(rel, 2) + " of the best";
    if (n - nFound > 0)
        parts.notUsed.push_back(Count(n - nFound) + " without a target" + (sel.fullFrameTarget ? " (cloud or gaps?)" : ""));
    if (nStray > 0) parts.notUsed.push_back(Plural(nStray, "stray detection", "stray detections"));
    if (atEdge > 0) parts.notUsed.push_back(Count(atEdge) + " at the frame edge");
    if (nClipped > 0) parts.notUsed.push_back(Count(nClipped) + " clipped");
    if (nNoDetail > 0) parts.notUsed.push_back(Count(nNoDetail) + " without detail");
    if (nLeft - keep > 0) parts.notUsed.push_back(Count(nLeft - keep) + " below the cut");
    if (sel.fullFrameTarget) parts.note = "; frame-filling target, each region ranks its own frames";
    std::vector<std::string> notUsed = parts.notUsed;
    if (nSuspects > 0) notUsed.push_back(Plural(nSuspects, "suspect frame", "suspect frames") + " (alignment decides)");
    sel.explanation = ComposeLine(keep, parts, notUsed, "");
    return sel;
}

Admission AdmitSuspects(const Selection& selection, const std::vector<Shift>& suspectShifts, double alignCut) {
    // AlignmentCut never returns less than the floor; below it (or NaN) a torn frame could pass.
    if (!(alignCut >= kMinAlignConfidence))
        throw std::invalid_argument("AdmitSuspects: alignCut " + std::to_string(alignCut) +
                                    " is not >= kMinAlignConfidence (" + std::to_string(kMinAlignConfidence) + ")");
    if (suspectShifts.size() != selection.suspects.size())
        throw std::invalid_argument("AdmitSuspects: " + std::to_string(suspectShifts.size()) + " shifts for " +
                                    std::to_string(selection.suspects.size()) + " suspect frames");
    Admission a;
    a.frames = selection.frames;
    if (selection.suspects.empty()) {
        a.explanation = selection.explanation;
        return a;
    }
    for (size_t i = 0; i < selection.suspects.size(); ++i) {
        // `>=` is false for a NaN confidence: such a frame is rejected.
        if (suspectShifts[i].confidence >= alignCut) a.admitted.push_back(selection.suspects[i].index);
        else a.rejected.push_back(selection.suspects[i].index);
    }
    std::sort(a.admitted.begin(), a.admitted.end());
    std::sort(a.rejected.begin(), a.rejected.end());
    a.frames.insert(a.frames.end(), a.admitted.begin(), a.admitted.end());
    std::sort(a.frames.begin(), a.frames.end());
    std::vector<std::string> notUsed = selection.line.notUsed;
    const auto nRejected = static_cast<int64_t>(a.rejected.size());
    if (nRejected > 0) notUsed.push_back(Count(nRejected) + " rejected by alignment");
    const auto nSuspects = static_cast<int64_t>(selection.suspects.size());
    a.explanation = ComposeLine(static_cast<int64_t>(a.frames.size()), selection.line, notUsed,
                                "; " + Plural(nSuspects, "suspect frame", "suspect frames") + " (sharper than the rest): " +
                                    Count(static_cast<int64_t>(a.admitted.size())) + " admitted after alignment, " +
                                    Count(nRejected) + " rejected");
    return a;
}

}  // namespace planetary::stack
