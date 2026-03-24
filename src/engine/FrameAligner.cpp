#include "FrameAligner.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nukex {

// Bilinear interpolation at sub-pixel position (sx, sy) in a row-major image.
// Returns false if the position is outside the frame (caller should mask).
static bool bilinearSample(const float* src, int width, int height,
                           double sx, double sy, float& out)
{
    if (sx < 0 || sy < 0 || sx >= width - 1 || sy >= height - 1) {
        // Allow exact edge: clamp to avoid out-of-bounds
        if (sx < -0.5 || sy < -0.5 || sx > width - 0.5 || sy > height - 0.5)
            return false;
        sx = std::max(0.0, std::min(sx, double(width - 1)));
        sy = std::max(0.0, std::min(sy, double(height - 1)));
    }

    int x0 = static_cast<int>(sx);
    int y0 = static_cast<int>(sy);
    int x1 = std::min(x0 + 1, width - 1);
    int y1 = std::min(y0 + 1, height - 1);

    double fx = sx - x0;
    double fy = sy - y0;

    float v00 = src[y0 * width + x0];
    float v10 = src[y0 * width + x1];
    float v01 = src[y1 * width + x0];
    float v11 = src[y1 * width + x1];

    out = static_cast<float>(
        v00 * (1 - fx) * (1 - fy) +
        v10 * fx * (1 - fy) +
        v01 * (1 - fx) * fy +
        v11 * fx * fy);
    return true;
}

// Compute inverse similarity transform: given reference coords (rx, ry),
// find source coords (sx, sy) in the target frame.
static void inverseTransform(const AlignmentResult& t, int srcWidth,
                             double rx, double ry, double& sx, double& sy)
{
    double det = t.a * t.a + t.b * t.b;
    if (det < 1e-12) { sx = sy = -1; return; }
    double dx = rx - t.tx;
    double dy = ry - t.ty;
    sx = (t.a * dx + t.b * dy) / det;
    sy = (-t.b * dx + t.a * dy) / det;
    if (t.flipped)
        sx = (srcWidth - 1) - sx;
}

CropRegion computeCropRegion(const std::vector<AlignmentResult>& offsets,
                              int originalWidth, int originalHeight) {
    // Use only the translation component (dx, dy) of valid frames for crop.
    // Rotated/flipped frames handle out-of-bounds via per-pixel masking.
    int maxDx = 0, maxDy = 0, minDx = 0, minDy = 0;
    for (const auto& o : offsets) {
        if (!o.valid) continue;
        maxDx = std::max(maxDx, o.dx);
        maxDy = std::max(maxDy, o.dy);
        minDx = std::min(minDx, o.dx);
        minDy = std::min(minDy, o.dy);
    }

    CropRegion crop;
    crop.x0 = std::max(0, -minDx);
    crop.y0 = std::max(0, -minDy);
    crop.x1 = std::min(originalWidth, originalWidth - maxDx);
    crop.y1 = std::min(originalHeight, originalHeight - maxDy);

    if (crop.width() <= 0 || crop.height() <= 0) {
        // Fallback: use reference frame footprint (no crop)
        crop.x0 = 0;
        crop.y0 = 0;
        crop.x1 = originalWidth;
        crop.y1 = originalHeight;
    }

    return crop;
}

AlignmentOutput alignFrames(const std::vector<const float*>& frameData,
                             int width, int height,
                             int referenceIdx,
                             const DetectorConfig& detConfig,
                             int matchMaxStars) {
    size_t nFrames = frameData.size();
    if (nFrames < 2)
        throw std::invalid_argument("FrameAligner: need at least 2 frames");
    if (referenceIdx < 0 || static_cast<size_t>(referenceIdx) >= nFrames)
        throw std::invalid_argument("FrameAligner: referenceIdx out of range");

    // 1. Detect stars in all frames
    std::vector<std::vector<StarPosition>> starLists(nFrames);
    for (size_t i = 0; i < nFrames; ++i)
        starLists[i] = detectStars(frameData[i], width, height, detConfig);

    // 2. Match each frame against reference (similarity transform + flip detection)
    std::vector<AlignmentResult> offsets(nFrames);
    offsets[referenceIdx].a = 1.0;
    offsets[referenceIdx].b = 0.0;
    offsets[referenceIdx].tx = 0.0;
    offsets[referenceIdx].ty = 0.0;
    offsets[referenceIdx].scale = 1.0;
    offsets[referenceIdx].rotation = 0.0;
    offsets[referenceIdx].numMatchedStars = static_cast<int>(starLists[referenceIdx].size());
    offsets[referenceIdx].valid = true;

    for (size_t i = 0; i < nFrames; ++i) {
        if (static_cast<int>(i) == referenceIdx) continue;
        offsets[i] = matchFrames(starLists[referenceIdx], starLists[i],
                                  width, matchMaxStars);

        // Frame rejection: RMS too high or scale anomaly
        if (offsets[i].valid) {
            if (offsets[i].convergenceRMS > 2.0 ||
                offsets[i].scale < 0.95 || offsets[i].scale > 1.05) {
                offsets[i].valid = false;
            }
        }
    }

    // 3. Compute crop region from valid frames' translations
    CropRegion crop = computeCropRegion(offsets, width, height);

    // 4. Allocate aligned SubCube + masks, apply transforms with bilinear resampling
    SubCube cube(nFrames, crop.height(), crop.width());
    cube.allocateMasks();

    for (size_t i = 0; i < nFrames; ++i) {
        if (!offsets[i].valid) {
            // Invalid frame: mask all pixels
            for (int cy = 0; cy < crop.height(); ++cy)
                for (int cx = 0; cx < crop.width(); ++cx)
                    cube.setMask(i, cy, cx, 1);
            continue;
        }

        const AlignmentResult& t = offsets[i];
        bool isIdentity = (std::abs(t.a - 1.0) < 1e-9 && std::abs(t.b) < 1e-9 && !t.flipped);

        for (int cy = 0; cy < crop.height(); ++cy) {
            for (int cx = 0; cx < crop.width(); ++cx) {
                double refX = crop.x0 + cx;
                double refY = crop.y0 + cy;

                if (isIdentity) {
                    // Pure translation — use integer shift (preserves noise stats)
                    int srcX = static_cast<int>(refX - t.tx + 0.5);
                    int srcY = static_cast<int>(refY - t.ty + 0.5);
                    if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height)
                        cube.setPixel(i, cy, cx, frameData[i][srcY * width + srcX]);
                    else
                        cube.setMask(i, cy, cx, 1);
                } else {
                    // Similarity transform — bilinear resampling
                    double sx, sy;
                    inverseTransform(t, width, refX, refY, sx, sy);

                    float val;
                    if (bilinearSample(frameData[i], width, height, sx, sy, val))
                        cube.setPixel(i, cy, cx, val);
                    else
                        cube.setMask(i, cy, cx, 1);
                }
            }
        }
    }

    return AlignmentOutput{std::move(cube), std::move(offsets), crop, referenceIdx};
}

SubCube applyAlignment(const std::vector<std::vector<float>>& channelFrameData,
                       const std::vector<AlignmentResult>& offsets,
                       const CropRegion& crop, int width, int height) {
    size_t nFrames = channelFrameData.size();
    SubCube cube(nFrames, crop.height(), crop.width());
    cube.allocateMasks();

    for (size_t i = 0; i < nFrames; ++i) {
        if (!offsets[i].valid) {
            for (int cy = 0; cy < crop.height(); ++cy)
                for (int cx = 0; cx < crop.width(); ++cx)
                    cube.setMask(i, cy, cx, 1);
            continue;
        }

        const AlignmentResult& t = offsets[i];
        const float* src = channelFrameData[i].data();
        bool isIdentity = (std::abs(t.a - 1.0) < 1e-9 && std::abs(t.b) < 1e-9 && !t.flipped);

        for (int cy = 0; cy < crop.height(); ++cy) {
            for (int cx = 0; cx < crop.width(); ++cx) {
                double refX = crop.x0 + cx;
                double refY = crop.y0 + cy;

                if (isIdentity) {
                    int srcX = static_cast<int>(refX - t.tx + 0.5);
                    int srcY = static_cast<int>(refY - t.ty + 0.5);
                    if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height)
                        cube.setPixel(i, cy, cx, src[srcY * width + srcX]);
                    else
                        cube.setMask(i, cy, cx, 1);
                } else {
                    double sx, sy;
                    inverseTransform(t, width, refX, refY, sx, sy);

                    float val;
                    if (bilinearSample(src, width, height, sx, sy, val))
                        cube.setPixel(i, cy, cx, val);
                    else
                        cube.setMask(i, cy, cx, 1);
                }
            }
        }
    }

    return cube;
}

} // namespace nukex
