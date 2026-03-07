#pragma once

#include "StarDetector.h"
#include "TriangleMatcher.h"
#include "SubCube.h"

#include <vector>

namespace nukex {

struct CropRegion {
    int x0, y0;  // top-left of overlap region
    int x1, y1;  // bottom-right (exclusive)
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

struct AlignmentOutput {
    SubCube alignedCube;
    std::vector<AlignmentResult> offsets;
    CropRegion crop;
    int referenceFrame;
};

// Compute the crop bounding box from alignment offsets
CropRegion computeCropRegion(const std::vector<AlignmentResult>& offsets,
                              int originalWidth, int originalHeight);

// Full alignment pipeline
AlignmentOutput alignFrames(const std::vector<const float*>& frameData,
                             int width, int height,
                             int referenceIdx = 0,
                             const DetectorConfig& detConfig = DetectorConfig{},
                             int matchMaxStars = 50);

} // namespace nukex
