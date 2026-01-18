//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "LogStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

LogStretch::LogStretch()
{
   // Scale factor - controls aggressiveness of log compression
   AddParameter( AlgorithmParameter(
      "scale",
      "Scale Factor",
      100.0,    // default
      1.0,      // min
      10000.0,  // max
      0,        // precision (integer-like)
      "Logarithmic scale factor. Higher values = more aggressive stretch "
      "of faint detail but more compression of highlights."
   ) );

   // Black point
   AddParameter( AlgorithmParameter(
      "blackPoint",
      "Black Point",
      0.0,      // default
      0.0,      // min
      0.5,      // max
      6,        // precision
      "Subtract this value before stretching (sets the black level)."
   ) );

   // Highlight protection - blend with linear at high values
   AddParameter( AlgorithmParameter(
      "highlightProtection",
      "Highlight Protection",
      0.0,      // default (no protection)
      0.0,      // min
      1.0,      // max
      2,        // precision
      "Blend log stretch with linear above this threshold to protect highlights. "
      "0 = no protection, 0.5 = start blending at 50% brightness."
   ) );
}

// ----------------------------------------------------------------------------

String LogStretch::Description() const
{
   return "Logarithmic stretch provides extreme compression of the dynamic "
          "range, making it excellent for revealing very faint details like "
          "Integrated Flux Nebula (IFN), outer galaxy halos, and faint "
          "extended emission. Use the highlight protection parameter to "
          "prevent blowing out brighter regions.";
}

// ----------------------------------------------------------------------------

void LogStretch::UpdateNormFactor() const
{
   double scale = Scale();
   if ( std::abs( scale - m_lastScale ) > 1e-10 )
   {
      m_lastScale = scale;
      // Normalization factor ensures output 1 for input 1
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

   // Apply black point
   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   if ( x <= 0.0 )
      return 0.0;
   if ( x >= 1.0 )
      return 1.0;

   // Update normalization factor if needed
   UpdateNormFactor();

   // Apply log stretch
   // Formula: log(1 + scale * x) / log(1 + scale)
   double logResult = std::log( 1.0 + scale * x ) / m_normFactor;

   // Highlight protection - blend with linear above threshold
   if ( highlightProt > 0.0 && x > highlightProt )
   {
      // Calculate blend factor (0 at threshold, 1 at x=1)
      double blendRange = 1.0 - highlightProt;
      double blendFactor = (x - highlightProt) / blendRange;
      blendFactor = blendFactor * blendFactor; // Smooth curve

      // Calculate what log would give at threshold
      double logAtThreshold = std::log( 1.0 + scale * highlightProt ) / m_normFactor;

      // Linear continuation from threshold
      double linearResult = logAtThreshold + (x - highlightProt) * (1.0 - logAtThreshold) / blendRange;

      // Blend log and linear
      logResult = logResult * (1.0 - blendFactor) + linearResult * blendFactor;
   }

   return Clamp( logResult );
}

// ----------------------------------------------------------------------------

void LogStretch::AutoConfigure( const RegionStatistics& stats )
{
   // For very faint regions (like faint nebulosity), use high scale
   // For brighter regions, use lower scale to avoid blowout

   double brightness = stats.median;
   double scale;

   if ( brightness < 0.001 )
   {
      // Extremely faint - very aggressive log stretch
      scale = 5000.0;
   }
   else if ( brightness < 0.01 )
   {
      // Very faint - aggressive stretch
      scale = 1000.0 + 4000.0 * (0.01 - brightness) / 0.01;
   }
   else if ( brightness < 0.1 )
   {
      // Faint - moderate stretch
      scale = 100.0 + 900.0 * (0.1 - brightness) / 0.1;
   }
   else
   {
      // Brighter - gentle stretch
      scale = 10.0 + 90.0 * (1.0 - brightness);
   }

   SetScale( scale );

   // Set black point based on background
   double blackPoint = std::max( 0.0, stats.median - 2.0 * stats.mad );
   SetBlackPoint( blackPoint );

   // Enable highlight protection for regions with bright content
   double highlightProt = 0.0;
   if ( stats.max > 0.3 && stats.dynamicRange > 2.0 )
   {
      // High dynamic range - protect highlights
      highlightProt = 0.5;
   }
   SetHighlightProtection( highlightProt );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> LogStretch::Clone() const
{
   auto clone = std::make_unique<LogStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
