//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "PhotometricStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

PhotometricStretch::PhotometricStretch()
{
   // Reference level - input brightness that maps to output reference
   AddParameter( AlgorithmParameter(
      "referenceLevel",
      "Reference Level",
      0.1,      // default
      0.001,    // min
      0.5,      // max
      4,        // precision
      "Input brightness level used as reference point for the transformation. "
      "Typically set to the median or a known calibration level."
   ) );

   // Output reference - where the reference level maps to
   AddParameter( AlgorithmParameter(
      "outputReference",
      "Output Reference",
      0.3,      // default
      0.1,      // min
      0.7,      // max
      3,        // precision
      "Output brightness for the reference level. Controls overall image brightness."
   ) );

   // Linear range - width of the quasi-linear region (in log units)
   AddParameter( AlgorithmParameter(
      "linearRange",
      "Linear Range",
      2.0,      // default (2 decades)
      0.5,      // min
      5.0,      // max
      1,        // precision
      "Width of the quasi-linear region in log10 units (decades). "
      "Larger values = more linear behavior."
   ) );

   // Compression factor for extremes
   AddParameter( AlgorithmParameter(
      "compressionFactor",
      "Compression Factor",
      0.5,      // default
      0.1,      // min
      1.0,      // max
      2,        // precision
      "How aggressively to compress values outside the linear range. "
      "Lower = more compression."
   ) );

   // Black point
   AddParameter( AlgorithmParameter(
      "blackPoint",
      "Black Point",
      0.0,      // default
      0.0,      // min
      0.1,      // max
      6,        // precision
      "Input level that maps to pure black. Values below are clipped."
   ) );
}

// ----------------------------------------------------------------------------

String PhotometricStretch::Description() const
{
   return "Photometric Stretch is designed to maintain relative brightness "
          "relationships (photometric accuracy) as much as possible while "
          "producing a viewable image. It uses a transfer function that is "
          "quasi-linear around a reference point with defined compression at "
          "the extremes. The transformation is mathematically invertible, "
          "allowing recovery of original values.";
}

// ----------------------------------------------------------------------------

double PhotometricStretch::TransferFunction( double x ) const
{
   // This transfer function:
   // 1. Is quasi-linear around the reference point
   // 2. Uses asinh for smooth compression at extremes
   // 3. Is invertible

   double refLevel = ReferenceLevel();
   double outRef = OutputReference();
   double linearRange = LinearRange();
   double compression = CompressionFactor();

   if ( x <= 0.0 )
      return 0.0;

   // Work in log space relative to reference
   double logX = std::log10( x / refLevel );

   // The linear range is defined in log units
   double halfRange = linearRange / 2.0;

   // Apply asinh transformation centered on reference
   // asinh(x) ~ x for small x, ~ log(2x) for large x
   double scale = 1.0 / halfRange;
   double normalized = std::asinh( logX * scale * compression );

   // Scale to map [-halfRange, +halfRange] to reasonable output range
   double maxNorm = std::asinh( scale * compression );
   double mapped = normalized / maxNorm;

   // Map to output centered on outRef
   // Range is approximately [0, 2*outRef] for inputs in [refLevel/10^halfRange, refLevel*10^halfRange]
   double result = outRef + mapped * outRef;

   return Clamp( result );
}

// ----------------------------------------------------------------------------

double PhotometricStretch::InverseTransferFunction( double y ) const
{
   double refLevel = ReferenceLevel();
   double outRef = OutputReference();
   double linearRange = LinearRange();
   double compression = CompressionFactor();

   if ( y <= 0.0 )
      return 0.0;

   double halfRange = linearRange / 2.0;
   double scale = 1.0 / halfRange;
   double maxNorm = std::asinh( scale * compression );

   // Reverse the mapping
   double mapped = (y - outRef) / outRef;
   double normalized = mapped * maxNorm;

   // Inverse asinh
   double logX = std::sinh( normalized ) / (scale * compression);

   // Convert back from log space
   double x = refLevel * std::pow( 10.0, logX );

   return Clamp( x );
}

// ----------------------------------------------------------------------------

double PhotometricStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();

   // Apply black point
   if ( value <= blackPoint )
      return 0.0;

   double x = (value - blackPoint) / (1.0 - blackPoint);

   return TransferFunction( x );
}

// ----------------------------------------------------------------------------

double PhotometricStretch::Inverse( double stretchedValue ) const
{
   double blackPoint = BlackPoint();

   double x = InverseTransferFunction( stretchedValue );

   // Reverse black point normalization
   return x * (1.0 - blackPoint) + blackPoint;
}

// ----------------------------------------------------------------------------

void PhotometricStretch::AutoConfigure( const RegionStatistics& stats )
{
   // Set reference level to image median
   double refLevel = stats.median;
   refLevel = std::max( 0.001, refLevel );
   SetReferenceLevel( refLevel );

   // Output reference - aim for pleasant mid-brightness
   SetOutputReference( 0.35 );

   // Set linear range based on dynamic range of the image
   double linearRange = std::min( stats.dynamicRange + 1.0, 4.0 );
   linearRange = std::max( 1.0, linearRange );
   SetLinearRange( linearRange );

   // Compression based on how much data is outside typical range
   double compression = 0.5;
   if ( stats.clippingPct > 1.0 )
   {
      // Significant clipping - use more compression
      compression = 0.3;
   }
   SetCompressionFactor( compression );

   // Set black point
   double bp = std::max( 0.0, stats.median - 3.0 * stats.mad );
   SetBlackPoint( bp );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> PhotometricStretch::Clone() const
{
   auto clone = std::make_unique<PhotometricStretch>();

   for ( const AlgorithmParameter& param : m_parameters )
   {
      clone->SetParameter( param.id, param.value );
   }

   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
