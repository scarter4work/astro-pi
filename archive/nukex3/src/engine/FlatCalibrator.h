// ----------------------------------------------------------------------------
// FlatCalibrator.h — Median-stack flat frames, normalize, calibrate lights
//
// Accepts debayered flat frames (3-channel float arrays), median-stacks them
// into a master flat, normalizes each channel by its median pixel value, then
// applies flat calibration to light frames via per-pixel division.
//
// Copyright (c) 2026 Scott Carter
// ----------------------------------------------------------------------------

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace nukex {

using LogCallback = std::function<void( const std::string& )>;

// ---------------------------------------------------------------------------
// FlatCalibrator
// ---------------------------------------------------------------------------

class FlatCalibrator
{
public:
   // Add a debayered flat frame (3 separate channel arrays, each W*H floats, row-major).
   // All frames must have the same dimensions.
   void addFrame( const float* r, const float* g, const float* b,
                  int width, int height );

   // Median-stack all added frames per channel, then normalize
   // (divide by channel median so values ~ 1.0). Call after all frames added.
   void buildMasterFlat( LogCallback log = nullptr );

   // Apply flat calibration: divide each channel pixel by master flat.
   // Frame dimensions must match the master flat.
   void calibrate( float* r, float* g, float* b,
                   int width, int height ) const;

   bool isReady()    const { return m_ready; }
   int  frameCount() const { return m_frameCount; }
   int  width()      const { return m_width; }
   int  height()     const { return m_height; }

private:
   std::vector<std::vector<float>> m_framesR, m_framesG, m_framesB;
   std::vector<float> m_masterR, m_masterG, m_masterB;
   int  m_width      = 0;
   int  m_height     = 0;
   int  m_frameCount = 0;
   bool m_ready      = false;

   // Prevent division by near-zero in the master flat
   static constexpr float MIN_FLAT_VALUE = 0.01f;

   // Maximum correction factor — beyond this, vignetting is so severe
   // that correcting amplifies noise more than it helps
   static constexpr float MAX_FLAT_CORRECTION = 1.40f;
};

} // namespace nukex
