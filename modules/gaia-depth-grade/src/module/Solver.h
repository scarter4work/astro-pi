// The module's native plate solver (plan Tasks 14-15): ImageSolver 6.6's algorithm in C++.
//
//   seed (existing solution, or RA/DEC + focal length + pixel size)  -- or a blind solution
//   -> StarDetector + PSF fits on the HSI intensity
//   -> local Gaia DR3 (pcl::GaiaDatabaseFile), astrometric places at the observation time
//   -> initial alignment: stock StarAlignment on a catalogue template (SolverAlign)
//   -> iterative refinement: predicted-position matching, sparsity outlier rejection, and a
//      SplineWorldTransformation (distortion) or a projective fit (linear)
//   -> AstrometricMetadata::Write on the window.
//
// Ported from ImageSolver 6.6.0 (/opt/PixInsight/src/scripts/ImageSolver/ImageSolverEngine.js):
// Copyright (c) 2013-2026 Andres del Pozo, Copyright (c) 2019-2026 Juan Conejero (PTeam);
// PixInsight Class Library License Version 2.0.
#ifndef __GDG_Solver_h
#define __GDG_Solver_h

#include "core/Log.h"

#include <pcl/ImageWindow.h>
#include <pcl/StringList.h>

namespace pcl
{

/// When the blind solver runs (ImageSolver's BlindSolverMode).
enum class BlindMode
{
   Automatic, // when the image provides no trusted centre and scale, or the seeded alignment fails
   Always,
   Disabled
};

struct SolverParams
{
   StringList gaiaDatabaseFiles;       // empty: DiscoverGaiaDatabases()
   String     indexFile;               // empty: the Gaia process' configured astrometric index
   double     magLimit = 0;            // 0: automatic, as ImageSolver does
   bool       distortion = true;       // spline distortion model (ImageSolver's default)
   BlindMode  blind = BlindMode::Automatic;
   bool       useExistingSolution = true; // seed from an astrometric solution the image already has
   /// With no database files given and none configured: plate-solve with the ESA archive's Gaia
   /// DR3 positions (plan C21) instead of failing with DiscoverGaiaDatabases' error. Only the
   /// online catalogue mode allows it: offline mode makes no network connection.
   bool       onlineCatalogFallback = false;
};

struct SolveReport
{
   bool   ok = false;
   String method;              // "seeded" or "blind+seeded"
   int    detected = 0;        // stars detected (before PSF fits)
   int    matches = 0;         // matched stars of the final solution
   double rmsPx = 0;           // RMS of the final matching, pixels
   double limitMagnitude = 0;
   int    catalogStars = 0;
   String catalog;             // the solver catalogue's name: "Gaia DR3 (XPSD)" or "Gaia DR3 (ESA archive)"
   int    iterations = 0;
   double seconds = 0;
   double blindSeconds = 0;
   int    blindAttempt = 0;    // which blind attempt solved (1..3), 0 when not used
   String error;
};

/// The error raised when neither a seed nor an astrometric index exists (Review Focus 1).
extern const char* const kUnsolvableMessage;

/// Plate-solves `work` and writes the solution to it. Never throws for solving failures: they
/// are reported in SolveReport::error (Error exceptions from PCL are caught and reported too).
SolveReport SolveImage( ImageWindow& work, const SolverParams& p, const gdg::Log& log );

/// The astrometric index file configured for PixInsight's Gaia process (its get-info result
/// astrometricIndexFilePath), as configured (the caller checks that it exists); empty when none
/// is configured or the Gaia process is not installed. Throws Error when the query fails.
String ConfiguredAstrometricIndex();

} // namespace pcl

#endif // __GDG_Solver_h
