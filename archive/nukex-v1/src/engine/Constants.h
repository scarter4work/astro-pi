//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Constants - Shared constants and thresholds

#ifndef __Constants_h
#define __Constants_h

namespace pcl
{

// ----------------------------------------------------------------------------
// Mathematical Constants
// ----------------------------------------------------------------------------

namespace NukeXConstants
{
   // Small value for avoiding division by zero
   constexpr double Epsilon = 1e-10;

   // Weight threshold for mask significance
   constexpr double MinWeight = 0.001;

   // Threshold for near-zero comparisons
   constexpr double NearZero = 0.001;
}

// ----------------------------------------------------------------------------
// Luminance Coefficients (Rec. 709 / sRGB)
// ----------------------------------------------------------------------------

namespace LuminanceCoeff
{
   // Standard Rec. 709 luminance coefficients
   constexpr double R = 0.2126;
   constexpr double G = 0.7152;
   constexpr double B = 0.0722;

   // Alternative coefficients (Rec. 601 / NTSC)
   namespace Rec601
   {
      constexpr double R = 0.299;
      constexpr double G = 0.587;
      constexpr double B = 0.114;
   }

   // Equal weight
   namespace Equal
   {
      constexpr double R = 0.333333;
      constexpr double G = 0.333333;
      constexpr double B = 0.333333;
   }
}

// ----------------------------------------------------------------------------
// Region Classification Thresholds
// ----------------------------------------------------------------------------

namespace RegionThresholds
{
   // Star detection
   constexpr double StarCoreMedian = 0.9;        // Very bright core
   constexpr double StarHaloMedian = 0.5;        // Moderate brightness
   constexpr double StarMinSNR = 50.0;           // High SNR for stars

   // Nebula detection
   constexpr double NebulaBrightMedian = 0.3;    // Bright nebula threshold
   constexpr double NebulaFaintMax = 0.15;       // Faint nebula upper limit
   constexpr double NebulaMinSNR = 5.0;          // Minimum SNR for nebula

   // Background detection
   constexpr double BackgroundMax = 0.05;        // Max median for background
   constexpr double BackgroundMaxSNR = 3.0;      // Low SNR for background

   // Galaxy detection
   constexpr double GalaxyCoreMedian = 0.7;      // Bright galactic nucleus
   constexpr double GalaxyHaloMax = 0.2;         // Faint halo threshold
   constexpr double GalaxyArmMedian = 0.25;      // Spiral arm brightness

   // General thresholds
   constexpr double HighSNR = 20.0;              // High signal-to-noise
   constexpr double LowSNR = 5.0;                // Low signal-to-noise
   constexpr double HighDynamicRange = 0.5;      // Wide dynamic range
   constexpr double LowDynamicRange = 0.1;       // Narrow dynamic range
}

// ----------------------------------------------------------------------------
// Algorithm Selection Confidence
// ----------------------------------------------------------------------------

namespace SelectionConfidence
{
   constexpr double VeryHigh = 0.95;
   constexpr double High = 0.85;
   constexpr double Medium = 0.70;
   constexpr double Low = 0.50;
   constexpr double VeryLow = 0.30;
}

// ----------------------------------------------------------------------------
// Performance Tuning
// ----------------------------------------------------------------------------

namespace PerformanceDefaults
{
   // Segmentation
   constexpr int MaxSegmentationDimension = 1024;
   constexpr int PreviewMaxDimension = 512;

   // Blending
   constexpr double DefaultFeatherRadius = 5.0;
   constexpr double DefaultBlendFalloff = 1.0;

   // LUT
   constexpr int DefaultLUTSize = 65536;

   // Local contrast
   constexpr int DefaultLocalContrastRadius = 64;
}

// ----------------------------------------------------------------------------
// Color Space Constants
// ----------------------------------------------------------------------------

namespace ColorSpaceConstants
{
   // D65 illuminant reference white
   constexpr double D65_Xn = 0.95047;
   constexpr double D65_Yn = 1.0;
   constexpr double D65_Zn = 1.08883;

   // Lab conversion constants
   constexpr double LabDelta = 6.0 / 29.0;
   constexpr double LabDelta3 = LabDelta * LabDelta * LabDelta;
   constexpr double LabK = 1.0 / (3.0 * LabDelta * LabDelta);
   constexpr double Lab16_116 = 16.0 / 116.0;
}

// ----------------------------------------------------------------------------
// Interpolation Utilities
// ----------------------------------------------------------------------------

namespace Interpolation
{
   // Bilinear interpolation of 4 corner values given fractional x,y position
   inline double BilinearInterpolate( double v00, double v10, double v01, double v11,
                                       double fx, double fy )
   {
      return v00 * (1.0 - fx) * (1.0 - fy) +
             v10 * fx * (1.0 - fy) +
             v01 * (1.0 - fx) * fy +
             v11 * fx * fy;
   }
}

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __Constants_h
