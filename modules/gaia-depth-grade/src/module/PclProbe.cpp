// Link probe (plan Task 2): proves the module links PCL 2.13's astrometric
// index and blind solver from the static libPCL it is built against. The
// native plate solver (Tasks 14-15) replaces this file.

#include <pcl/AstrometricIndexFile.h>
#include <pcl/BlindAstrometricSolver.h>

namespace pcl
{

// Never called; an external (hidden, version-script local) function the
// compiler must keep, so its references pull the solver's code into the link,
// where -Wl,-z,defs fails the build if libPCL lacks any of it.
[[gnu::used]] int GDGPclProbeSolve( const String& indexPath );
[[gnu::used]] int GDGPclProbeSolve( const String& indexPath )
{
   AstrometricIndexFile index( indexPath );
   BlindAstrometricSolver solver( index );
   Array<DPoint> positions;
   Array<float> fluxes;
   return solver.Solve( positions, fluxes, 1, 1 ).solved ? 1 : 0;
}

} // namespace pcl
