//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Lumpton (SDSS) Stretch Algorithm

#ifndef __LumptonStretch_h
#define __LumptonStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Lumpton Stretch (SDSS-style HDR)
//
// Based on the method described by Lupton et al. (2004) for creating
// color images from SDSS survey data. Provides excellent handling of
// high dynamic range astronomical images.
//
// The stretch applies an asinh-based transformation that preserves colors
// while compressing the dynamic range:
//
//   stretched = asinh(Q * x / minimum) / Q
//
// Where:
//   x = input pixel value
//   Q = softening parameter (controls transition from linear to log-like)
//   minimum = noise floor / scaling parameter
//
// This is particularly good for:
//   - Galaxy halos and faint outer regions
//   - Images with high dynamic range
//   - Preserving color relationships in stretched data
//
// Reference: Lupton et al. (2004) "Preparing Red-Green-Blue Images from
// CCD Data", PASP 116:133-137
// ----------------------------------------------------------------------------

class LumptonStretch : public StretchAlgorithmBase
{
public:

   LumptonStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "Lumpton"; }
   String Name() const override { return "Lumpton (SDSS HDR)"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
   double Q() const { return GetParameter( "Q" ); }
   void SetQ( double q ) { SetParameter( "Q", q ); }

   double Minimum() const { return GetParameter( "minimum" ); }
   void SetMinimum( double m ) { SetParameter( "minimum", m ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double Stretch() const { return GetParameter( "stretch" ); }
   void SetStretch( double s ) { SetParameter( "stretch", s ); }

private:

   // Cached normalization
   mutable double m_normFactor = 1.0;
   mutable double m_lastQ = -1.0;

   void UpdateNormFactor() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __LumptonStretch_h
