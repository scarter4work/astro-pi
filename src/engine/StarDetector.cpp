#include "engine/StarDetector.h"

#include <algorithm>
#include <cmath>
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

} // namespace nukex
