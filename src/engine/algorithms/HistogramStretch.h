//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Histogram Transformation Stretch Algorithm

#ifndef __HistogramStretch_h
#define __HistogramStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Histogram Transformation Stretch
//
// Classic histogram transformation with shadows clipping, highlights clipping,
// midtones adjustment, and output range expansion.
// Equivalent to PixInsight's HistogramTransformation process.
// ----------------------------------------------------------------------------

class HistogramStretch : public StretchAlgorithmBase
{
public:

   HistogramStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "Histogram"; }
   String Name() const override { return "Histogram Transformation"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   double ShadowsClip() const { return GetParameter( "shadowsClip" ); }
   void SetShadowsClip( double s ) { SetParameter( "shadowsClip", s ); }

   double HighlightsClip() const { return GetParameter( "highlightsClip" ); }
   void SetHighlightsClip( double h ) { SetParameter( "highlightsClip", h ); }

   double Midtones() const { return GetParameter( "midtones" ); }
   void SetMidtones( double m ) { SetParameter( "midtones", m ); }

   double LowOutput() const { return GetParameter( "lowOutput" ); }
   void SetLowOutput( double l ) { SetParameter( "lowOutput", l ); }

   double HighOutput() const { return GetParameter( "highOutput" ); }
   void SetHighOutput( double h ) { SetParameter( "highOutput", h ); }

private:

   static double MTF( double x, double m );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __HistogramStretch_h
