//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "GHStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

GHStretch::GHStretch()
{
   AddParameter( AlgorithmParameter(
      "D", "Stretch (D)", 2.0, 0.0, 10.0, 2,
      "Stretch factor. 0 = linear (no stretch), higher values = more aggressive stretch."
   ) );

   AddParameter( AlgorithmParameter(
      "b", "Symmetry (b)", 0.0, -5.0, 5.0, 2,
      "Symmetry balance. Negative = more shadow stretching, Positive = more highlight stretching."
   ) );

   AddParameter( AlgorithmParameter(
      "SP", "Shadow Protection", 0.0, 0.0, 1.0, 3,
      "Protect shadow detail from being crushed. Higher = more protection."
   ) );

   AddParameter( AlgorithmParameter(
      "HP", "Highlight Protection", 0.0, 0.0, 1.0, 3,
      "Protect highlight detail from blowing out. Higher = more protection."
   ) );

   AddParameter( AlgorithmParameter(
      "LP", "Local Point", 0.0, 0.0, 1.0, 4,
      "Focus point for the stretch (typically image median)."
   ) );

   AddParameter( AlgorithmParameter(
      "BP", "Black Point", 0.0, 0.0, 0.5, 6,
      "Clip input values below this point to black."
   ) );
}

// ----------------------------------------------------------------------------

String GHStretch::Description() const
{
   return "Generalized Hyperbolic Stretch (GHS) provides sophisticated control "
          "over image stretching with separate parameters for stretch intensity, "
          "symmetry balance between shadows and highlights, and protection for "
          "both ends of the tonal range.";
}

// ----------------------------------------------------------------------------

double GHStretch::Asinh( double x ) const
{
   return std::log( x + std::sqrt( x * x + 1.0 ) );
}

// ----------------------------------------------------------------------------

double GHStretch::ComputeQ() const
{
   double d = D();
   if ( d <= 0 )
      return 1.0;
   return std::exp( d ) - 1.0;
}

// ----------------------------------------------------------------------------

double GHStretch::GHSTransform( double x ) const
{
   double d = D();
   double b = B();
   double sp = SP();
   double hp = HP();
   double lp = LP();

   if ( d <= 0.0001 )
      return x;

   double q = ComputeQ();
   double bFactor = std::pow( 10.0, b / 10.0 );

   double dx = x - lp;
   double stretched;

   if ( dx >= 0 )
   {
      double scale = (1.0 - lp);
      if ( scale <= 0 )
         scale = 1.0;

      double normDx = dx / scale;
      double stretchedNorm = Asinh( normDx * q * bFactor ) / Asinh( q * bFactor );

      if ( hp > 0 && normDx > 0.5 )
      {
         double protFactor = 1.0 - hp * (normDx - 0.5) / 0.5;
         protFactor = Clamp( protFactor, 0.0, 1.0 );
         stretchedNorm = stretchedNorm * (1.0 - hp) + normDx * hp * protFactor +
                         stretchedNorm * (1.0 - protFactor) * hp;
      }

      stretched = lp + stretchedNorm * scale;
   }
   else
   {
      double scale = lp;
      if ( scale <= 0 )
         scale = 1.0;

      double normDx = -dx / scale;
      double stretchedNorm = Asinh( normDx * q / bFactor ) / Asinh( q / bFactor );

      if ( sp > 0 && normDx > 0.5 )
      {
         double protFactor = 1.0 - sp * (normDx - 0.5) / 0.5;
         protFactor = Clamp( protFactor, 0.0, 1.0 );
         stretchedNorm = stretchedNorm * (1.0 - sp) + normDx * sp;
      }

      stretched = lp - stretchedNorm * scale;
   }

   return stretched;
}

// ----------------------------------------------------------------------------

double GHStretch::Apply( double value ) const
{
   double bp = BP();

   if ( value <= bp )
      return 0.0;

   double x = (value - bp) / (1.0 - bp);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   double result = GHSTransform( x );
   return Clamp( result );
}

// ----------------------------------------------------------------------------

void GHStretch::AutoConfigure( double median, double mad )
{
   // Black point: clip 2.5 MAD below median
   double bp = std::max( 0.0, median - 2.5 * mad );
   SetBP( bp );

   // Remap local point to BP-adjusted [0,1] space.
   // Apply() remaps input as x = (value - bp) / (1 - bp), so LP must be
   // in that same space for GHSTransform to work correctly.
   double scale = 1.0 - bp;
   double lp = (scale > 1e-10) ? (median - bp) / scale : median;
   SetLP( lp );

   // Compute D based on remapped LP — lower LP means tighter data, needs more stretch
   double d;
   if ( lp < 0.001 )
      d = 6.0;
   else if ( lp < 0.01 )
      d = 4.0 + 2.0 * (0.01 - lp) / 0.01;
   else if ( lp < 0.1 )
      d = 2.0 + 2.0 * (0.1 - lp) / 0.1;
   else
      d = 1.0 + 1.0 * (0.5 - std::min( lp, 0.5 )) / 0.5;
   SetD( d );

   // Default symmetric, no protection
   SetB( 0.0 );
   SetSP( 0.0 );
   SetHP( 0.0 );
}

// ----------------------------------------------------------------------------

void GHStretch::PresetLinear()
{
   SetD( 0.0 );
   SetB( 0.0 );
   SetSP( 0.0 );
   SetHP( 0.0 );
   SetLP( 0.5 );
   SetBP( 0.0 );
}

// ----------------------------------------------------------------------------

void GHStretch::PresetBalanced()
{
   SetD( 3.0 );
   SetB( 0.0 );
   SetSP( 0.1 );
   SetHP( 0.1 );
   SetLP( 0.1 );
   SetBP( 0.0 );
}

// ----------------------------------------------------------------------------

void GHStretch::PresetShadowBias()
{
   SetD( 4.0 );
   SetB( -2.0 );
   SetSP( 0.0 );
   SetHP( 0.3 );
   SetLP( 0.05 );
   SetBP( 0.0 );
}

// ----------------------------------------------------------------------------

void GHStretch::PresetHighlightProtect()
{
   SetD( 3.0 );
   SetB( 1.0 );
   SetSP( 0.1 );
   SetHP( 0.5 );
   SetLP( 0.15 );
   SetBP( 0.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> GHStretch::Clone() const
{
   auto clone = std::make_unique<GHStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
