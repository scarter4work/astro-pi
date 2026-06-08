//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
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
// A stretch algorithm designed to maintain photometric accuracy as much as
// possible while still producing a visually pleasing image. This is
// particularly important for scientific applications where relative
// brightness relationships need to be preserved.
//
// Key principles:
// 1. Preserve flux ratios between objects (logarithmic relationship)
// 2. Maintain linearity in specific brightness ranges
// 3. Use well-defined reference points for calibration
// 4. Document the transformation for reproducibility
//
// The algorithm uses a carefully calibrated transfer function that:
// - Is nearly linear in the mid-tones
// - Has defined behavior at the extremes
// - Can be mathematically inverted to recover original values
//
// Best suited for:
// - Variable star photometry visualization
// - Supernova monitoring
// - Calibrated survey data
// - Any application where relative brightness matters
// ----------------------------------------------------------------------------

class PhotometricStretch : public StretchAlgorithmBase
{
public:

   PhotometricStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "Photometric"; }
   String Name() const override { return "Photometric Stretch"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
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

   // Inverse function - recover original value from stretched
   double Inverse( double stretchedValue ) const;

private:

   // The core transfer function (invertible)
   double TransferFunction( double x ) const;
   double InverseTransferFunction( double y ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __PhotometricStretch_h
