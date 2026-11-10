// Blind astrometric solution. Port of ImageSolver 6.6.0's blindSolve() (ImageSolverEngine.js
// l.905-1063; Copyright (c) 2013-2026 Andres del Pozo, Copyright (c) 2019-2026 Juan Conejero
// (PTeam); PCL License 2.0) over PCL 2.13's BlindAstrometricSolver.
#include "SolverBlind.h"

#include "core/Format.h"

#include <pcl/AstrometricIndexFile.h>
#include <pcl/ErrorHandler.h>

#include <limits>

namespace pcl
{

namespace
{

BlindAstrometricSolver::Solution Solve( const BlindAstrometricSolver& solver, const DetectedStars& d, int w, int h )
{
   Array<DPoint> positions;
   Array<float> fluxes;
   for ( size_t i = 0; i < d.size(); ++i )
   {
      positions << DPoint( d.pos[i].x, d.pos[i].y );
      fluxes << float( d.flux[i] );
   }
   return solver.Solve( positions, fluxes, w, h );
}

} // namespace

bool BlindSolve( const ImageVariant& gray, const DetectedStars& stars, const DetectParams& params,
                 const String& indexPath, BlindResult& result, const gdg::Log& log )
{
   const int width = gray.Width(), height = gray.Height();
   AstrometricIndexFile index( indexPath );
   BlindAstrometricSolver::Parameters prm;
   prm.maxSeconds = 120; // ImageSolver: solver.maxSeconds = 120
   BlindAstrometricSolver solver( index, prm );

   result = BlindResult();
   result.bestLogOdds = -std::numeric_limits<double>::infinity();
   auto account = [&]( const BlindAstrometricSolver::Solution& S )
   {
      ++result.attempts;
      result.quadsTried += S.quadsTried;
      result.candidates += S.candidates;
      result.bestLogOdds = std::max( result.bestLogOdds, S.bestLogOdds );
      result.seconds += S.seconds;
   };

   BlindAstrometricSolver::Solution S = Solve( solver, stars, width, height );
   account( S );
   if ( S.solved )
   {
      result.solution = S;
      result.attempt = 1;
      return true;
   }

   // The second attempt: a binned copy when the brightest stars are large, else the image with
   // permissive parameters; the third: whichever of the two was not tried, when it applies.
   const DetectParams permissive = params.Permissive();
   const bool canPermit = !(permissive == params);
   const DetectParams q = canPermit ? permissive : params;
   int binning = gdg::blind_binning_factor( stars.brightFWHM, width, height );
   int next = (binning > 1) ? binning : (canPermit ? 1 : 0);
   bool triedBinned = false, triedImage = false;
   for ( int i = 0; i < 2 && next > 0 && !S.solved && !S.aborted; ++i )
   {
      const int k = next;
      gdg::log_info( log, gdg::strformat( "blind solver: attempt %d%s, detection scales %d, peak response %.2f, "
                                          "bright threshold %.2f, maximum star distortion %.2f", i + 2,
                                          (k > 1) ? gdg::strformat( " on the image binned %dx%d", k, k ).c_str() : "",
                                          q.structureLayers, q.peakResponse, q.brightThreshold, q.maxStarDistortion ) );
      const DetectedStars d = DetectImageStars( gray, q, k );
      if ( d.size() >= 6 )
      {
         S = Solve( solver, d, width, height );
         account( S );
         if ( S.solved )
         {
            result.solution = S;
            result.attempt = i + 2;
            result.binning = k;
            return true;
         }
      }
      if ( k > 1 )
         triedBinned = true;
      else
         triedImage = true;
      next = 0;
      if ( !triedImage && canPermit )
         next = 1;
      else if ( !triedBinned )
      {
         const int k2 = gdg::blind_binning_factor( d.brightFWHM, width, height );
         if ( k2 > 1 )
            next = k2;
      }
   }
   result.solution = S;
   return false;
}

} // namespace pcl
