#include "Select.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// A frame that reached the ranking: its stats and its de-noised quality.
struct Ranked {
    const FrameStats* s;
    double q;      // q^ (step 6)
    double noise;  // lambda * qualitySigma: the noise left in q^
    double qEff = 0;  // step 8
};

}  // namespace

bool FillsFrame(const FrameStats& s, int lumaWidth, int lumaHeight) {
    const bool touches = s.bx0 <= 0 || s.by0 <= 0 || s.bx1 >= lumaWidth || s.by1 >= lumaHeight;
    return s.targetFound && touches &&
           (s.bx1 - s.bx0 >= kFullFrameSpan * lumaWidth || s.by1 - s.by0 >= kFullFrameSpan * lumaHeight);
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
    if (static_cast<int64_t>(detailed.size()) < need) {
        return Diagnosed(sel, DiagnosisKind::OutOfFocus,
                         "Out of focus: only " + Count(static_cast<int64_t>(detailed.size())) + " of " +
                             Count(static_cast<int64_t>(unclipped.size())) +
                             " frames show any detail at all (the rest measure quality 0).",
                         {{"frames considered", static_cast<double>(unclipped.size())},
                          {"frames with detail", static_cast<double>(detailed.size())},
                          {"best quality", qMax}});
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
        const double robustSd = std::max(1.4826 * Quantile(dev, 0.5), Quantile(relNoise, 0.5));
        const double fence = med + kFenceSigmas * robustSd;
        int64_t above = 0;
        for (double v : lq) above += v > fence;
        const auto modeSize =
            static_cast<int64_t>(std::ceil(std::sqrt(static_cast<double>(detailed.size()))));
        for (size_t i = 0; i < detailed.size(); ++i)
            if (above >= modeSize || !(lq[i] > fence)) left.push_back(detailed[i]);
    }
    const int64_t nOutliers = static_cast<int64_t>(detailed.size() - left.size());
    const int64_t nLeft = static_cast<int64_t>(left.size());

    // 6. De-noising (empirical Bayes shrinkage toward the mean by each frame's qualitySigma).
    std::vector<Ranked> ranked;
    {
        double mu = 0, noise2 = 0;
        for (const FrameStats* s : left) {
            mu += s->quality;
            noise2 += Sigma(*s) * Sigma(*s);
        }
        mu /= static_cast<double>(nLeft);
        noise2 /= static_cast<double>(nLeft);
        double var = 0;
        for (const FrameStats* s : left) var += (s->quality - mu) * (s->quality - mu);
        var /= static_cast<double>(nLeft);
        const double tau2 = std::max(0.0, var - noise2);
        for (const FrameStats* s : left) {
            const double s2 = Sigma(*s) * Sigma(*s);
            const double lambda = tau2 + s2 > 0 ? tau2 / (tau2 + s2) : 1.0;
            ranked.push_back({s, mu + lambda * (s->quality - mu), lambda * Sigma(*s)});
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.q != b.q ? a.q > b.q : a.s->index < b.s->index; });
    const int64_t refRank = std::max<int64_t>(std::min<int64_t>(3, nLeft), (nLeft + 99) / 100);
    const Ranked ref = ranked[static_cast<size_t>(refRank - 1)];
    const double qRef = ref.q;  // > 0: a mean of positive values pulled toward a positive mean

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
            std::vector<double> sigmas;
            for (const Ranked& r : ranked) sigmas.push_back(limbBlur(r.q));
            const double mid = Quantile(sigmas, 0.5);
            const double spread = mid > 0 ? (Quantile(sigmas, 0.9) - Quantile(sigmas, 0.1)) / mid : 0.0;
            const std::string blur = "blur the limb by " + Num(sigmaRef * scale, 1) + " px, " +
                                     Num(100 * sigmaRef / radius, 0) + "% of the target's radius (usable: under " +
                                     Num(limit * scale, 1) + " px)";
            DiagnosisKind kind;
            std::string msg;
            if (nLeft < kMinJudgedFrames) {
                kind = DiagnosisKind::OutOfFocus;
                msg = "Too blurred: even the sharpest frames " + blur + " -- focus or seeing, too few frames to tell (" +
                      Count(nLeft) + ").";
            } else if (spread < kSteadySpread) {
                kind = DiagnosisKind::OutOfFocus;
                msg = "Out of focus: even the sharpest frames " + blur +
                      ", and the blur is the same in every frame (spread " + Num(100 * spread, 0) +
                      "% beyond the measurement noise) -- the optics, not the air.";
            } else {
                kind = DiagnosisKind::PoorSeeing;
                msg = "Seeing too poor: the blur changes from frame to frame (spread " + Num(100 * spread, 0) +
                      "% beyond the measurement noise), and even the sharpest frames " + blur + ".";
            }
            return Diagnosed(sel, kind, msg,
                             {{"frames judged", static_cast<double>(nLeft)},
                              {"limb blur (pixels)", sigmaRef * scale},
                              {"blur limit (pixels)", limit * scale},
                              {"target radius (pixels)", radius * scale},
                              {"blur / radius", sigmaRef / radius},
                              {"blur spread", spread}});
        }
    }

    // 8. Selection: each frame's deficit to the reference less the noise left in it, then the k
    // sharpest maximising sum(MTF_i) / sqrt(k) at the detail frequency.
    for (Ranked& r : ranked)
        r.qEff = std::min(qRef, r.q + std::sqrt(ref.noise * ref.noise + r.noise * r.noise));
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.qEff != b.qEff) return a.qEff > b.qEff;
        return a.q != b.q ? a.q > b.q : a.s->index < b.s->index;
    });
    double sum = 0, bestScore = -1;
    int64_t keep = 0;
    for (int64_t k = 1; k <= nLeft; ++k) {
        const double r2 = std::pow(qRef / ranked[static_cast<size_t>(k - 1)].qEff, 4);  // (sigma_i / sigma_ref)^2
        sum += std::exp(-kDetailDepth * std::max(1.0, r2));
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
    const double rel = std::floor(100 * sel.threshold / ref.s->quality) / 100;
    std::string line = "used " + Count(keep) + " of " + Count(n) + " frames: the sharpest group, quality >= " +
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
