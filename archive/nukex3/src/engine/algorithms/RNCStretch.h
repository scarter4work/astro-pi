//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
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
// Based on Roger N. Clark's color-preserving stretch methodology.
// Stretches luminance while preserving color ratios between channels.
// ----------------------------------------------------------------------------

class RNCStretch : public StretchAlgorithmBase
{
public:

   RNCStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "RNC"; }
   String Name() const override { return "RNC Color Stretch"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Special color-aware application (for full RGB processing)
   void ApplyToImageRGB( Image& image, const Image* mask = nullptr ) const;

   double StretchFactor() const { return GetParameter( "stretchFactor" ); }
   void SetStretchFactor( double s ) { SetParameter( "stretchFactor", s ); }

   double ColorBoost() const { return GetParameter( "colorBoost" ); }
   void SetColorBoost( double cb ) { SetParameter( "colorBoost", cb ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double SaturationProtect() const { return GetParameter( "saturationProtect" ); }
   void SetSaturationProtect( double sp ) { SetParameter( "saturationProtect", sp ); }

private:

   static constexpr double LUM_R = 0.2126;
   static constexpr double LUM_G = 0.7152;
   static constexpr double LUM_B = 0.0722;

   double PowerStretch( double x ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __RNCStretch_h
