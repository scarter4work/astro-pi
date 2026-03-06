//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Veralux (Film-like) Stretch Algorithm

#ifndef __VeraluxStretch_h
#define __VeraluxStretch_h

#include "../IStretchAlgorithm.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Veralux Stretch (Film-like Response)
//
// Emulates the characteristic S-curve response of photographic film.
// Natural-looking stretch with smooth shadow roll-off and gentle
// highlight compression.
// ----------------------------------------------------------------------------

class VeraluxStretch : public StretchAlgorithmBase
{
public:

   VeraluxStretch();

   double Apply( double value ) const override;
   IsoString Id() const override { return "Veralux"; }
   String Name() const override { return "Veralux (Film Response)"; }
   String Description() const override;
   void AutoConfigure( double median, double mad ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   double Exposure() const { return GetParameter( "exposure" ); }
   void SetExposure( double e ) { SetParameter( "exposure", e ); }

   double Contrast() const { return GetParameter( "contrast" ); }
   void SetContrast( double c ) { SetParameter( "contrast", c ); }

   double ToeStrength() const { return GetParameter( "toeStrength" ); }
   void SetToeStrength( double ts ) { SetParameter( "toeStrength", ts ); }

   double ShoulderStrength() const { return GetParameter( "shoulderStrength" ); }
   void SetShoulderStrength( double ss ) { SetParameter( "shoulderStrength", ss ); }

   double BlackPoint() const { return GetParameter( "blackPoint" ); }
   void SetBlackPoint( double bp ) { SetParameter( "blackPoint", bp ); }

   double WhitePoint() const { return GetParameter( "whitePoint" ); }
   void SetWhitePoint( double wp ) { SetParameter( "whitePoint", wp ); }

   // Preset film emulations
   void PresetNeutral();
   void PresetHighContrast();
   void PresetLowContrast();
   void PresetCinematic();

private:

   double FilmCurve( double x ) const;
   double ToeCurve( double x, double strength ) const;
   double ShoulderCurve( double x, double strength ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __VeraluxStretch_h
