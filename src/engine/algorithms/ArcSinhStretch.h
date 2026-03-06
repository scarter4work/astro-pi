//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
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
// Formula: stretched = asinh(x * beta) / asinh(beta)
// Excellent for HDR images, protecting bright star cores.
// ----------------------------------------------------------------------------

class ArcSinhStretch : public StretchAlgorithmBase
{
public:

   ArcSinhStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "ArcSinh"; }
   String Name() const override { return "Inverse Hyperbolic Sine"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   double Beta() const { return GetParameter( "beta" ); }
   void SetBeta( double b ) { SetParameter( "beta", b ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double Stretch() const { return GetParameter( "stretch" ); }
   void SetStretch( double s ) { SetParameter( "stretch", s ); }

private:

   mutable double m_normFactor = 1.0;
   mutable double m_lastBeta = -1.0;

   void UpdateNormFactor() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __ArcSinhStretch_h
