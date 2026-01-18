//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "OTSStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

OTSStretch::OTSStretch()
{
   // Target median - where to place the median after stretch
   AddParameter( AlgorithmParameter(
      "targetMedian",
      "Target Median",
      0.25,     // default
      0.1,      // min
      0.5,      // max
      3,        // precision
      "Target median brightness after stretch. 0.25 is typical for "
      "well-stretched astrophotography."
   ) );

   // Black point
   AddParameter( AlgorithmParameter(
      "blackPoint",
      "Black Point",
      0.0,      // default (auto-calculated)
      0.0,      // min
      0.5,      // max
      6,        // precision
      "Background level to clip to black. Set to 0 for auto-detection."
   ) );

   // Shadows adjustment
   AddParameter( AlgorithmParameter(
      "shadows",
      "Shadows",
      0.0,      // default
      -1.0,     // min (lift shadows)
      1.0,      // max (crush shadows)
      2,        // precision
      "Shadow adjustment. Negative values lift shadows, positive crush them."
   ) );

   // Highlights adjustment
   AddParameter( AlgorithmParameter(
      "highlights",
      "Highlights",
      0.0,      // default
      -1.0,     // min (compress highlights)
      1.0,      // max (expand highlights)
      2,        // precision
      "Highlight adjustment. Negative compresses, positive expands."
   ) );

   // Curve shape - blend between MTF and power curve
   AddParameter( AlgorithmParameter(
      "curveShape",
      "Curve Shape",
      0.5,      // default (balanced)
      0.0,      // min (pure MTF)
      1.0,      // max (pure power)
      2,        // precision
      "Shape of the transfer curve. 0 = MTF-like, 1 = power curve-like."
   ) );
}

// ----------------------------------------------------------------------------

String OTSStretch::Description() const
{
   return "Optimal Transfer Stretch automatically calculates the best stretch "
          "parameters based on image statistics. It aims to bring the image "
          "median to a target brightness while preserving detail across the "
          "full tonal range. This is an excellent starting point for most "
          "astrophotography images.";
}

// ----------------------------------------------------------------------------

double OTSStretch::MTF( double x, double m ) const
{
   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;
   if ( std::abs( m - 0.5 ) < 1e-10 )
      return x;

   return (m - 1.0) * x / ((2.0 * m - 1.0) * x - m);
}

// ----------------------------------------------------------------------------

void OTSStretch::UpdateTransferFunction() const
{
   if ( !m_needsUpdate )
      return;

   // Calculate midtones balance to achieve target median
   // This is done during AutoConfigure, but we cache it here

   m_needsUpdate = false;
}

// ----------------------------------------------------------------------------

double OTSStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double targetMedian = TargetMedian();
   double shadows = Shadows();
   double highlights = Highlights();
   double curveShape = CurveShape();

   // Apply black point
   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   // Apply shadows adjustment (affects low values more)
   if ( std::abs( shadows ) > 0.001 )
   {
      if ( shadows < 0 )
      {
         // Lift shadows - apply power < 1
         double shadowLift = 1.0 + shadows * 0.5; // 0.5 to 1.0
         x = std::pow( x, shadowLift );
      }
      else
      {
         // Crush shadows - apply power > 1
         double shadowCrush = 1.0 + shadows * 2.0; // 1.0 to 3.0
         if ( x < 0.5 )
         {
            double t = x / 0.5;
            x = 0.5 * std::pow( t, shadowCrush );
         }
      }
   }

   // Blend between MTF curve and power curve based on curveShape
   double mtfResult = MTF( x, m_midtones );
   double powerResult = std::pow( x, 1.0 / (1.0 + m_midtones * 3.0) );

   double result = mtfResult * (1.0 - curveShape) + powerResult * curveShape;

   // Apply highlights adjustment
   if ( std::abs( highlights ) > 0.001 )
   {
      if ( highlights < 0 )
      {
         // Compress highlights
         double compress = 1.0 + std::abs( highlights ) * 0.5;
         if ( result > 0.5 )
         {
            double t = (result - 0.5) / 0.5;
            result = 0.5 + 0.5 * std::pow( t, compress );
         }
      }
      else
      {
         // Expand highlights
         double expand = 1.0 / (1.0 + highlights * 0.5);
         if ( result > 0.5 )
         {
            double t = (result - 0.5) / 0.5;
            result = 0.5 + 0.5 * std::pow( t, expand );
         }
      }
   }

   return Clamp( result );
}

// ----------------------------------------------------------------------------

void OTSStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Auto-detect black point from background
   double bp = std::max( 0.0, stats.median - 2.8 * stats.mad );
   SetBlackPoint( bp );

   // Calculate effective median after black point
   double effectiveMedian = (stats.median - bp) / (1.0 - bp);
   effectiveMedian = Clamp( effectiveMedian, 0.0001, 0.9999 );

   // Set target median based on image content
   double targetMedian = 0.25; // Default

   // Adjust target based on image characteristics
   if ( stats.dynamicRange > 3.0 )
   {
      // High dynamic range - slightly lower target
      targetMedian = 0.20;
   }
   else if ( stats.max < 0.5 )
   {
      // All faint content - higher target
      targetMedian = 0.30;
   }

   SetTargetMedian( targetMedian );

   // Calculate midtones to achieve target
   double numerator = effectiveMedian * (targetMedian - 1.0);
   double denominator = effectiveMedian * (2.0 * targetMedian - 1.0) - targetMedian;

   if ( std::abs( denominator ) > 1e-10 )
   {
      m_midtones = numerator / denominator;
      m_midtones = Clamp( m_midtones, 0.0001, 0.9999 );
   }
   else
   {
      m_midtones = 0.5;
   }

   // Set curve shape based on dynamic range
   double curveShape = 0.5;
   if ( stats.dynamicRange > 4.0 )
   {
      // High DR - use more power-like curve
      curveShape = 0.7;
   }
   else if ( stats.dynamicRange < 2.0 )
   {
      // Low DR - use more MTF-like curve
      curveShape = 0.3;
   }
   SetCurveShape( curveShape );

   // Default shadow/highlight adjustments
   SetShadows( 0.0 );
   SetHighlights( 0.0 );

   m_needsUpdate = false;
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> OTSStretch::Clone() const
{
   auto clone = std::make_unique<OTSStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   clone->m_midtones = m_midtones;
   clone->m_needsUpdate = m_needsUpdate;

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
