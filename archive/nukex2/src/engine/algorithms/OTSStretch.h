//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Optimal Transfer Stretch (OTS) Algorithm

#ifndef __OTSStretch_h
#define __OTSStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Optimal Transfer Stretch (OTS)
//
// An automatic stretch algorithm that calculates optimal transfer function
// parameters based on image statistics. The goal is to achieve a well-balanced
// stretch that brings the image median to a target value while preserving
// detail across the tonal range.
//
// The algorithm combines:
// 1. Automatic black point detection (background level)
// 2. Target median calculation for optimal brightness
// 3. Adaptive curve shape based on dynamic range
// 4. Optional histogram equalization influence
//
// This is designed as a "smart auto" stretch that works well as a
// starting point for most astrophotography images.
// ----------------------------------------------------------------------------

class OTSStretch : public StretchAlgorithmBase
{
public:

   OTSStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "OTS"; }
   String Name() const override { return "Optimal Transfer Stretch"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
   double TargetMedian() const { return GetParameter( "targetMedian" ); }
   void SetTargetMedian( double tm ) { SetParameter( "targetMedian", tm ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double Shadows() const { return GetParameter( "shadows" ); }
   void SetShadows( double s ) { SetParameter( "shadows", s ); }

   double Highlights() const { return GetParameter( "highlights" ); }
   void SetHighlights( double h ) { SetParameter( "highlights", h ); }

   double CurveShape() const { return GetParameter( "curveShape" ); }
   void SetCurveShape( double cs ) { SetParameter( "curveShape", cs ); }

private:

   // Computed transfer function parameters
   mutable double m_midtones = 0.5;
   mutable bool m_needsUpdate = true;

   void UpdateTransferFunction() const;
   double MTF( double x, double m ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __OTSStretch_h
