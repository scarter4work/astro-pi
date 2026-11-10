// EZTools -- EZHazeKill engine (PCL-free).
// Copyright and licence: see src/EZToolsLicense.h.
//
// The sky model of EZHazeKill.js 1.1.0, exactly as the offline reference hk_lib.hk_v11 (q = 10):
//
//   sky_c    median, over the darkest 10% of 64 px tiles (ranked by tile luminance = mean of the
//            channel medians), of each tile's channel-c median
//   sigma_c  1.4826 x the median tile MAD of the same tiles
//   cast_c   sky_c - mean(sky)                       (removed x Neutralize)
//   T_eff    max(Target, 4 x max sigma)              (noise floor)
//   ped      max(0, mean(sky) - T_eff)               (removed x Amount; never raised)
//   u        x - (Neutralize x cast_c + Amount x ped)
//   out      clamp(Toe(u, eps), 0, 1),  eps = max(1e-6, 2 x max sigma)
//
// Medians of an even count are the mean of the two middle values; the percentile is numpy's
// "linear" one; tiles are selected with L <= percentile; partial edge tiles are dropped; an image
// smaller than one tile in either direction is a single tile. Arithmetic is in double.

#ifndef EZTOOLS_HAZEKILL_H
#define EZTOOLS_HAZEKILL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eztools::hazekill
{

constexpr int    kTile = 64;              // tile size, pixels
constexpr double kSkyPercentile = 10;     // darkest tiles used for the sky, percent
constexpr double kFloorSigma = 4;         // the sky is never pushed below this many sigma
constexpr double kToeSigma = 2;           // the soft toe starts this many sigma above zero
constexpr double kStretchedNoise = 0.03;  // max sigma above this fraction of the sky reads as stretched

struct Settings
{
   double amount = 0.5;       // fraction of the sky pedestal above the target removed
   double neutralize = 1.0;   // fraction of the sky colour cast removed
   double target = 0.05;      // lowest level the sky is brought down to
};

struct SkyEstimate
{
   bool ok = false;
   std::string error;         // set when !ok
   std::vector<double> sky, sigma;
   int tiles = 0, skyTiles = 0;
};

struct Model
{
   std::vector<double> sky, sigma, cast, offset;
   double skyL = 0, sigmaMax = 0, floorLevel = 0, tEff = 0, ped = 0, eps = 0;
   bool stretched = false;
   int tiles = 0, skyTiles = 0;
   Settings settings;
};

// channels[c] points at width*height row-major samples (any finite value accepted). Returns
// ok = false, with an error naming the problem, for no channels, an empty image, or any NaN/Inf.
SkyEstimate MeasureSky( const std::vector<const double*>& channels, int width, int height );
SkyEstimate MeasureSky( const std::vector<const float*>& channels, int width, int height );
// Integer images: each sample read as x * (1/max) (PixInsight's sample conversion), tile by tile.
SkyEstimate MeasureSky( const std::vector<const uint8_t*>& channels, int width, int height );
SkyEstimate MeasureSky( const std::vector<const uint16_t*>& channels, int width, int height );
SkyEstimate MeasureSky( const std::vector<const uint32_t*>& channels, int width, int height );

// The correction for a (successful) estimate and the given settings.
Model BuildModel( const SkyEstimate&, const Settings& );

// Soft toe: u for u >= eps, eps*exp(u/eps - 1) below. Monotonic, C1 at eps, positive.
double Toe( double u, double eps );

// One sample: clamp(Toe(x - offset, eps), 0, 1).
inline double ApplySample( double x, double offset, double eps )
{
   double v = Toe( x - offset, eps );
   return v < 0 ? 0.0 : ( v > 1 ? 1.0 : v );
}

// In place on n samples per channel; channels.size() must equal the model's channel count.
void Apply( const std::vector<float*>& channels, size_t n, const Model& );
void Apply( const std::vector<double*>& channels, size_t n, const Model& );

// The script's hkReport() console text, line for line ('\n' separated, no trailing newline).
// The stretched-image note, when present, is the last line and starts with "Note:".
std::string Report( const Model& );

} // namespace eztools::hazekill

#endif
