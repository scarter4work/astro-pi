//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Statistical Adaptive Stretch (SAS) Algorithm

#ifndef __SASStretch_h
#define __SASStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Statistical Adaptive Stretch (SAS)
//
// Noise-aware stretch that adapts based on local SNR.
// Prevents noise amplification in faint regions while allowing
// aggressive stretching where signal is strong.
// ----------------------------------------------------------------------------

class SASStretch : public StretchAlgorithmBase
{
public:

   SASStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "SAS"; }
   String Name() const override { return "Statistical Adaptive Stretch"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   double SNRThreshold() const { return GetParameter( "snrThreshold" ); }
   void SetSNRThreshold( double t ) { SetParameter( "snrThreshold", t ); }

   double NoiseFloor() const { return GetParameter( "noiseFloor" ); }
   void SetNoiseFloor( double nf ) { SetParameter( "noiseFloor", nf ); }

   double StretchStrength() const { return GetParameter( "stretchStrength" ); }
   void SetStretchStrength( double ss ) { SetParameter( "stretchStrength", ss ); }

   double Iterations() const { return GetParameter( "iterations" ); }
   void SetIterations( double i ) { SetParameter( "iterations", i ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double BackgroundTarget() const { return GetParameter( "backgroundTarget" ); }
   void SetBackgroundTarget( double bt ) { SetParameter( "backgroundTarget", bt ); }

private:

   double StretchIteration( double x, double snrWeight ) const;
   double EstimateSNRWeight( double value ) const;

   mutable double m_noiseEstimate = 0.01;
   mutable double m_signalEstimate = 0.1;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __SASStretch_h
