// Test-only calibration probe (Ruling P26; test/calibration/, built by test/CMakeLists.txt): never shipped, not a
// ctest case (a scalar model, no frames). Built as calib-sigmafloor.
//
//   calib-sigmafloor [draws per cell (400000)]
//
// qualitySigma's soft floor (Measure.h "qualitySigma", kSigmaFloor). The scalar model: quality^2 ~ N(mu, rho^2),
// observed where positive, rho = 1; quality = sqrt(quality^2), its real sd T over the draws. A floored form
//     h = rho / (2 (q^(2p) + (m rho)^p)^(1 / (2p)))     (p = 4, m = kSigmaFloor: Measure.h's form)
// against T: the worst |mean / T - 1|, |rms / T - 1| and |median / T - 1| over the cells whose median
// quality / h lies in [2.2, 50] (and the lowest median ratio that range starts from); for every p in
// {2, 3, 4, 6} and m 0.6-2.0, the best five, then the shipped form's cells row by row and the plain
// first-order form (m = 0) for comparison. Exit 0.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "Measure.h"

namespace {
struct Cell {
    double r50 = 0, mean = 0, rms = 0, median = 0;  // the ratio, and h's mean / rms / median over T
};
Cell Evaluate(const std::vector<double>& z, double T, int p, double m) {
    std::vector<double> h, r;
    h.reserve(z.size());
    r.reserve(z.size());
    for (double v : z) {
        const double f = std::pow(std::pow(v, p) + std::pow(m, p), 1.0 / p);  // floored quality^2, rho units
        const double hv = 1 / (2 * std::sqrt(f));
        h.push_back(hv);
        r.push_back(std::sqrt(v) / hv);
    }
    double s = 0, s2 = 0;
    for (double v : h) {
        s += v;
        s2 += v * v;
    }
    const double n = static_cast<double>(h.size());
    std::nth_element(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() / 2), h.end());
    std::nth_element(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(r.size() / 2), r.end());
    return {r[r.size() / 2], s / n / T, std::sqrt(s2 / n) / T, h[h.size() / 2] / T};
}
}  // namespace

int main(int argc, char** argv) {
    const int draws = argc > 1 ? std::atoi(argv[1]) : 400000;
    std::mt19937 rng(6);
    std::normal_distribution<double> n01(0, 1);
    std::vector<double> base(static_cast<size_t>(draws));
    for (double& v : base) v = n01(rng);
    std::vector<double> mus;
    for (double mu = 0.05; mu < 3; mu += 0.05) mus.push_back(mu);
    for (double mu = 3; mu <= 60.01; mu += 0.5) mus.push_back(mu);
    std::vector<std::vector<double>> Z;
    std::vector<double> T;
    for (double mu : mus) {
        std::vector<double> z;
        for (double b : base)
            if (mu + b > 0) z.push_back(mu + b);
        double s = 0, s2 = 0;
        for (double v : z) {
            s += std::sqrt(v);
            s2 += v;
        }
        const double nz = static_cast<double>(z.size());
        T.push_back(std::sqrt(std::max(0.0, s2 / nz - (s / nz) * (s / nz))));
        Z.push_back(std::move(z));
    }
    struct Result {
        double worst, lowest;
        int p;
        double m;
    };
    auto worstOf = [&](int p, double m, bool print) {
        double w = 0, lo = 1e9;
        for (size_t i = 0; i < mus.size(); ++i) {
            const Cell c = Evaluate(Z[i], T[i], p, m);
            if (c.r50 < 2.2 || c.r50 > 50) continue;
            lo = std::min(lo, c.r50);
            w = std::max({w, std::fabs(c.mean - 1), std::fabs(c.rms - 1), std::fabs(c.median - 1)});
            if (print) std::printf("    median ratio %6.2f: mean %.3f rms %.3f median %.3f\n", c.r50, c.mean, c.rms, c.median);
        }
        return Result{w, lo, p, m};
    };
    std::vector<Result> all;
    for (int p : {2, 3, 4, 6})
        for (double m = 0.6; m <= 2.001; m += 0.1) all.push_back(worstOf(p, m, false));
    std::sort(all.begin(), all.end(), [](const Result& a, const Result& b) { return a.worst < b.worst; });
    std::printf("best floors (worst deviation over median ratios 2.2-50):\n");
    for (size_t i = 0; i < std::min<size_t>(5, all.size()); ++i)
        std::printf("  p %d m %.1f: %.1f%% from %.2f\n", all[i].p, all[i].m, 100 * all[i].worst, all[i].lowest);
    std::printf("shipped p 4 m %.2f:\n", planetary::stack::kSigmaFloor);
    const Result shipped = worstOf(4, planetary::stack::kSigmaFloor, true);
    std::printf("  worst %.1f%% from a median ratio of %.2f\n", 100 * shipped.worst, shipped.lowest);
    // The plain first-order form, rho / (2 q): no floor.
    double plainRms = 0, plainLo = 1e9;
    for (size_t i = 0; i < mus.size(); ++i) {
        const Cell c = Evaluate(Z[i], T[i], 4, 0.0);
        if (c.r50 < 2.2 || c.r50 > 50) continue;
        plainLo = std::min(plainLo, c.r50);
        plainRms = std::max(plainRms, std::fabs(c.rms - 1));
    }
    std::printf("plain rho / (2 q): worst rms deviation %.0f%% from a median ratio of %.2f\n", 100 * plainRms, plainLo);
    return 0;
}
