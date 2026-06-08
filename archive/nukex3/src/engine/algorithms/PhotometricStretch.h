//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Photometric Stretch Algorithm

#ifndef __PhotometricStretch_h
#define __PhotometricStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Photometric Stretch
//
// Maintains photometric accuracy with an invertible transfer function.
// Best for scientific applications where relative brightness matters.
// ----------------------------------------------------------------------------

class PhotometricStretch : public StretchAlgorithmBase
{
public:

   PhotometricStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "Photometric"; }
   String Name() const override { return "Photometric Stretch"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   double ReferenceLevel() const { return GetParameter( "referenceLevel" ); }
   void SetReferenceLevel( double rl ) { SetParameter( "referenceLevel", rl ); }

   double OutputReference() const { return GetParameter( "outputReference" ); }
   void SetOutputReference( double or_ ) { SetParameter( "outputReference", or_ ); }

   double LinearRange() const { return GetParameter( "linearRange" ); }
   void SetLinearRange( double lr ) { SetParameter( "linearRange", lr ); }

   double CompressionFactor() const { return GetParameter( "compressionFactor" ); }
   void SetCompressionFactor( double cf ) { SetParameter( "compressionFactor", cf ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double Inverse( double stretchedValue ) const;

private:

   double TransferFunction( double x ) const;
   double InverseTransferFunction( double y ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __PhotometricStretch_h
