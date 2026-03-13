#include "engine/FlatEstimator.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace nukex {

FlatEstimator::FlatEstimator(const FlatEstimatorConfig& config)
    : m_config(config) {}

// ---------------------------------------------------------------------------
// Median-stack across frames for a single channel
// ---------------------------------------------------------------------------
std::vector<float> FlatEstimator::medianStack(
    const std::vector<std::vector<float>>& channelData,
    int numPixels) const
{
    size_t nFrames = channelData.size();
    std::vector<float> result(numPixels);
    std::vector<float> column(nFrames);

    for (int p = 0; p < numPixels; ++p) {
        for (size_t f = 0; f < nFrames; ++f)
            column[f] = channelData[f][p];
        std::nth_element(column.begin(), column.begin() + nFrames / 2, column.end());
        result[p] = column[nFrames / 2];
    }
    return result;
}

// ---------------------------------------------------------------------------
// Separable Gaussian blur
// ---------------------------------------------------------------------------
void FlatEstimator::gaussianBlur(std::vector<float>& image,
                                  int width, int height, double sigma) const
{
    // Build 1D kernel
    int radius = static_cast<int>(std::ceil(3.0 * sigma));
    int ksize = 2 * radius + 1;
    std::vector<double> kernel(ksize);
    double sum = 0;
    for (int i = 0; i < ksize; ++i) {
        double d = i - radius;
        kernel[i] = std::exp(-0.5 * d * d / (sigma * sigma));
        sum += kernel[i];
    }
    for (int i = 0; i < ksize; ++i)
        kernel[i] /= sum;

    // Horizontal pass
    std::vector<float> temp(size_t(width) * height);
    #pragma omp parallel for schedule(dynamic, 16)
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double acc = 0;
            double wt = 0;
            for (int k = -radius; k <= radius; ++k) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= width) xx = width - 1;
                double w = kernel[k + radius];
                acc += w * image[y * width + xx];
                wt += w;
            }
            temp[y * width + x] = static_cast<float>(acc / wt);
        }
    }

    // Vertical pass
    #pragma omp parallel for schedule(dynamic, 16)
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double acc = 0;
            double wt = 0;
            for (int k = -radius; k <= radius; ++k) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= height) yy = height - 1;
                double w = kernel[k + radius];
                acc += w * temp[yy * width + x];
                wt += w;
            }
            image[y * width + x] = static_cast<float>(acc / wt);
        }
    }
}

// ---------------------------------------------------------------------------
// Apply self-flat correction to all frames
// ---------------------------------------------------------------------------
std::vector<std::vector<float>> FlatEstimator::applyCorrection(
    std::vector<std::vector<std::vector<float>>>& frameData,
    int width, int height)
{
    if (!m_config.enabled || frameData.empty())
        return {};

    size_t nFrames = frameData.size();
    size_t nChannels = frameData[0].size();
    int numPixels = width * height;

    std::vector<std::vector<float>> flats(nChannels);

    for (size_t ch = 0; ch < nChannels; ++ch) {
        // Gather this channel's data across all frames
        std::vector<std::vector<float>> channelData(nFrames);
        for (size_t f = 0; f < nFrames; ++f)
            channelData[f] = frameData[f][ch];

        // Median-stack (unregistered) to get raw flat
        std::vector<float> flat = medianStack(channelData, numPixels);

        // Smooth to remove residual star/sky structure
        gaussianBlur(flat, width, height, m_config.smoothSigma);

        // Normalize to mean = 1.0
        double flatSum = 0;
        for (int p = 0; p < numPixels; ++p)
            flatSum += flat[p];
        double flatMean = flatSum / numPixels;
        if (flatMean < 1e-10) flatMean = 1e-10;
        for (int p = 0; p < numPixels; ++p)
            flat[p] = static_cast<float>(flat[p] / flatMean);

        // Apply correction: divide each frame by the flat
        for (size_t f = 0; f < nFrames; ++f) {
            for (int p = 0; p < numPixels; ++p) {
                float fv = flat[p];
                if (fv > 0.01f)
                    frameData[f][ch][p] /= fv;
            }
        }

        flats[ch] = std::move(flat);
    }

    return flats;
}

} // namespace nukex
