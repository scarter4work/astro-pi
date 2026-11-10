// The solver's blind fallback (plan Task 15): PCL 2.13's BlindAstrometricSolver over an
// astrometric index, with ImageSolver 6.6's three detection attempts (blindSolve()).
#ifndef __GDG_SolverBlind_h
#define __GDG_SolverBlind_h

#include "SolverDetect.h"

#include "core/Log.h"

#include <pcl/BlindAstrometricSolver.h>
#include <pcl/String.h>

namespace pcl
{

struct BlindResult
{
   BlindAstrometricSolver::Solution solution;
   int attempt = 0;      // 1: the solver's stars; 2, 3: permissive detection (possibly binned)
   int binning = 1;      // binning of the attempt that solved
   int attempts = 0;
   uint64 quadsTried = 0;
   uint64 candidates = 0;
   double bestLogOdds = 0;
   double seconds = 0;
};

/// Solves blindly from `stars` (the solver's detection with `params`) and, when that fails, up
/// to two more attempts with permissive detection parameters on a binned copy and/or the image
/// itself (ImageSolver's order). Returns false with the result's diagnostics when no attempt
/// solves; throws Error when the index cannot be opened.
bool BlindSolve( const ImageVariant& gray, const DetectedStars& stars, const DetectParams& params,
                 const String& indexPath, BlindResult& result, const gdg::Log& log );

} // namespace pcl

#endif // __GDG_SolverBlind_h
