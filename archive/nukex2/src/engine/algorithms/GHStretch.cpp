//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "GHStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

GHStretch::GHStretch()
{
   // D - Stretch factor (logarithmic scale internally)
   AddParameter( AlgorithmParameter(
      "D",
      "Stretch (D)",
      2.0,      // default
      0.0,      // min (0 = linear)
      10.0,     // max
      2,        // precision
      "Stretch factor. 0 = linear (no stretch), higher values = more aggressive stretch."
   ) );

   // b - Symmetry parameter
   AddParameter( AlgorithmParameter(
      "b",
      "Symmetry (b)",
      0.0,      // default (symmetric)
      -5.0,     // min (shadow bias)
      5.0,      // max (highlight bias)
      2,        // precision
      "Symmetry balance. Negative = more shadow stretching, Positive = more highlight stretching."
   ) );

   // SP - Shadow protection
   AddParameter( AlgorithmParameter(
      "SP",
      "Shadow Protection",
      0.0,      // default
      0.0,      // min
      1.0,      // max
      3,        // precision
      "Protect shadow detail from being crushed. Higher = more protection."
   ) );

   // HP - Highlight protection
   AddParameter( AlgorithmParameter(
      "HP",
      "Highlight Protection",
      0.0,      // default
      0.0,      // min
      1.0,      // max
      3,        // precision
      "Protect highlight detail from blowing out. Higher = more protection."
   ) );

   // LP - Local/focus point
   AddParameter( AlgorithmParameter(
      "LP",
      "Local Point",
      0.0,      // default (will be set to median)
      0.0,      // min
      1.0,      // max
      4,        // precision
      "Focus point for the stretch (typically image median). "
      "The stretch is centered around this value."
   ) );

   // BP - Black point
   AddParameter( AlgorithmParameter(
      "BP",
      "Black Point",
      0.0,      // default
      0.0,      // min
      0.5,      // max
      6,        // precision
      "Clip input values below this point to black."
   ) );
}

// ----------------------------------------------------------------------------

String GHStretch::Description() const
{
   return "Generalized Hyperbolic Stretch (GHS) provides sophisticated control "
          "over image stretching with separate parameters for stretch intensity, "
          "symmetry balance between shadows and highlights, and protection for "
          "both ends of the tonal range. This algorithm is excellent for achieving "
          "well-balanced stretches that preserve detail across the entire image.";
}

// ----------------------------------------------------------------------------

double GHStretch::Asinh( double x ) const
{
   return std::log( x + std::sqrt( x * x + 1.0 ) );
}

// ----------------------------------------------------------------------------

double GHStretch::ComputeQ() const
{
   // Q factor for normalization based on D
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

   // If D is 0, return linear
   if ( d <= 0.0001 )
      return x;

   // Compute transformation parameters
   double q = ComputeQ();

   // Apply symmetry (b parameter) to shift the stretch curve
   // Positive b shifts stretch toward highlights
   // Negative b shifts stretch toward shadows
   double bFactor = std::pow( 10.0, b / 10.0 );

   // Compute the stretched value using GHS formula
   // This is a simplified but effective implementation

   // Calculate distance from local point
   double dx = x - lp;

   // Apply stretch with symmetry
   double stretched;
   if ( dx >= 0 )
   {
      // Above local point - stretch toward highlights
      double scale = (1.0 - lp);
      if ( scale <= 0 )
         scale = 1.0;

      double normDx = dx / scale;

      // Apply hyperbolic stretch
      double stretchedNorm = Asinh( normDx * q * bFactor ) / Asinh( q * bFactor );

      // Apply highlight protection
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
      // Below local point - stretch shadows
      double scale = lp;
      if ( scale <= 0 )
         scale = 1.0;

      double normDx = -dx / scale;

      // Apply hyperbolic stretch (inverted for shadows)
      double stretchedNorm = Asinh( normDx * q / bFactor ) / Asinh( q / bFactor );

      // Apply shadow protection
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

   // Apply black point clipping
   if ( value <= bp )
      return 0.0;

   // Normalize for black point
   double x = (value - bp) / (1.0 - bp);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   // Apply GHS transformation
   double result = GHSTransform( x );

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void GHStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Set local point to image median
   double lp = stats.median;
   SetLP( lp );

   // Calculate D based on how dark the image is
   double d;
   if ( lp < 0.001 )
   {
      // Very dark - aggressive stretch
      d = 6.0;
   }
   else if ( lp < 0.01 )
   {
      d = 4.0 + 2.0 * (0.01 - lp) / 0.01;
   }
   else if ( lp < 0.1 )
   {
      d = 2.0 + 2.0 * (0.1 - lp) / 0.1;
   }
   else
   {
      // Already fairly bright
      d = 1.0 + 1.0 * (0.5 - std::min( lp, 0.5 )) / 0.5;
   }
   SetD( d );

   // Set symmetry based on dynamic range and content
   double b = 0.0; // Start symmetric

   if ( stats.max > 0.8 && stats.median < 0.1 )
   {
      // High dynamic range with bright spots - protect highlights
      b = 1.0;
   }
   else if ( stats.median < 0.01 && stats.max < 0.1 )
   {
      // Very faint image - bias toward shadows
      b = -1.0;
   }
   SetB( b );

   // Set black point
   double bp = std::max( 0.0, stats.median - 2.5 * stats.mad );
   SetBP( bp );

   // Set protections based on content
   double sp = 0.0;
   double hp = 0.0;

   if ( stats.snrEstimate < 10.0 )
   {
      // Low SNR - protect shadows to avoid noise amplification
      sp = 0.2 * (10.0 - stats.snrEstimate) / 10.0;
   }

   if ( stats.clippingPct > 0.1 || stats.max > 0.9 )
   {
      // Risk of highlight blowout
      hp = 0.3;
   }

   SetSP( sp );
   SetHP( hp );
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
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
