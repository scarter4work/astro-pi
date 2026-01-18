//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Generalized Hyperbolic Stretch (GHS) Algorithm

#ifndef __GHStretch_h
#define __GHStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Generalized Hyperbolic Stretch (GHS)
//
// A sophisticated stretch algorithm that provides fine-grained control over
// how different tonal ranges are stretched. Based on the work by Mike Cranfield.
//
// Key Parameters:
// - D (Stretch Factor): Overall stretch amount (0 = linear, higher = more stretch)
// - b (Symmetry): Balance between shadow and highlight stretching
//                 b < 0: More shadow stretching
//                 b = 0: Symmetric
//                 b > 0: More highlight stretching
// - SP (Shadow Protection): Preserve shadow detail (0-1)
// - HP (Highlight Protection): Preserve highlight detail (0-1)
// - LP (Local Point): Focus point for local stretch (typically image median)
// - BP (Black Point): Clip shadows below this value
//
// This is the same algorithm used in the popular GHS tool for PixInsight.
// Reference: https://ghsastro.co.uk/
// ----------------------------------------------------------------------------

class GHStretch : public StretchAlgorithmBase
{
public:

   GHStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "GHS"; }
   String Name() const override { return "Generalized Hyperbolic Stretch"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
   double D() const { return GetParameter( "D" ); }
   void SetD( double d ) { SetParameter( "D", d ); }

   double B() const { return GetParameter( "b" ); }
   void SetB( double b ) { SetParameter( "b", b ); }

   double SP() const { return GetParameter( "SP" ); }
   void SetSP( double sp ) { SetParameter( "SP", sp ); }

   double HP() const { return GetParameter( "HP" ); }
   void SetHP( double hp ) { SetParameter( "HP", hp ); }

   double LP() const { return GetParameter( "LP" ); }
   void SetLP( double lp ) { SetParameter( "LP", lp ); }

   double BP() const { return GetParameter( "BP" ); }
   void SetBP( double bp ) { SetParameter( "BP", bp ); }

   // Preset configurations
   void PresetLinear();
   void PresetBalanced();
   void PresetShadowBias();
   void PresetHighlightProtect();

private:

   // Core GHS transformation
   double GHSTransform( double x ) const;

   // Helper functions
   double Asinh( double x ) const;
   double ComputeQ() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __GHStretch_h
