//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// FrameRegistration - Triangle-matching star alignment for subframe stacking

#ifndef __FrameRegistration_h
#define __FrameRegistration_h

#include <pcl/Image.h>
#include <pcl/Console.h>
#include <vector>
#include <cmath>
#include <string>

namespace pcl
{

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------

struct FrameRegistrationConfig
{
   int    maxStars = 200;          // Maximum stars to detect per frame
   double sensitivity = 0.5;       // Star detection sensitivity (0-1)
   int    minMatches = 6;          // Minimum star matches for valid registration
   double ratioTolerance = 0.01;   // Triangle side ratio matching tolerance
   double maxTranslation = 500.0;  // Max translation in pixels before rejection
   double maxRotationDeg = 5.0;    // Max rotation in degrees before rejection
   double maxScaleDev = 0.05;      // Max |scale - 1.0| before rejection
   double nearIdentityPx = 0.5;   // Skip resampling if total displacement < this
   double outlierSigma = 2.5;      // MAD-based sigma clipping for match refinement
   int    maxTriangles = 5000;     // Maximum triangles to generate
};

// ----------------------------------------------------------------------------
// Detected star
// ----------------------------------------------------------------------------

struct DetectedStar
{
   double x;          // Centroid X
   double y;          // Centroid Y
   double flux;       // Total flux (for sorting)
};

// ----------------------------------------------------------------------------
// Triangle descriptor (scale-invariant)
// ----------------------------------------------------------------------------

struct StarTriangle
{
   int    i, j, k;       // Indices into star list
   double r1, r2;         // Normalized side ratios: side[1]/side[0], side[2]/side[0]
                           // where sides are sorted: side[0] >= side[1] >= side[2]
};

// ----------------------------------------------------------------------------
// Similarity transform: x' = a*x - b*y + tx,  y' = b*x + a*y + ty
// ----------------------------------------------------------------------------

struct SimilarityTransform
{
   double a  = 1.0;   // cos(theta) * scale
   double b  = 0.0;   // sin(theta) * scale
   double tx = 0.0;   // X translation
   double ty = 0.0;   // Y translation

   double Scale() const { return std::sqrt( a * a + b * b ); }
   double RotationDeg() const { return std::atan2( b, a ) * 180.0 / 3.14159265358979323846; }
   double TranslationPx() const { return std::sqrt( tx * tx + ty * ty ); }

   void Apply( double x, double y, double& xp, double& yp ) const
   {
      xp = a * x - b * y + tx;
      yp = b * x + a * y + ty;
   }

   bool IsNearIdentity( double threshold ) const
   {
      // Check if max corner displacement on a typical frame is below threshold
      // Simple check: translation + rotation effect
      return TranslationPx() < threshold
          && std::abs( RotationDeg() ) < 0.01
          && std::abs( Scale() - 1.0 ) < 0.0001;
   }
};

// ----------------------------------------------------------------------------
// Star match pair
// ----------------------------------------------------------------------------

struct StarMatch
{
   int refIndex;
   int targetIndex;
   double residual;   // Distance after transform
};

// ----------------------------------------------------------------------------
// Per-frame registration result
// ----------------------------------------------------------------------------

struct FrameRegistrationResult
{
   int    frameIndex = 0;
   int    starsDetected = 0;
   int    starsMatched = 0;
   double rmsResidual = 0.0;
   double dx = 0.0;
   double dy = 0.0;
   double rotationDeg = 0.0;
   double scale = 1.0;
   bool   success = false;
   bool   skippedNearIdentity = false;
   String message;
};

// ----------------------------------------------------------------------------
// Overall summary
// ----------------------------------------------------------------------------

struct RegistrationSummary
{
   int    totalFrames = 0;
   int    registeredFrames = 0;
   int    skippedFrames = 0;      // Near-identity, no resampling needed
   int    failedFrames = 0;
   double totalTimeMs = 0.0;
   std::vector<FrameRegistrationResult> perFrame;
};

// ----------------------------------------------------------------------------
// FrameRegistration engine
// ----------------------------------------------------------------------------

class FrameRegistration
{
public:

   FrameRegistration() = default;
   explicit FrameRegistration( const FrameRegistrationConfig& config ) : m_config( config ) {}

   void SetConfig( const FrameRegistrationConfig& config ) { m_config = config; }
   const FrameRegistrationConfig& Config() const { return m_config; }

   // Main entry: register all frames in-place. Frame[0] is reference.
   bool RegisterFrames( std::vector<Image>& frames, RegistrationSummary& summary );

   // Single frame registration
   FrameRegistrationResult RegisterFrame( Image& frame,
                                          const std::vector<DetectedStar>& refStars,
                                          const std::vector<StarTriangle>& refTriangles,
                                          int frameIndex );

   // Star detection: create luminance and run detection
   std::vector<DetectedStar> DetectStarsInFrame( const Image& image, int maxStars );

   // Build triangles from brightest stars
   std::vector<StarTriangle> BuildTriangles( const std::vector<DetectedStar>& stars,
                                              int maxStars,
                                              int maxTriangles );

   // Match triangles between reference and target
   std::vector<StarMatch> MatchTriangles( const std::vector<StarTriangle>& refTri,
                                           const std::vector<StarTriangle>& targetTri,
                                           const std::vector<DetectedStar>& refStars,
                                           const std::vector<DetectedStar>& targetStars );

   // Compute similarity transform from matched star pairs
   SimilarityTransform ComputeSimilarityTransform( const std::vector<DetectedStar>& refStars,
                                                    const std::vector<DetectedStar>& targetStars,
                                                    const std::vector<StarMatch>& matches );

   // Refine matches using MAD-based outlier rejection, recompute transform
   SimilarityTransform RefineMatches( const std::vector<DetectedStar>& refStars,
                                       const std::vector<DetectedStar>& targetStars,
                                       std::vector<StarMatch>& matches );

   // Apply transform via bicubic resampling (in-place, per-channel, OpenMP)
   void ApplyTransform( Image& frame, const SimilarityTransform& transform );

private:

   FrameRegistrationConfig m_config;

   // Bicubic (Catmull-Rom) interpolation weight
   static double CatmullRom( double t );
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __FrameRegistration_h
