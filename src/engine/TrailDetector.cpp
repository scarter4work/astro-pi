#include "engine/TrailDetector.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace nukex {

TrailDetector::TrailDetector(const TrailDetectorConfig& config)
    : m_config(config) {}

// ---------------------------------------------------------------------------
// Local background estimation using box median
// ---------------------------------------------------------------------------
std::vector<float> TrailDetector::estimateBackground(const float* data,
                                                      int width, int height) const
{
    int box = m_config.backgroundBoxSize;
    int halfBox = box / 2;
    std::vector<float> bg(size_t(width) * height);
    std::vector<float> samples;
    samples.reserve(size_t(box) * box);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            samples.clear();
            int y0 = std::max(0, y - halfBox);
            int y1 = std::min(height, y + halfBox);
            int x0 = std::max(0, x - halfBox);
            int x1 = std::min(width, x + halfBox);
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx)
                    samples.push_back(data[yy * width + xx]);
            size_t n = samples.size();
            std::nth_element(samples.begin(), samples.begin() + n / 2, samples.end());
            bg[y * width + x] = samples[n / 2];
        }
    }
    return bg;
}

// ---------------------------------------------------------------------------
// Sobel gradient magnitude
// ---------------------------------------------------------------------------
std::vector<float> TrailDetector::sobelMagnitude(const float* residual,
                                                   int width, int height) const
{
    std::vector<float> mag(size_t(width) * height, 0.0f);

    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            // Sobel kernels
            float gx = -residual[(y-1)*width + (x-1)] + residual[(y-1)*width + (x+1)]
                      - 2*residual[y*width + (x-1)]   + 2*residual[y*width + (x+1)]
                      - residual[(y+1)*width + (x-1)] + residual[(y+1)*width + (x+1)];
            float gy = -residual[(y-1)*width + (x-1)] - 2*residual[(y-1)*width + x] - residual[(y-1)*width + (x+1)]
                      + residual[(y+1)*width + (x-1)] + 2*residual[(y+1)*width + x] + residual[(y+1)*width + (x+1)];
            mag[y * width + x] = std::sqrt(gx * gx + gy * gy);
        }
    }
    return mag;
}

// ---------------------------------------------------------------------------
// Hough Transform line detection
// ---------------------------------------------------------------------------
std::vector<DetectedTrail> TrailDetector::houghLines(const std::vector<uint8_t>& edges,
                                                      int width, int height) const
{
    // Accumulator parameterization
    int diag = static_cast<int>(std::ceil(std::sqrt(double(width)*width + double(height)*height)));
    int rhoMax = 2 * diag + 1;        // rho range: [-diag, +diag]
    constexpr int numTheta = 180;      // 1-degree resolution
    constexpr double thetaStep = M_PI / numTheta;

    // Precompute sin/cos
    std::vector<double> cosTable(numTheta), sinTable(numTheta);
    for (int t = 0; t < numTheta; ++t) {
        double theta = t * thetaStep;
        cosTable[t] = std::cos(theta);
        sinTable[t] = std::sin(theta);
    }

    // Accumulator
    std::vector<int> accum(size_t(rhoMax) * numTheta, 0);

    // Vote
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!edges[y * width + x])
                continue;
            for (int t = 0; t < numTheta; ++t) {
                double rho = x * cosTable[t] + y * sinTable[t];
                int rhoIdx = static_cast<int>(std::round(rho)) + diag;
                if (rhoIdx >= 0 && rhoIdx < rhoMax)
                    ++accum[rhoIdx * numTheta + t];
            }
        }
    }

    // Peak detection: threshold based on image dimensions
    // Satellite trails typically span > 25% of the frame diagonal
    int voteThreshold = std::max(m_config.minLineLength, std::max(width, height) / 4);

    // Find peaks above threshold
    struct Peak { int rhoIdx; int thetaIdx; int votes; };
    std::vector<Peak> peaks;
    for (int r = 0; r < rhoMax; ++r) {
        for (int t = 0; t < numTheta; ++t) {
            int v = accum[r * numTheta + t];
            if (v >= voteThreshold)
                peaks.push_back({r, t, v});
        }
    }

    // Sort by votes descending
    std::sort(peaks.begin(), peaks.end(),
              [](const Peak& a, const Peak& b) { return a.votes > b.votes; });

    // Cluster nearby peaks (merge within 5 rho pixels and 3 degrees)
    std::vector<DetectedTrail> trails;
    std::vector<bool> used(peaks.size(), false);

    for (size_t i = 0; i < peaks.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;

        double sumRho = peaks[i].rhoIdx - diag;
        double sumTheta = peaks[i].thetaIdx * thetaStep;
        int sumVotes = peaks[i].votes;
        int count = 1;

        for (size_t j = i + 1; j < peaks.size(); ++j) {
            if (used[j]) continue;
            int dRho = std::abs(peaks[i].rhoIdx - peaks[j].rhoIdx);
            int dTheta = std::abs(peaks[i].thetaIdx - peaks[j].thetaIdx);
            // Handle theta wrap-around (0 and 179 degrees are neighbors)
            dTheta = std::min(dTheta, numTheta - dTheta);
            if (dRho <= 5 && dTheta <= 3) {
                used[j] = true;
                sumRho += peaks[j].rhoIdx - diag;
                sumTheta += peaks[j].thetaIdx * thetaStep;
                sumVotes += peaks[j].votes;
                ++count;
            }
        }

        DetectedTrail trail;
        trail.rho = sumRho / count;
        trail.theta = sumTheta / count;
        trail.votes = sumVotes / count;
        trail.width = m_config.dilateRadius;
        trails.push_back(trail);
    }

    return trails;
}

// ---------------------------------------------------------------------------
// Generate dilated mask from detected trails
// ---------------------------------------------------------------------------
std::vector<uint8_t> TrailDetector::generateMask(const std::vector<DetectedTrail>& trails,
                                                   int width, int height) const
{
    std::vector<uint8_t> mask(size_t(width) * height, 0);
    if (trails.empty()) return mask;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (const auto& trail : trails) {
                // Distance from point (x,y) to line rho = x*cos(theta) + y*sin(theta)
                double cosT = std::cos(trail.theta);
                double sinT = std::sin(trail.theta);
                double dist = std::abs(x * cosT + y * sinT - trail.rho);
                if (dist <= trail.width) {
                    mask[y * width + x] = 1;
                    break;
                }
            }
        }
    }
    return mask;
}

// ---------------------------------------------------------------------------
// Main detection pipeline for a single frame
// ---------------------------------------------------------------------------
std::vector<uint8_t> TrailDetector::detectAndMask(const float* frameData,
                                                    int width, int height) const
{
    size_t numPx = size_t(width) * height;

    // 1. Local background subtraction
    std::vector<float> bg = estimateBackground(frameData, width, height);
    std::vector<float> residual(numPx);
    for (size_t i = 0; i < numPx; ++i)
        residual[i] = frameData[i] - bg[i];

    // 2. Compute residual statistics for thresholding
    double sum = 0, sumSq = 0;
    for (size_t i = 0; i < numPx; ++i) {
        sum += residual[i];
        sumSq += double(residual[i]) * residual[i];
    }
    double mean = sum / numPx;
    double sigma = std::sqrt(sumSq / numPx - mean * mean);
    if (sigma < 1e-10) sigma = 1e-10;

    // 3. Sobel edge detection on residual
    std::vector<float> grad = sobelMagnitude(residual.data(), width, height);

    // Gradient statistics for edge threshold
    double gSum = 0, gSumSq = 0;
    for (size_t i = 0; i < numPx; ++i) {
        gSum += grad[i];
        gSumSq += double(grad[i]) * grad[i];
    }
    double gMean = gSum / numPx;
    double gSigma = std::sqrt(gSumSq / numPx - gMean * gMean);

    // 4. Binary edge image: edges where gradient > mean + threshold*sigma
    //    AND residual is bright (above mean + threshold*sigma of residual).
    //    The residual filter ensures we only detect bright linear features
    //    (satellite trails), not dark features (dust motes) or noise edges.
    float edgeThresh = static_cast<float>(gMean + m_config.threshold * gSigma);
    float brightThresh = static_cast<float>(mean + m_config.threshold * sigma);

    std::vector<uint8_t> edges(numPx, 0);
    for (size_t i = 0; i < numPx; ++i) {
        if (grad[i] > edgeThresh && residual[i] > brightThresh)
            edges[i] = 1;
    }

    // 5. Hough transform
    std::vector<DetectedTrail> trails = houghLines(edges, width, height);
    m_lastTrails = trails;

    // 6. Generate mask
    if (trails.empty())
        return std::vector<uint8_t>(numPx, 0);

    return generateMask(trails, width, height);
}

// ---------------------------------------------------------------------------
// Detect trails in all frames
// ---------------------------------------------------------------------------
std::vector<std::vector<uint8_t>> TrailDetector::detectAllFrames(
    const std::vector<std::vector<float>>& frameData,
    int width, int height) const
{
    size_t nFrames = frameData.size();
    std::vector<std::vector<uint8_t>> masks(nFrames);

    #pragma omp parallel for schedule(dynamic, 1)
    for (size_t f = 0; f < nFrames; ++f) {
        masks[f] = detectAndMask(frameData[f].data(), width, height);
    }

    return masks;
}

} // namespace nukex
