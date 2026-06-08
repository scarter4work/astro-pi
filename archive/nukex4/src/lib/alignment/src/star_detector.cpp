#include "nukex/alignment/star_detector.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>
#include <tuple>

namespace nukex {

std::pair<float, float> StarDetector::compute_background_noise(const Image& image) {
    // Sample pixels for background estimation (every 4th pixel for speed)
    std::vector<float> samples;
    int step = 4;
    for (int y = 0; y < image.height(); y += step) {
        for (int x = 0; x < image.width(); x += step) {
            samples.push_back(image.at(x, y, 0));
        }
    }

    if (samples.empty()) return {0.0f, 1.0f};

    // Median
    size_t n = samples.size();
    std::nth_element(samples.begin(), samples.begin() + n / 2, samples.end());
    float median = samples[n / 2];

    // MAD (Median Absolute Deviation)
    for (auto& v : samples) {
        v = std::abs(v - median);
    }
    std::nth_element(samples.begin(), samples.begin() + n / 2, samples.end());
    float mad = samples[n / 2];

    // Scale MAD to estimate sigma: sigma ≈ 1.4826 * MAD for Gaussian data
    float sigma = 1.4826f * mad;
    if (sigma < 1e-10f) sigma = 1e-10f;

    return {median, sigma};
}

std::vector<std::tuple<int, int, float>> StarDetector::find_local_maxima(
    const Image& image, float threshold, int exclusion_radius) {

    int w = image.width();
    int h = image.height();
    int r = 3; // local max search radius

    std::vector<std::tuple<int, int, float>> candidates;

    // Skip border pixels
    for (int y = r; y < h - r; y++) {
        for (int x = r; x < w - r; x++) {
            float val = image.at(x, y, 0);
            if (val < threshold) continue;

            // Check if this is a local maximum in a (2r+1) × (2r+1) neighborhood
            bool is_max = true;
            for (int dy = -r; dy <= r && is_max; dy++) {
                for (int dx = -r; dx <= r && is_max; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (image.at(x + dx, y + dy, 0) > val) {
                        is_max = false;
                    }
                }
            }

            if (is_max) {
                candidates.emplace_back(x, y, val);
            }
        }
    }

    // Sort by peak value (brightest first)
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  return std::get<2>(a) > std::get<2>(b);
              });

    // Apply exclusion radius — remove candidates too close to brighter ones
    std::vector<std::tuple<int, int, float>> filtered;
    for (const auto& [cx, cy, cv] : candidates) {
        bool too_close = false;
        for (const auto& [fx, fy, fv] : filtered) {
            float dx = static_cast<float>(cx - fx);
            float dy = static_cast<float>(cy - fy);
            if (dx * dx + dy * dy < static_cast<float>(exclusion_radius * exclusion_radius)) {
                too_close = true;
                break;
            }
        }
        if (!too_close) {
            filtered.emplace_back(cx, cy, cv);
        }
    }

    return filtered;
}

std::pair<float, float> StarDetector::refine_centroid(
    const Image& image, int x, int y) {

    // Compute intensity-weighted centroid in a 7×7 window
    // This is faster and more robust than full 2D Gaussian fitting
    // for the purpose of sub-pixel centroid estimation.
    int w = image.width();
    int h = image.height();
    int radius = 3;

    float background = 0.0f;
    int bg_count = 0;

    // Estimate local background from corners of an 11×11 region
    for (int dy = -5; dy <= 5; dy += 10) {
        for (int dx = -5; dx <= 5; dx += 10) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                background += image.at(px, py, 0);
                bg_count++;
            }
        }
    }
    if (bg_count > 0) background /= static_cast<float>(bg_count);

    float sum_x = 0.0f, sum_y = 0.0f, sum_w = 0.0f;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px < 0 || px >= w || py < 0 || py >= h) continue;

            float val = image.at(px, py, 0) - background;
            if (val <= 0.0f) continue;

            sum_x += val * static_cast<float>(px);
            sum_y += val * static_cast<float>(py);
            sum_w += val;
        }
    }

    if (sum_w > 1e-10f) {
        return {sum_x / sum_w, sum_y / sum_w};
    }
    return {static_cast<float>(x), static_cast<float>(y)};
}

float StarDetector::compute_flux(const Image& image, float cx, float cy,
                                  float background, int aperture_radius) {
    int w = image.width();
    int h = image.height();
    int icx = static_cast<int>(cx + 0.5f);
    int icy = static_cast<int>(cy + 0.5f);
    float r2 = static_cast<float>(aperture_radius * aperture_radius);
    float flux = 0.0f;

    for (int dy = -aperture_radius; dy <= aperture_radius; dy++) {
        for (int dx = -aperture_radius; dx <= aperture_radius; dx++) {
            int px = icx + dx;
            int py = icy + dy;
            if (px < 0 || px >= w || py < 0 || py >= h) continue;
            float dist2 = static_cast<float>(dx * dx + dy * dy);
            if (dist2 > r2) continue;

            float val = image.at(px, py, 0) - background;
            if (val > 0.0f) flux += val;
        }
    }

    return flux;
}

float StarDetector::saturation_fraction(const Image& image, float saturation_level) {
    if (image.empty()) return 0.0f;
    const int w = image.width();
    const int h = image.height();
    const int step = 4;  // 4x decimation: ~16x faster than full scan
    long saturated = 0;
    long total = 0;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            if (image.at(x, y, 0) >= saturation_level) saturated++;
            total++;
        }
    }
    if (total == 0) return 0.0f;
    return static_cast<float>(saturated) / static_cast<float>(total);
}

StarCatalog StarDetector::detect(const Image& image, const Config& config) {
    StarCatalog catalog;

    if (image.empty() || image.width() < 20 || image.height() < 20) {
        return catalog;
    }

    // Saturation guard: reject frames where a majority of pixels are clipped
    // to saturation.  Without this, find_local_maxima sees the entire clipped
    // plateau as candidate stars and the O(n^2) exclusion-radius filter hangs
    // (observed 2026-04-20 on a dawn-twilight M27 frame; 5 min per frame).
    if (saturation_fraction(image, config.saturation_level)
            >= config.saturation_reject_fraction) {
        return catalog;  // empty — caller treats as alignment failure
    }

    // Step 1: Background and noise estimation
    auto [background, sigma] = compute_background_noise(image);
    float threshold = background + config.snr_multiplier * sigma;

    // Step 2: Find local maxima
    auto candidates = find_local_maxima(image, threshold, config.exclusion_radius);

    // Step 3: Build star catalog with refined centroids
    for (const auto& [ix, iy, peak] : candidates) {
        // Reject saturated stars
        if (peak > config.saturation_level) continue;

        Star star;
        star.peak = peak;
        star.background = background;

        // Refine centroid
        auto [rx, ry] = refine_centroid(image, ix, iy);
        star.x = rx;
        star.y = ry;

        // Estimate FWHM from second moments of intensity around centroid.
        // For a Gaussian PSF, FWHM = 2.355 * sigma (the Gaussian FWHM-sigma relation).
        // We compute sigma_x and sigma_y independently, then use geometric mean.
        {
            int w = image.width();
            int h = image.height();
            int fwhm_radius = 3;
            float sum_xx = 0.0f, sum_yy = 0.0f, sum_wt = 0.0f;
            int icx = static_cast<int>(rx + 0.5f);
            int icy = static_cast<int>(ry + 0.5f);

            for (int dy = -fwhm_radius; dy <= fwhm_radius; dy++) {
                for (int dx = -fwhm_radius; dx <= fwhm_radius; dx++) {
                    int px = icx + dx;
                    int py = icy + dy;
                    if (px < 0 || px >= w || py < 0 || py >= h) continue;

                    float val = image.at(px, py, 0) - background;
                    if (val <= 0.0f) continue;

                    float dxc = static_cast<float>(px) - rx;
                    float dyc = static_cast<float>(py) - ry;
                    sum_xx += val * dxc * dxc;
                    sum_yy += val * dyc * dyc;
                    sum_wt += val;
                }
            }

            if (sum_wt > 1e-10f) {
                float sigma_x = std::sqrt(sum_xx / sum_wt);
                float sigma_y = std::sqrt(sum_yy / sum_wt);
                float sigma = std::sqrt(sigma_x * sigma_y);
                if (sigma > 0.1f) {
                    star.fwhm_x = 2.355f * sigma_x;
                    star.fwhm_y = 2.355f * sigma_y;
                    star.fwhm = 2.355f * sigma;
                }
            }
        }

        // Compute flux
        star.flux = compute_flux(image, rx, ry, background);

        // SNR
        star.snr = (peak - background) / sigma;

        // Require minimum SNR above detection threshold
        if (star.snr < config.snr_multiplier) continue;

        catalog.stars.push_back(star);
    }

    // Step 4: Sort by flux and keep top N
    catalog.sort_by_flux();
    catalog.keep_top(config.max_stars);

    return catalog;
}

void StarCatalog::sort_by_flux() {
    std::sort(stars.begin(), stars.end(),
              [](const Star& a, const Star& b) { return a.flux > b.flux; });
}

void StarCatalog::keep_top(int n) {
    if (static_cast<int>(stars.size()) > n) {
        stars.resize(n);
    }
}

} // namespace nukex
