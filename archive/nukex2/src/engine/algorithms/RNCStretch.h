//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// RNC (Roger N. Clark) Color Stretch Algorithm

#ifndef __RNCStretch_h
#define __RNCStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// RNC Color Stretch
//
// Based on the color-preserving stretch methodology by Roger N. Clark.
// This algorithm stretches the luminance while preserving the color ratios
// between channels, preventing color shifts and desaturation that commonly
// occur with standard stretching methods.
//
// The approach:
// 1. Calculate luminance from RGB
// 2. Stretch the luminance using a power function
// 3. Scale RGB channels to match new luminance while preserving ratios
// 4. Apply optional color boost
//
// Key benefits:
// - Preserves accurate star colors
// - Maintains nebula color integrity
// - Prevents the "washed out" look common with aggressive stretches
//
// Reference: Roger N. Clark's astrophotography techniques
// https://clarkvision.com/articles/astrophotography.image.processing/
// ----------------------------------------------------------------------------

class RNCStretch : public StretchAlgorithmBase
{
public:

   RNCStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "RNC"; }
   String Name() const override { return "RNC Color Stretch"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Special color-aware application (for full RGB processing)
   void ApplyToImageRGB( Image& image, const Image* mask = nullptr ) const;

   // Convenience accessors
   double StretchFactor() const { return GetParameter( "stretchFactor" ); }
   void SetStretchFactor( double s ) { SetParameter( "stretchFactor", s ); }

   double ColorBoost() const { return GetParameter( "colorBoost" ); }
   void SetColorBoost( double cb ) { SetParameter( "colorBoost", cb ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double SaturationProtect() const { return GetParameter( "saturationProtect" ); }
   void SetSaturationProtect( double sp ) { SetParameter( "saturationProtect", sp ); }

private:

   // Luminance calculation (Rec. 709 coefficients)
   static constexpr double LUM_R = 0.2126;
   static constexpr double LUM_G = 0.7152;
   static constexpr double LUM_B = 0.0722;

   // Power stretch function
   double PowerStretch( double x ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __RNCStretch_h
