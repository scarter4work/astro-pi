//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
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
// This provides a pleasing, natural-looking stretch with smooth
// shadow roll-off and gentle highlight compression.
//
// Key characteristics:
// 1. Toe region - gentle shadow lift with smooth roll-off
// 2. Linear region - faithful mid-tone reproduction
// 3. Shoulder region - smooth highlight compression
// 4. D-max - maximum density (black point)
// 5. D-min - minimum density (white point)
//
// The algorithm simulates a characteristic curve (H&D curve) similar
// to classic film stocks like Kodak Tri-X or Ilford HP5.
//
// This creates images with:
// - Natural-looking contrast
// - Pleasing shadow transitions
// - Protected highlights
// - "Filmic" aesthetic
// ----------------------------------------------------------------------------

class VeraluxStretch : public StretchAlgorithmBase
{
public:

   VeraluxStretch();

   // IStretchAlgorithm interface
   double Apply( double value ) const override;
   IsoString Id() const override { return "Veralux"; }
   String Name() const override { return "Veralux (Film Response)"; }
   String Description() const override;
   void AutoConfigure( const RegionStatistics& stats ) override;
   std::unique_ptr<IStretchAlgorithm> Clone() const override;

   // Convenience accessors
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

   // Film response curve calculation
   double FilmCurve( double x ) const;

   // Toe curve (shadow region)
   double ToeCurve( double x, double strength ) const;

   // Shoulder curve (highlight region)
   double ShoulderCurve( double x, double strength ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __VeraluxStretch_h
