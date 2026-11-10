// The solver's initial field alignment: PixInsight's stock StarAlignment process run from C++
// through pcl::ProcessInstance on two star lists, as ImageSolver 6.6's registerWithTemplate()
// does (plan Task 14).
#ifndef __GDG_SolverAlign_h
#define __GDG_SolverAlign_h

#include <pcl/Array.h>
#include <pcl/Point.h>
#include <pcl/String.h>

namespace pcl
{

class View;

/// One star of a StarAlignment CSV star list.
struct AlignStar
{
   double x, y, flux;
};

/// The matched star pairs: pS on the reference (catalogue template), pI on the target image, in
/// PixInsight image coordinates (the target positions carry ImageSolver's +0.5 shift, l.439).
struct AlignPairs
{
   Array<DPoint> pS, pI;
};

/// StarAlignment's enumeration values, as the running PixInsight 1.9.6 defines them
/// (StarAlignment.prototype.OutputMatrix etc., recorded by the probe `starAlignment_enums`;
/// the C++ API does not publish enumeration element identifiers for core processes).
namespace StarAlignmentEnum
{
   constexpr int OutputMatrix   = 8;
   constexpr int NoIntersection = 0;
   constexpr int Always         = 2;
}

/// The star-list CSV format StarAlignment reads (referenceIsFile / targetCSVFilePath): a first
/// line "width,height", then one "x,y,flux" line per star (%.4f,%.4f,%.3e).
IsoString StarListCSV( int width, int height, const Array<AlignStar>& stars );

/// Runs StarAlignment in OutputMatrix mode with ImageSolver's parameters on the reference star
/// list `referenceCSV` and the target star list `targetCSV` (files), executed on `view` (which
/// supplies only the execution context: both star lists come from the files). `numStars` is the
/// reference star count (exhaustive matching up to 2000 stars, else 1.25 x numStars descriptors,
/// up to 2000; zero forces exhaustive matching). `intersectionAlways`: StarAlignment.Always,
/// else NoIntersection. Returns false when StarAlignment finds no valid transformation; up to
/// 4000 pairs otherwise.
bool RegisterWithTemplate( View& view, const String& referenceCSV, const String& targetCSV, int numStars,
                           bool distortionCorrection, bool intersectionAlways, AlignPairs& pairs );

} // namespace pcl

#endif // __GDG_SolverAlign_h
