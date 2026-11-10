#include "SigmaClip.h"

#include "Numerics.h"

#include <cmath>
#include <limits>

namespace gdg {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// np.nanmean of NaN-free data: pairwise sum / count.
double mean_of(const std::vector<double>& x) { return pairwise_sum(x) / static_cast<double>(x.size()); }

// np.nanstd(ddof=0) of NaN-free data, numpy's two-pass nanvar: mean, squared deviations,
// pairwise sum, divide by the count, sqrt.
double std_of(const std::vector<double>& x) {
    const double avg = mean_of(x);
    std::vector<double> sq(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double d = x[i] - avg;
        sq[i] = d * d;
    }
    return std::sqrt(pairwise_sum(sq) / static_cast<double>(x.size()));
}

}  // namespace

ClipStats sigma_clipped_stats(const std::vector<double>& v, double sigma, int maxiters) {
    ClipStats out{kNaN, kNaN, kNaN};
    std::vector<double> data;
    data.reserve(v.size());
    for (double x : v) {
        if (std::isfinite(x))
            data.push_back(x);
        else
            ++out.nonfinite;
    }

    // astropy SigmaClip._sigmaclip_noaxis.
    const bool unlimited = maxiters == 0;  // `maxiters or np.inf`
    size_t nchanged = 1;
    int iteration = 0;
    std::vector<double> kept;
    while (nchanged != 0 && (unlimited || iteration < maxiters)) {
        ++iteration;
        const size_t size = data.size();
        if (size == 0) break;  // bounds would be NaN and keep nothing: nchanged 0
        const double cen = median(data);
        const double sd = std_of(data);
        const double lo = cen - sd * sigma;
        const double hi = cen + sd * sigma;
        kept.clear();
        for (double x : data)
            if (x >= lo && x <= hi) kept.push_back(x);
        data.swap(kept);
        nchanged = size - data.size();
    }
    out.iterations = iteration;

    if (!data.empty()) {
        out.mean = mean_of(data);
        out.median = median(data);
        out.std = std_of(data);
    }
    return out;
}

}  // namespace gdg
