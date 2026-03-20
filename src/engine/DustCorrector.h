// ----------------------------------------------------------------------------
// DustCorrector.h — Edge-referenced radial profile dust mote correction
//
// For each detected dust blob, samples an annular ring of edge pixels just
// outside the mote boundary, builds a per-angle radial correction profile,
// smooths it, and applies a per-pixel multiplicative correction.
//
// Copyright (c) 2026 Scott Carter
// ----------------------------------------------------------------------------

#pragma once

#include "engine/ArtifactDetector.h"  // for DustBlobInfo, LogCallback

#include <vector>

namespace nukex {

// ---------------------------------------------------------------------------
// DustCorrector
// ---------------------------------------------------------------------------

class DustCorrector
{
public:
   void correct( float* image, int width, int height,
                 const std::vector<DustBlobInfo>& blobs,
                 LogCallback log = nullptr ) const;

private:
   // Number of angular bins (5 degrees each, 360/5 = 72)
   static constexpr int ANGULAR_BINS = 72;

   // Edge sampling annulus: [R + EDGE_INNER, R + EDGE_OUTER] pixels from center
   static constexpr int EDGE_INNER   = 2;
   static constexpr int EDGE_OUTER   = 5;

   // Safety clamp — never boost a pixel more than this factor
   static constexpr float MAX_CORRECTION = 1.5f;
};

} // namespace nukex
