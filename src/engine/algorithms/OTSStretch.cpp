//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "OTSStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

OTSStretch::OTSStretch()
{
   AddParameter( AlgorithmParameter(
      "targetMedian", "Target Median", 0.25, 0.1, 0.5, 3,
      "Target median brightness after stretch."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.5, 6,
      "Background level to clip to black."
   ) );

   AddParameter( AlgorithmParameter(
      "shadows", "Shadows", 0.0, -1.0, 1.0, 2,
      "Shadow adjustment. Negative lifts shadows, positive crushes them."
   ) );

   AddParameter( AlgorithmParameter(
      "highlights", "Highlights", 0.0, -1.0, 1.0, 2,
      "Highlight adjustment. Negative compresses, positive expands."
   ) );

   AddParameter( AlgorithmParameter(
      "curveShape", "Curve Shape", 0.5, 0.0, 1.0, 2,
      "Shape of the transfer curve. 0 = MTF-like, 1 = power curve-like."
   ) );
}

// ----------------------------------------------------------------------------

String OTSStretch::Description() const
{
   return "Optimal Transfer Stretch automatically calculates the best stretch "
          "parameters based on image statistics. An excellent starting point "
          "for most astrophotography images.";
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
   m_needsUpdate = false;
}

// ----------------------------------------------------------------------------

double OTSStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double shadows = Shadows();
   double highlights = Highlights();
   double curveShape = CurveShape();

   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   // Apply shadows adjustment
   if ( std::abs( shadows ) > 0.001 )
   {
      if ( shadows < 0 )
      {
         double shadowLift = 1.0 + shadows * 0.5;
         x = std::pow( x, shadowLift );
      }
      else
      {
         double shadowCrush = 1.0 + shadows * 2.0;
         if ( x < 0.5 )
         {
            double t = x / 0.5;
            x = 0.5 * std::pow( t, shadowCrush );
         }
      }
   }

   // Blend between MTF and power curve
   double mtfResult = MTF( x, m_midtones );
   double powerResult = std::pow( x, 1.0 / (1.0 + m_midtones * 3.0) );

   double result = mtfResult * (1.0 - curveShape) + powerResult * curveShape;

   // Apply highlights adjustment
   if ( std::abs( highlights ) > 0.001 )
   {
      if ( highlights < 0 )
      {
         double compress = 1.0 + std::abs( highlights ) * 0.5;
         if ( result > 0.5 )
         {
            double t = (result - 0.5) / 0.5;
            result = 0.5 + 0.5 * std::pow( t, compress );
         }
      }
      else
      {
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

void OTSStretch::AutoConfigure( double median, double mad )
{
   double bp = std::max( 0.0, median - 2.8 * mad );
   SetBlackPoint( bp );

   double effectiveMedian = (median - bp) / (1.0 - bp);
   effectiveMedian = Clamp( effectiveMedian, 0.0001, 0.9999 );

   double targetMedian = 0.25;
   SetTargetMedian( targetMedian );

   // Calculate midtones
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

   SetCurveShape( 0.5 );
   SetShadows( 0.0 );
   SetHighlights( 0.0 );

   m_needsUpdate = false;
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> OTSStretch::Clone() const
{
   auto clone = std::make_unique<OTSStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   clone->m_midtones = m_midtones;
   clone->m_needsUpdate = m_needsUpdate;
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
