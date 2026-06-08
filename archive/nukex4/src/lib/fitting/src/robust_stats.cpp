#include "nukex/fitting/robust_stats.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <numeric>

namespace nukex {

float median_inplace(float* data, int n) {
    if (n <= 0) return 0.0f;
    if (n == 1) return data[0];
    std::nth_element(data, data + n / 2, data + n);
    float med = data[n / 2];
    if (n % 2 == 0) {
        float left = *std::max_element(data, data + n / 2);
        med = 0.5f * (left + med);
    }
    return med;
}

float mad(const float* data, int n) {
    if (n <= 0) return 0.0f;
    std::vector<float> copy(data, data + n);
    float med = median_inplace(copy.data(), n);
    std::vector<float> abs_devs(n);
    for (int i = 0; i < n; i++) {
        abs_devs[i] = std::fabs(data[i] - med);
    }
    return median_inplace(abs_devs.data(), n);
}

float biweight_location(const float* data, int n) {
    if (n <= 0) return 0.0f;
    if (n == 1) return data[0];

    constexpr float c = 6.0f;
    constexpr int max_iter = 10;
    constexpr float tol = 1e-7f;

    std::vector<float> copy(data, data + n);
    float location = median_inplace(copy.data(), n);

    float mad_val = mad(data, n);
    if (mad_val < 1e-30f) return location;
    float scale = mad_val * 1.4826f;

    for (int iter = 0; iter < max_iter; iter++) {
        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; i++) {
            float u = (data[i] - location) / (c * scale);
            if (std::fabs(u) < 1.0f) {
                float u2 = u * u;
                float w = (1.0f - u2) * (1.0f - u2);
                num += w * data[i];
                den += w;
            }
        }
        if (den < 1e-30) break;
        float new_location = static_cast<float>(num / den);
        if (std::fabs(new_location - location) < tol * scale) {
            location = new_location;
            break;
        }
        location = new_location;
    }
    return location;
}

float biweight_midvariance(const float* data, int n) {
    if (n <= 1) return 0.0f;

    constexpr float c = 9.0f;

    std::vector<float> copy(data, data + n);
    float med = median_inplace(copy.data(), n);

    float mad_val = mad(data, n);
    if (mad_val < 1e-30f) return 0.0f;

    double num = 0.0, den = 0.0;
    for (int i = 0; i < n; i++) {
        float u = (data[i] - med) / (c * mad_val);
        if (std::fabs(u) < 1.0f) {
            float u2 = u * u;
            float diff = data[i] - med;
            num += diff * diff * std::pow(1.0f - u2, 4);
            den += (1.0f - u2) * (1.0f - 5.0f * u2);
        }
    }
    if (std::fabs(den) < 1e-30) return 0.0f;
    return static_cast<float>(n * num / (den * den));
}

float iqr(const float* data, int n) {
    if (n < 4) return 0.0f;
    std::vector<float> sorted(data, data + n);
    std::sort(sorted.begin(), sorted.end());
    int q1_idx = n / 4;
    int q3_idx = (3 * n) / 4;
    return sorted[q3_idx] - sorted[q1_idx];
}

float weighted_median(const float* data, const float* weights, int n) {
    if (n <= 0) return 0.0f;
    if (n == 1) return data[0];

    std::vector<int> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
              [data](int a, int b) { return data[a] < data[b]; });

    double total_weight = 0.0;
    for (int i = 0; i < n; i++) total_weight += weights[i];
    if (total_weight < 1e-30) return data[idx[n / 2]];

    double cumulative = 0.0;
    double half = total_weight * 0.5;
    for (int i = 0; i < n; i++) {
        cumulative += weights[idx[i]];
        if (cumulative >= half) {
            return data[idx[i]];
        }
    }
    return data[idx[n - 1]];
}

} // namespace nukex
