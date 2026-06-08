#include "nukex/combine/spatial_context.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

namespace nukex {

float SpatialContext::sobel_gradient(const Image& img, int x, int y) {
    if (x <= 0 || x >= img.width() - 1 || y <= 0 || y >= img.height() - 1)
        return 0.0f;

    float max_grad = 0.0f;
    for (int ch = 0; ch < img.n_channels(); ch++) {
        float gx = -1.0f * img.at(x-1, y-1, ch) + 1.0f * img.at(x+1, y-1, ch)
                  + -2.0f * img.at(x-1, y,   ch) + 2.0f * img.at(x+1, y,   ch)
                  + -1.0f * img.at(x-1, y+1, ch) + 1.0f * img.at(x+1, y+1, ch);
        float gy = -1.0f * img.at(x-1, y-1, ch) - 2.0f * img.at(x, y-1, ch) - 1.0f * img.at(x+1, y-1, ch)
                  +  1.0f * img.at(x-1, y+1, ch) + 2.0f * img.at(x, y+1, ch) + 1.0f * img.at(x+1, y+1, ch);
        float grad = std::sqrt(gx * gx + gy * gy);
        max_grad = std::max(max_grad, grad);
    }
    return max_grad;
}

void SpatialContext::compute(const Image& output, Cube& cube) const {
    int w = cube.width;
    int h = cube.height;

    // Pre-allocate reusable buffer for neighborhood computation
    int max_window = (2 * WINDOW_RADIUS + 1) * (2 * WINDOW_RADIUS + 1);
    std::vector<float> neighborhood(max_window);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            auto& voxel = cube.at(x, y);
            voxel.gradient_mag = sobel_gradient(output, x, y);

            int x0 = std::max(0, x - WINDOW_RADIUS);
            int x1 = std::min(w - 1, x + WINDOW_RADIUS);
            int y0 = std::max(0, y - WINDOW_RADIUS);
            int y1 = std::min(h - 1, y + WINDOW_RADIUS);

            int nn = 0;
            for (int ny = y0; ny <= y1; ny++) {
                for (int nx = x0; nx <= x1; nx++) {
                    float avg = 0.0f;
                    for (int ch = 0; ch < output.n_channels(); ch++) {
                        avg += output.at(nx, ny, ch);
                    }
                    neighborhood[nn++] = avg / output.n_channels();
                }
            }

            if (nn > 0) {
                voxel.local_background = biweight_location(neighborhood.data(), nn);
                voxel.local_rms = mad(neighborhood.data(), nn) * 1.4826f;
            }
        }
    }
}

} // namespace nukex
