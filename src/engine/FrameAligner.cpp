#include "FrameAligner.h"
#include <algorithm>
#include <stdexcept>

namespace nukex {

CropRegion computeCropRegion(const std::vector<AlignmentResult>& offsets,
                              int originalWidth, int originalHeight) {
    int maxDx = 0, maxDy = 0, minDx = 0, minDy = 0;
    for (const auto& o : offsets) {
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

    if (crop.width() <= 0 || crop.height() <= 0)
        throw std::runtime_error("FrameAligner: no overlap region — offsets too large");

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

    // 2. Match each frame against reference
    std::vector<AlignmentResult> offsets(nFrames);
    offsets[referenceIdx] = {0, 0, static_cast<int>(starLists[referenceIdx].size()), 0.0, true};

    for (size_t i = 0; i < nFrames; ++i) {
        if (static_cast<int>(i) == referenceIdx) continue;
        offsets[i] = matchFrames(starLists[referenceIdx], starLists[i], matchMaxStars);
        if (!offsets[i].valid) {
            // Alignment failed — keep frame with zero offset but mark as unaligned.
            // We use ALL data: the distribution fitting pipeline handles the noise.
            offsets[i] = {0, 0, 0, 0.0, false};
        }
    }

    // 3. Compute crop region
    CropRegion crop = computeCropRegion(offsets, width, height);

    // 4. Allocate aligned SubCube and copy shifted+cropped data
    SubCube cube(nFrames, crop.height(), crop.width());

    for (size_t i = 0; i < nFrames; ++i) {
        int dx = offsets[i].dx;
        int dy = offsets[i].dy;

        for (int cy = 0; cy < crop.height(); ++cy) {
            for (int cx = 0; cx < crop.width(); ++cx) {
                int srcX = crop.x0 + cx + dx;
                int srcY = crop.y0 + cy + dy;

                if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height) {
                    cube.setPixel(i, cy, cx, frameData[i][srcY * width + srcX]);
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

    for (size_t i = 0; i < nFrames; ++i) {
        int dx = offsets[i].dx;
        int dy = offsets[i].dy;
        const float* src = channelFrameData[i].data();

        for (int cy = 0; cy < crop.height(); ++cy) {
            for (int cx = 0; cx < crop.width(); ++cx) {
                int srcX = crop.x0 + cx + dx;
                int srcY = crop.y0 + cy + dy;
                if (srcX >= 0 && srcX < width && srcY >= 0 && srcY < height)
                    cube.setPixel(i, cy, cx, src[srcY * width + srcX]);
            }
        }
    }

    return cube;
}

} // namespace nukex
