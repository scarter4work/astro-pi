#include "Select.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// A quality the ranking can use: anything not a positive finite number is "no detail".
double UsableQuality(double q) { return std::isfinite(q) && q > 0 ? q : 0.0; }

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

}  // namespace

Selection SelectFrames(const std::vector<FrameStats>& stats, const ser::FileInfo& info) {
    Selection sel;
    const int64_t n = static_cast<int64_t>(stats.size());
    const double scale = LumaScale(info);
    const int lw = static_cast<int>(info.header.width / scale);
    const int lh = static_cast<int>(info.header.height / scale);
    // Fewest frames that make a stack; a short capture may lose up to half its frames.
    const int64_t need = std::max<int64_t>(1, std::min<int64_t>(kMinStackFrames, (n + 1) / 2));

    // 1. Target frames.
    std::vector<const FrameStats*> target;
    for (const FrameStats& s : stats)
        if (s.targetFound) target.push_back(&s);
    const int64_t nTarget = static_cast<int64_t>(target.size());
    if (nTarget < need) {
        const std::string msg = n == 0 ? "No target: the capture has no frames."
                                       : "No target: the frames hold only background (a target in " + Count(nTarget) +
                                             " of " + Count(n) + " frames).";
        return Diagnosed(sel, DiagnosisKind::NoTarget, msg,
                         {{"frames", static_cast<double>(n)}, {"frames with a target", static_cast<double>(nTarget)}});
    }

    // 2. Full-frame target or disc; a disc cut by the frame edge is never used.
    auto touches = [&](const FrameStats& s) { return s.bx0 <= 0 || s.by0 <= 0 || s.bx1 >= lw || s.by1 >= lh; };
    auto spansFrame = [&](const FrameStats& s) {
        return s.bx1 - s.bx0 >= kFullFrameSpan * lw || s.by1 - s.by0 >= kFullFrameSpan * lh;
    };
    int64_t filling = 0, atEdge = 0;
    for (const FrameStats* s : target) {
        if (touches(*s)) ++atEdge;
        if (touches(*s) && spansFrame(*s)) ++filling;
    }
    sel.fullFrameTarget = static_cast<double>(filling) >= kFullFrameShare * static_cast<double>(nTarget);
    std::vector<const FrameStats*> placed;
    if (sel.fullFrameTarget) {
        placed = target;
        atEdge = 0;
    } else {
        for (const FrameStats* s : target)
            if (!touches(*s)) placed.push_back(s);
        if (static_cast<int64_t>(placed.size()) < need) {
            const int64_t inside = static_cast<int64_t>(placed.size());
            return Diagnosed(sel, DiagnosisKind::TargetLeavesFrame,
                             "The target leaves the frame: it touches the frame edge in " + Count(atEdge) + " of " +
                                 Count(nTarget) + " frames with a target, so only " + Count(inside) +
                                 " show it whole; keep it centred while capturing.",
                             {{"frames", static_cast<double>(n)},
                              {"frames with a target", static_cast<double>(nTarget)},
                              {"frames at the frame edge", static_cast<double>(atEdge)},
                              {"frames fully inside", static_cast<double>(inside)}});
        }
    }

    // 3. Clipped frames.
    std::vector<const FrameStats*> left;
    std::vector<double> clipFractions;
    for (const FrameStats* s : placed) {
        clipFractions.push_back(s->clippedFraction);
        if (!(s->clippedFraction >= kClippedFrame)) left.push_back(s);
    }
    const int64_t nClipped = static_cast<int64_t>(placed.size() - left.size());
    if (static_cast<int64_t>(left.size()) < need) {
        const double median = Quantile(clipFractions, 0.5);
        return Diagnosed(sel, DiagnosisKind::Clipped,
                         "The target is saturated: " + Count(nClipped) + " of " + Count(static_cast<int64_t>(placed.size())) +
                             " frames have at least " + Num(100 * kClippedFrame, 0) +
                             "% of the target at full scale (median " + Num(100 * median, 0) +
                             "%); lower the gain or the exposure.",
                         {{"frames considered", static_cast<double>(placed.size())},
                          {"clipped frames", static_cast<double>(nClipped)},
                          {"median clipped fraction", median},
                          {"clipped limit", kClippedFrame}});
    }

    // 4. Ranking: sharpest first; ties by frame index, so the result does not depend on input order.
    std::sort(left.begin(), left.end(), [](const FrameStats* a, const FrameStats* b) {
        const double qa = UsableQuality(a->quality), qb = UsableQuality(b->quality);
        return qa != qb ? qa > qb : a->index < b->index;
    });
    const int64_t nLeft = static_cast<int64_t>(left.size());
    const int64_t refRank = std::max<int64_t>(std::min<int64_t>(3, nLeft), (nLeft + 99) / 100);
    const double qBest = UsableQuality(left.front()->quality);
    const double qRef = UsableQuality(left[static_cast<size_t>(refRank - 1)]->quality);
    if (qRef <= 0) {
        return Diagnosed(sel, DiagnosisKind::OutOfFocus,
                         "Out of focus: the sharpest frames show no detail at all (quality 0).",
                         {{"frames considered", static_cast<double>(nLeft)}, {"best quality", qBest}});
    }

    // 5. Focus and seeing, on a disc large enough to judge.
    if (!sel.fullFrameTarget) {
        const int d = DetectionBlock(lw, lh);
        std::vector<double> radii;
        for (const FrameStats* s : left)
            radii.push_back(std::sqrt(static_cast<double>(s->bx1 - s->bx0) * (s->by1 - s->by0)) / 2 - d);
        const double radius = Quantile(radii, 0.5);
        if (radius >= kMinJudgedRadius) {
            const double sqrtPi = std::sqrt(M_PI);
            const double sigmaRef = 1 / (sqrtPi * qRef * qRef * radius);
            const double rel = sigmaRef / radius;
            if (rel >= kBlurLimit) {
                std::vector<double> sigmas;
                for (const FrameStats* s : left) {
                    const double q = UsableQuality(s->quality);
                    if (q > 0) sigmas.push_back(1 / (sqrtPi * q * q * radius));
                }
                const double spread = (Quantile(sigmas, 0.9) - Quantile(sigmas, 0.1)) / Quantile(sigmas, 0.5);
                const bool steady = spread < kSteadySpread;
                const std::string blur = "blur the limb by " + Num(sigmaRef * scale, 1) + " px, " +
                                         Num(100 * rel, 0) + "% of the target's radius (usable: under " +
                                         Num(100 * kBlurLimit, 1) + "%)";
                const std::string msg =
                    steady ? "Out of focus: even the sharpest frames " + blur +
                                 ", and the blur is the same in every frame (spread " + Num(100 * spread, 0) +
                                 "%) -- the optics, not the air."
                           : "Seeing too poor: the blur changes from frame to frame (spread " + Num(100 * spread, 0) +
                                 "%), and even the sharpest frames " + blur + ".";
                return Diagnosed(sel, steady ? DiagnosisKind::OutOfFocus : DiagnosisKind::PoorSeeing, msg,
                                 {{"frames considered", static_cast<double>(nLeft)},
                                  {"limb blur (pixels)", sigmaRef * scale},
                                  {"target radius (pixels)", radius * scale},
                                  {"blur / radius", rel},
                                  {"blur limit / radius", kBlurLimit},
                                  {"blur spread", spread}});
            }
        }
    }

    // 6. Selection: the k sharpest frames maximising sum(MTF_i) / sqrt(k) at the detail frequency.
    double sum = 0, bestScore = -1;
    int64_t keep = 0;
    for (int64_t k = 1; k <= nLeft; ++k) {
        const double q = UsableQuality(left[static_cast<size_t>(k - 1)]->quality);
        if (q > 0) {
            const double r2 = std::pow(qRef / q, 4);  // (sigma_i / sigma_ref)^2
            sum += std::exp(-kDetailDepth * std::max(1.0, r2));
        }
        const double score = sum / std::sqrt(static_cast<double>(k));
        if (score > bestScore) {
            bestScore = score;
            keep = k;
        }
    }
    keep = std::max(keep, std::min(need, nLeft));

    sel.threshold = left[static_cast<size_t>(keep - 1)]->quality;
    for (int64_t k = 0; k < keep; ++k) sel.frames.push_back(left[static_cast<size_t>(k)]->index);
    std::sort(sel.frames.begin(), sel.frames.end());

    // Rounded down, so the stated bound holds for every kept frame.
    const double rel = std::floor(100 * sel.threshold / qRef) / 100;
    std::string line = "used " + Count(keep) + " of " + Count(n) + " frames: the sharpest group, quality >= " +
                       Num(rel, 2) + " of the best";
    std::vector<std::string> notUsed;
    if (n - nTarget > 0) notUsed.push_back(Count(n - nTarget) + " without a target");
    if (atEdge > 0) notUsed.push_back(Count(atEdge) + " at the frame edge");
    if (nClipped > 0) notUsed.push_back(Count(nClipped) + " clipped");
    if (!notUsed.empty()) {
        line += "; not used: ";
        for (size_t i = 0; i < notUsed.size(); ++i) line += (i ? ", " : "") + notUsed[i];
    }
    if (sel.fullFrameTarget) line += "; frame-filling target, each region ranks its own frames";
    sel.explanation = line;
    return sel;
}

}  // namespace planetary::stack
