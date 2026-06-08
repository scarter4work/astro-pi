//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Statistical Auto-Stretch Algorithm
// Pure statistics-based automatic stretch with no ML dependencies

#ifndef __StatisticalAutoStretch_h
#define __StatisticalAutoStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Statistical Auto-Stretch
//
// Automatically determines optimal stretch parameters from image statistics.
// Uses signal compression analysis to select appropriate midtone balance.
//
// Key concepts:
//   - Background estimation via median + MAD
//   - Signal compression ratio: (p90 - background) / (p99 - background)
//   - Lower compression = more aggressive stretch needed
//   - Automatic black point: background - 2.8 * sigma
//   - Automatic white point: p99.9 or user-specified
//
// This algorithm is designed for LINEAR astronomical data where signal
// is compressed into a tiny range above background. It automatically
// detects how compressed the signal is and applies appropriate stretch.
// ----------------------------------------------------------------------------

class StatisticalAutoStretch : public StretchAlgorithmBase
{
public:

   StatisticalAutoStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   void ApplyToImage( Image& image, const Image* mask = nullptr ) const override;

   IsoString Id() const override { return "StatAuto"; }
   String Name() const override { return "Statistical Auto-Stretch"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // -------------------------------------------------------------------------
   // Parameter accessors
   // -------------------------------------------------------------------------

   // Target background level after stretch (0-1, default 0.15)
   double TargetBackground() const { return GetParameter( "targetBackground" ); }
   void SetTargetBackground( double t ) { SetParameter( "targetBackground", t ); }

   // Sigma clipping factor for black point (default 2.8)
   double SigmaClip() const { return GetParameter( "sigmaClip" ); }
   void SetSigmaClip( double s ) { SetParameter( "sigmaClip", s ); }

   // Override midtone (0 = auto, otherwise use this value)
   double MidtoneOverride() const { return GetParameter( "midtoneOverride" ); }
   void SetMidtoneOverride( double m ) { SetParameter( "midtoneOverride", m ); }

   // Highlight protection percentile (default 99.9)
   double HighlightProtection() const { return GetParameter( "highlightProtection" ); }
   void SetHighlightProtection( double h ) { SetParameter( "highlightProtection", h ); }

   // Minimum midtone value (prevents over-aggressive stretch)
   double MinMidtone() const { return GetParameter( "minMidtone" ); }
   void SetMinMidtone( double m ) { SetParameter( "minMidtone", m ); }

   // Maximum midtone value (prevents too-mild stretch)
   double MaxMidtone() const { return GetParameter( "maxMidtone" ); }
   void SetMaxMidtone( double m ) { SetParameter( "maxMidtone", m ); }

   // -------------------------------------------------------------------------
   // Computed parameters (read-only, set by AutoConfigure)
   // -------------------------------------------------------------------------

   // These are computed during AutoConfigure and used during Apply

   double ComputedBlackPoint() const { return m_blackPoint; }
   double ComputedWhitePoint() const { return m_whitePoint; }
   double ComputedMidtone() const { return m_midtone; }
   double SignalCompression() const { return m_signalCompression; }

   // -------------------------------------------------------------------------
   // Static utilities
   // -------------------------------------------------------------------------

   // Calculate midtone from signal compression ratio
   static double CompressionToMidtone( double compression, double minMid = 0.02, double maxMid = 0.5 );

   // MTF function for internal use
   static double MTF( double x, double midtone );

   // Calculate robust sigma from MAD
   static double MADToSigma( double mad ) { return mad * 1.4826; }

private:

   // Computed stretch parameters
   mutable double m_blackPoint = 0.0;
   mutable double m_whitePoint = 1.0;
   mutable double m_midtone = 0.15;
   mutable double m_signalCompression = 0.5;
   mutable bool m_configured = false;

   // Helper to compute statistics from image
   void ComputeStatistics( const Image& image ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __StatisticalAutoStretch_h
