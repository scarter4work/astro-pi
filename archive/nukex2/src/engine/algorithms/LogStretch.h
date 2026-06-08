//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
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
// Applies a logarithmic transformation that dramatically compresses the
// dynamic range, making it excellent for revealing faint details.
//
// Formula: stretched = log(1 + scale * x) / log(1 + scale)
//
// Where:
//   x = input pixel value (normalized 0-1)
//   scale = stretch factor (higher = more compression of highlights)
//
// This stretch is particularly good for:
//   - Revealing very faint nebulosity (IFN, outer galaxy halos)
//   - Faint extended emission
//   - Any region where you want maximum lift of faint detail
//
// Caution: Can blow out bright regions if used aggressively.
// ----------------------------------------------------------------------------

class LogStretch : public StretchAlgorithmBase
{
public:

   LogStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "Log"; }
   String Name() const override { return "Logarithmic Stretch"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
   double Scale() const { return GetParameter( "scale" ); }
   void SetScale( double s ) { SetParameter( "scale", s ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double HighlightProtection() const { return GetParameter( "highlightProtection" ); }
   void SetHighlightProtection( double hp ) { SetParameter( "highlightProtection", hp ); }

private:

   // Cached normalization factor
   mutable double m_normFactor = 1.0;
   mutable double m_lastScale = -1.0;

   void UpdateNormFactor() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __LogStretch_h
