//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
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
   void AutoConfigure( double median, double mad ) override;
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
