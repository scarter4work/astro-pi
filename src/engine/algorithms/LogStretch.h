//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Logarithmic Stretch Algorithm

#ifndef __LogStretch_h
#define __LogStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Logarithmic Stretch
//
// Formula: stretched = log(1 + scale * x) / log(1 + scale)
// Excellent for revealing very faint detail (IFN, outer galaxy halos).
// ----------------------------------------------------------------------------

class LogStretch : public StretchAlgorithmBase
{
public:

   LogStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "Log"; }
   String Name() const override { return "Logarithmic Stretch"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   double Scale() const { return GetParameter( "scale" ); }
   void SetScale( double s ) { SetParameter( "scale", s ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double HighlightProtection() const { return GetParameter( "highlightProtection" ); }
   void SetHighlightProtection( double hp ) { SetParameter( "highlightProtection", hp ); }

private:

   mutable double m_normFactor = 1.0;
   mutable double m_lastScale = -1.0;

   void UpdateNormFactor() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __LogStretch_h
