//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "PhotometricStretch.h"

#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------

PhotometricStretch::PhotometricStretch()
{
   AddParameter( AlgorithmParameter(
      "referenceLevel", "Reference Level", 0.1, 0.001, 0.5, 4,
      "Input brightness level used as reference point for the transformation."
   ) );

   AddParameter( AlgorithmParameter(
      "outputReference", "Output Reference", 0.3, 0.1, 0.7, 3,
      "Output brightness for the reference level."
   ) );

   AddParameter( AlgorithmParameter(
      "linearRange", "Linear Range", 2.0, 0.5, 5.0, 1,
      "Width of the quasi-linear region in log10 units (decades)."
   ) );

   AddParameter( AlgorithmParameter(
      "compressionFactor", "Compression Factor", 0.5, 0.1, 1.0, 2,
      "How aggressively to compress values outside the linear range."
   ) );

   AddParameter( AlgorithmParameter(
      "blackPoint", "Black Point", 0.0, 0.0, 0.1, 6,
      "Input level that maps to pure black."
   ) );
}

// ----------------------------------------------------------------------------

String PhotometricStretch::Description() const
{
   return "Photometric Stretch maintains relative brightness relationships "
          "using an invertible transfer function. Quasi-linear around a "
          "reference point with smooth compression at extremes.";
}

// ----------------------------------------------------------------------------

double PhotometricStretch::TransferFunction( double x ) const
{
   double refLevel = ReferenceLevel();
   double outRef = OutputReference();
   double linearRange = LinearRange();
   double compression = CompressionFactor();

   if ( x <= 0.0 )
      return 0.0;

   double logX = std::log10( x / refLevel );
   double halfRange = linearRange / 2.0;
   double scale = 1.0 / halfRange;
   double normalized = std::asinh( logX * scale * compression );
   double maxNorm = std::asinh( scale * compression );
   double mapped = normalized / maxNorm;

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

   double mapped = (y - outRef) / outRef;
   double normalized = mapped * maxNorm;
   double logX = std::sinh( normalized ) / (scale * compression);
   double x = refLevel * std::pow( 10.0, logX );

   return Clamp( x );
}

// ----------------------------------------------------------------------------

double PhotometricStretch::Apply( double value ) const
{
   double blackPoint = BlackPoint();

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
   return x * (1.0 - blackPoint) + blackPoint;
}

// ----------------------------------------------------------------------------

void PhotometricStretch::AutoConfigure( double median, double mad )
{
   double refLevel = std::max( 0.001, median );
   SetReferenceLevel( refLevel );
   SetOutputReference( 0.35 );

   double linearRange = 2.0;
   SetLinearRange( linearRange );

   SetCompressionFactor( 0.5 );

   double bp = std::max( 0.0, median - 3.0 * mad );
   SetBlackPoint( bp );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> PhotometricStretch::Clone() const
{
   auto clone = std::make_unique<PhotometricStretch>();
   for ( const AlgorithmParameter& param : m_parameters )
      clone->SetParameter( param.id, param.value );
   return clone;
}

// ----------------------------------------------------------------------------

} // namespace pcl
