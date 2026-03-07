#include "engine/StarDetector.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace nukex {

std::pair<double, double> computeBackground(const float* data, size_t count) {
    if (count == 0) return {0.0, 0.0};

    // Copy and sort for median
    std::vector<double> sorted(data, data + count);
    std::sort(sorted.begin(), sorted.end());

    double median;
    if (count % 2 == 0) {
        median = (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0;
    } else {
        median = sorted[count / 2];
    }

    // Compute MAD = median(|x_i - median|)
    std::vector<double> absdev(count);
    for (size_t i = 0; i < count; ++i) {
        absdev[i] = std::abs(sorted[i] - median);
    }
    std::sort(absdev.begin(), absdev.end());

    double mad;
    if (count % 2 == 0) {
        mad = (absdev[count / 2 - 1] + absdev[count / 2]) / 2.0;
    } else {
        mad = absdev[count / 2];
    }

    return {median, mad};
}

std::vector<Blob> extractBlobs(const float* image, int width, int height, double threshold) {
    const int n = width * height;
    std::vector<bool> visited(n, false);
    std::vector<Blob> blobs;

    for (int i = 0; i < n; ++i) {
        if (visited[i] || static_cast<double>(image[i]) <= threshold)
            continue;

        // BFS flood fill with 4-connectivity
        Blob blob;
        std::vector<int> queue;
        queue.push_back(i);
        visited[i] = true;

        size_t head = 0;
        while (head < queue.size()) {
            int idx = queue[head++];
            int x = idx % width;
            int y = idx / width;
            blob.emplace_back(x, y);

            // 4-connectivity neighbors: up, down, left, right
            const int dx[] = {0, 0, -1, 1};
            const int dy[] = {-1, 1, 0, 0};
            for (int d = 0; d < 4; ++d) {
                int nx = x + dx[d];
                int ny = y + dy[d];
                if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                    int nidx = ny * width + nx;
                    if (!visited[nidx] && static_cast<double>(image[nidx]) > threshold) {
                        visited[nidx] = true;
                        queue.push_back(nidx);
                    }
                }
            }
        }

        blobs.push_back(std::move(blob));
    }

    return blobs;
}

std::optional<StarPosition> blobToStar(const Blob& blob, const float* image,
                                        int width, int height,
                                        int minSize, int maxSize,
                                        double maxEccentricity) {
    int blobSize = static_cast<int>(blob.size());

    // Size filtering
    if (blobSize < minSize || blobSize > maxSize)
        return std::nullopt;

    // Intensity-weighted centroid
    double sumVal = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;

    for (const auto& [px, py] : blob) {
        double val = static_cast<double>(image[py * width + px]);
        sumVal += val;
        sumX += px * val;
        sumY += py * val;
    }

    if (sumVal <= 0.0)
        return std::nullopt;

    double cx = sumX / sumVal;
    double cy = sumY / sumVal;

    // Second moments for eccentricity
    double Mxx = 0.0, Myy = 0.0, Mxy = 0.0;
    for (const auto& [px, py] : blob) {
        double val = static_cast<double>(image[py * width + px]);
        double dx = px - cx;
        double dy = py - cy;
        Mxx += val * dx * dx;
        Myy += val * dy * dy;
        Mxy += val * dx * dy;
    }
    Mxx /= sumVal;
    Myy /= sumVal;
    Mxy /= sumVal;

    // Eigenvalues of the 2x2 moment matrix [[Mxx, Mxy], [Mxy, Myy]]
    double trace = Mxx + Myy;
    double det = Mxx * Myy - Mxy * Mxy;
    double disc = trace * trace - 4.0 * det;
    if (disc < 0.0) disc = 0.0;

    double sqrtDisc = std::sqrt(disc);
    double lambda1 = (trace + sqrtDisc) / 2.0;
    double lambda2 = (trace - sqrtDisc) / 2.0;

    // Eccentricity: sqrt(1 - lambda2/lambda1)
    double ecc = 0.0;
    if (lambda1 > 0.0) {
        double ratio = lambda2 / lambda1;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        ecc = std::sqrt(1.0 - ratio);
    }

    if (ecc > maxEccentricity)
        return std::nullopt;

    return StarPosition{cx, cy, sumVal};
}

std::vector<StarPosition> detectStars(const float* image, int width, int height,
                                       const DetectorConfig& config) {
    size_t count = static_cast<size_t>(width) * height;

    // Step 1: background statistics
    auto [median, mad] = computeBackground(image, count);

    // Step 2: threshold = median + sigmaThreshold * mad * 1.4826 (MAD-to-sigma)
    double threshold = median + config.sigmaThreshold * mad * 1.4826;

    // Step 3: extract blobs
    auto blobs = extractBlobs(image, width, height, threshold);

    // Step 4: filter blobs and compute centroids
    std::vector<StarPosition> stars;
    stars.reserve(blobs.size());
    for (const auto& blob : blobs) {
        auto star = blobToStar(blob, image, width, height,
                               config.minBlobSize, config.maxBlobSize,
                               config.maxEccentricity);
        if (star.has_value()) {
            stars.push_back(star.value());
        }
    }

    // Step 5: sort by flux descending
    std::sort(stars.begin(), stars.end(),
              [](const StarPosition& a, const StarPosition& b) {
                  return a.flux > b.flux;
              });

    return stars;
}

} // namespace nukex
