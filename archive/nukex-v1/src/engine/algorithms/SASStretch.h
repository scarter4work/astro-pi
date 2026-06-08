//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
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
// A noise-aware stretch algorithm that adapts its behavior based on local
// signal-to-noise ratio (SNR). This approach prevents amplification of noise
// in faint regions while still allowing aggressive stretching of areas with
// good signal.
//
// Key features:
// 1. SNR-weighted stretch - stronger stretch where signal is stronger
// 2. Noise floor protection - limits stretch in noisy regions
// 3. Iterative refinement - can apply multiple gentle passes
// 4. Background neutralization - ensures clean background
//
// The algorithm calculates a local "confidence" based on the estimated SNR
// and blends between aggressive and conservative stretch accordingly.
//
// Best suited for:
// - Faint nebulosity with varying SNR
// - Images with significant background noise
// - Situations where noise amplification is a concern
// ----------------------------------------------------------------------------

class SASStretch : public StretchAlgorithmBase
{
public:

   SASStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "SAS"; }
   String Name() const override { return "Statistical Adaptive Stretch"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
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

   // Single iteration stretch
   double StretchIteration( double x, double snrWeight ) const;

   // Estimate local SNR weight for a pixel value
   double EstimateSNRWeight( double value ) const;

   // Cached noise parameters
   mutable double m_noiseEstimate = 0.01;
   mutable double m_signalEstimate = 0.1;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __SASStretch_h
