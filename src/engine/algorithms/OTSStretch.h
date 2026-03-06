//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
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
// Automatic stretch that calculates optimal transfer function parameters
// based on image statistics. Combines MTF and power curves.
// ----------------------------------------------------------------------------

class OTSStretch : public StretchAlgorithmBase
{
public:

   OTSStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "OTS"; }
   String Name() const override { return "Optimal Transfer Stretch"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

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

   mutable double m_midtones = 0.5;
   mutable bool m_needsUpdate = true;

   void UpdateTransferFunction() const;
   double MTF( double x, double m ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __OTSStretch_h
