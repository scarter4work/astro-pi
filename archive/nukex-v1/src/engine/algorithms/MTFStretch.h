//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Midtones Transfer Function (MTF) Stretch Algorithm

#ifndef __MTFStretch_h
#define __MTFStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Midtones Transfer Function (MTF) Stretch
//
// The classic PixInsight midtones transfer function.
// Maps input values through a non-linear curve controlled by the midtones
// balance parameter.
//
// Formula: MTF(x, m) = (m - 1) * x / ((2*m - 1) * x - m)
//
// Where:
//   x = input pixel value (0-1)
//   m = midtones balance (0-1)
//       m < 0.5 = lighten (shadows lift)
//       m = 0.5 = no change
//       m > 0.5 = darken (highlights compress)
// ----------------------------------------------------------------------------

class MTFStretch : public StretchAlgorithmBase
{
public:

   MTFStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "MTF"; }
   String Name() const override { return "Midtones Transfer Function"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
   double Midtones() const { return GetParameter( "midtones" ); }
   void SetMidtones( double m ) { SetParameter( "midtones", m ); }

   double ShadowsClip() const { return GetParameter( "shadowsClip" ); }
   void SetShadowsClip( double s ) { SetParameter( "shadowsClip", s ); }

   double HighlightsClip() const { return GetParameter( "highlightsClip" ); }
   void SetHighlightsClip( double h ) { SetParameter( "highlightsClip", h ); }

   double TargetMedian() const { return GetParameter( "targetMedian" ); }
   void SetTargetMedian( double t ) { SetParameter( "targetMedian", t ); }

   // Static utility function for MTF calculation
   static double MTF( double x, double m );

   // Calculate midtones balance to achieve target median from current median
   static double CalculateMidtonesBalance( double currentMedian, double targetMedian );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __MTFStretch_h
