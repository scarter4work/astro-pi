//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "LogStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

LogStretch::LogStretch()
{
   AddParameter( AlgorithmParameter(
      "scale", "Scale Factor", 100.0, 1.0, 10000.0, 0,
      "Logarithmic scale factor. Higher values = more aggressive stretch."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.5, 6,
      "Subtract this value before stretching (sets the black level)."
   ) );

   AddParameter( AlgorithmParameter(
      "highlightProtection", "Highlight Protection", 0.0, 0.0, 1.0, 2,
      "Blend log stretch with linear above this threshold to protect highlights."
   ) );
}

// ----------------------------------------------------------------------------

String LogStretch::Description() const
{
   return "Logarithmic stretch provides extreme compression of the dynamic "
          "range, excellent for revealing very faint details like IFN, outer "
          "galaxy halos, and faint extended emission.";
}

// ----------------------------------------------------------------------------

void LogStretch::UpdateNormFactor() const
{
   double scale = Scale();
   if ( std::abs( scale - m_lastScale ) > 1e-10 )
   {
      m_lastScale = scale;
      m_normFactor = std::log( 1.0 + scale );
      if ( m_normFactor < 1e-10 )
         m_normFactor = 1.0;
   }
}

// ----------------------------------------------------------------------------

double LogStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();
   double scale = Scale();
   double highlightProt = HighlightProtection();

   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   UpdateNormFactor();

   double logResult = std::log( 1.0 + scale * x ) / m_normFactor;

   if ( highlightProt > 0.0 && x > highlightProt )
   {
      double blendRange = 1.0 - highlightProt;
      double blendFactor = (x - highlightProt) / blendRange;
      blendFactor = blendFactor * blendFactor;

      double logAtThreshold = std::log( 1.0 + scale * highlightProt ) / m_normFactor;
      double linearResult = logAtThreshold + (x - highlightProt) * (1.0 - logAtThreshold) / blendRange;

      logResult = logResult * (1.0 - blendFactor) + linearResult * blendFactor;
   }

   return Clamp( logResult );
}

// ----------------------------------------------------------------------------

void LogStretch::AutoConfigure( double median, double mad )
{
   double brightness = median;
   double scale;

   if ( brightness < 0.001 )
      scale = 5000.0;
   else if ( brightness < 0.01 )
      scale = 1000.0 + 4000.0 * (0.01 - brightness) / 0.01;
   else if ( brightness < 0.1 )
      scale = 100.0 + 900.0 * (0.1 - brightness) / 0.1;
   else
      scale = 10.0 + 90.0 * (1.0 - brightness);

   SetScale( scale );

   double blackPoint = std::max( 0.0, median - 2.0 * mad );
   SetBlackPoint( blackPoint );

   SetHighlightProtection( 0.0 );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> LogStretch::Clone() const
{
   auto clone = std::make_unique<LogStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
