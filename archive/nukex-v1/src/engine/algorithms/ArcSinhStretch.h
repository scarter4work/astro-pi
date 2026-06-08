//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Inverse Hyperbolic Sine (ArcSinh) Stretch Algorithm

#ifndef __ArcSinhStretch_h
#define __ArcSinhStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// ArcSinh (Inverse Hyperbolic Sine) Stretch
//
// The arcsinh function provides a logarithmic-like stretch that is excellent
// for high dynamic range images. Unlike log, it's defined at zero and has
// better behavior for bright regions.
//
// Formula: stretched = asinh(x * beta) / asinh(beta)
//
// Where:
//   x = input pixel value (normalized 0-1)
//   beta = stretch factor (higher = more aggressive stretch)
//
// This stretch is particularly good for:
//   - Protecting bright star cores from blowing out
//   - High dynamic range scenes
//   - Galaxy cores
//
// The function maps:
//   - 0 -> 0
//   - 1 -> 1
//   - Low values are stretched more than high values
// ----------------------------------------------------------------------------

class ArcSinhStretch : public StretchAlgorithmBase
{
public:

   ArcSinhStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "ArcSinh"; }
   String Name() const override { return "Inverse Hyperbolic Sine"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
   double Beta() const { return GetParameter( "beta" ); }
   void SetBeta( double b ) { SetParameter( "beta", b ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double Stretch() const { return GetParameter( "stretch" ); }
   void SetStretch( double s ) { SetParameter( "stretch", s ); }

private:

   // Cached normalization factor
   mutable double m_normFactor = 1.0;
   mutable double m_lastBeta = -1.0;

   void UpdateNormFactor() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __ArcSinhStretch_h
