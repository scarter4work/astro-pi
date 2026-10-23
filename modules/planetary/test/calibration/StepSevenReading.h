// Test-only (test/calibration/): step-7 readings for a disc capture, replicating SelectFrames' steps 1-4 filters
// and step 6's ranking, with intermediate values Selection does not expose (G's mean and sd, tauLo). Not the
// shipped reading, which is Selection::focusReading (seedsel and mdq print it): X is sigma(q_ref) / R_h with no
// narrowing; Xg is Ruling P26 fix round 1's gross reading -- tauLo in quality, no SNR gate -- where Select.cpp
// now takes tauLo in quality^2 when every frame carries quality2Sigma and gates the narrowing by the frames'
// SNR (fix round 2). Kept for the figures measured with it at round 1.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "Select.h"

struct Reading {
    bool ok = false;
    double qRef = 0, rh = 0, rbox = 0, X = 0, S = 0, Xg = 0, muG = 0, sdG = 0, tauLo = 0;
    int64_t detailed = 0;
};

inline double Med(std::vector<double> v) {
    const size_t k = static_cast<size_t>(0.5 * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

inline Reading ReadingOf(const std::vector<planetary::stack::FrameStats>& stats, const planetary::ser::FileInfo& info) {
    using namespace planetary::stack;
    Reading r;
    const int64_t n = stats.size();
    const double scale = LumaScale(info);
    const int lw = static_cast<int>(info.header.width / scale), lh = static_cast<int>(info.header.height / scale);
    const int64_t need = std::max<int64_t>(1, std::min<int64_t>(kMinStackFrames, (n + 1) / 2));
    const int64_t floorCount = std::max<int64_t>(need, static_cast<int64_t>(std::ceil(std::sqrt(double(n)))));
    std::vector<const FrameStats*> found;
    for (const auto& s : stats)
        if (s.targetFound) found.push_back(&s);
    if (found.empty()) return r;
    std::vector<double> ws, hs;
    for (auto* s : found) {
        ws.push_back(s->bx1 - s->bx0);
        hs.push_back(s->by1 - s->by0);
    }
    const double mw = Med(ws), mh = Med(hs);
    auto within = [](double v, double m) { return v >= m / kBoxTolerance && v <= m * kBoxTolerance; };
    std::vector<double> dq, ds, ds2;
    std::vector<const FrameStats*> det;
    for (auto* s : found) {
        if (!(within(s->bx1 - s->bx0, mw) && within(s->by1 - s->by0, mh))) continue;
        if (s->bx0 <= 0 || s->by0 <= 0 || s->bx1 >= lw || s->by1 >= lh) continue;
        if (s->clippedFraction >= kClippedFrame) continue;
        if (!(s->quality > 0)) continue;
        det.push_back(s);
        dq.push_back(s->quality);
        ds.push_back(s->qualitySigma);
        ds2.push_back(s->quality2Sigma);
    }
    if (det.empty()) return r;
    const DetailRanking rank = RankByDetail(dq, ds, ds2, need, floorCount);
    std::vector<double> radii, half;
    const int d = DetectionBlock(lw, lh);
    for (size_t i : rank.ranked) {
        const FrameStats* s = det[i];
        radii.push_back(std::sqrt(double(s->bx1 - s->bx0) * (s->by1 - s->by0)) / 2 - d);
        half.push_back(s->halfRadius);
    }
    r.ok = true;
    r.detailed = det.size();
    r.qRef = rank.qRef;
    r.rbox = std::max(1.0, Med(radii));
    r.rh = std::max(1.0, Med(half));
    const double sp = std::sqrt(M_PI);
    r.X = 1 / (sp * r.qRef * r.qRef * r.rh) / r.rh;
    {  // Ruling P26 fix round 1's gross reading at tauLo (Select.cpp step 7 then; not the shipped one, above)
        std::vector<const FrameStats*> left;
        for (size_t i : rank.left) left.push_back(det[i]);
        double mu = 0, n2 = 0, var = 0;
        for (auto* s : left) {
            mu += s->quality;
            n2 += s->qualitySigma * s->qualitySigma;
        }
        mu /= left.size();
        n2 /= left.size();
        for (auto* s : left) var += (s->quality - mu) * (s->quality - mu);
        var /= left.size();
        const double se = std::sqrt(2.0 / left.size()) * var, tauLo = std::sqrt(std::max(0.0, var - n2 - 2 * se));
        const double qg = rank.priorSd > tauLo ? rank.pointQuality + (r.qRef - rank.pointQuality) * tauLo / rank.priorSd : r.qRef;
        r.Xg = 1 / (sp * qg * qg * r.rh) / r.rh;
        r.muG = rank.pointQuality;
        r.sdG = rank.priorSd;
        r.tauLo = tauLo;
    }
    const double sg = 1 / (sp * r.qRef * r.qRef * r.rbox);
    const double sgc = std::sqrt(std::max(0.0, sg * sg - kSamplingBlur * kSamplingBlur));
    r.S = sgc / std::max(kBlurLimit * r.rbox, kBlurFloorPixels / scale);
    return r;
}
