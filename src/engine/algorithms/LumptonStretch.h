//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
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
// Based on Lupton et al. (2004) for SDSS survey data.
// Uses asinh transformation that preserves colors while compressing DR.
// ----------------------------------------------------------------------------

class LumptonStretch : public StretchAlgorithmBase
{
public:

   LumptonStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "Lumpton"; }
   String Name() const override { return "Lumpton (SDSS HDR)"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   double Q() const { return GetParameter( "Q" ); }
   void SetQ( double q ) { SetParameter( "Q", q ); }

   double Minimum() const { return GetParameter( "minimum" ); }
   void SetMinimum( double m ) { SetParameter( "minimum", m ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double Stretch() const { return GetParameter( "stretch" ); }
   void SetStretch( double s ) { SetParameter( "stretch", s ); }

private:

   mutable double m_normFactor = 1.0;
   mutable double m_lastQ = -1.0;

   void UpdateNormFactor() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __LumptonStretch_h
