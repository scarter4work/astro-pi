// Prepare / render / output for the GaiaDepthGrade process (plan Tasks 16-17): the two phases of
// the dialog (GaiaDepthGradeDialog.js doPrepare, doPreview, doExecute) and the headless script
// (gaia_depth_grade.js), in one place for the instance's ExecuteOn and the interface.
//
// Prepare: duplicate the target to <id>_gdgwork (pixels and keywords; the master is never
// written), plate-solve the duplicate, split it (StarXTerminator, or the supplied layers),
// bring both layers into the core, fetch the catalogue, detect and cross-match. The session
// keeps the layers and the match in memory and force-closes every window it made.
//
// Threads (plan C14): everything that touches PixInsight objects -- windows, processes, the
// Gaia database files and the online catalogue's NetworkTransfer (whose event receiver is a
// Control) -- runs on the calling (GUI) thread, the thread TapClient was verified on. Only the
// PCL-free core stages (detection + cross-match against the fetched catalogue, the render, the
// nebula grade, the blend, the preview stretch) go through the WorkRunner, which the interface
// runs on a worker thread while it keeps the GUI responsive.
#ifndef __GDG_PrepareSession_h
#define __GDG_PrepareSession_h

#include "Solver.h"

#include "core/Grade.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/Pipeline.h"

#include <pcl/AstrometricMetadata.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/ImageWindow.h>
#include <pcl/StringList.h>
#include <pcl/View.h>

#include <functional>
#include <memory>

namespace pcl
{

/// Everything a run reads from the process instance.
struct GradeOptions
{
   gdg::GradeSettings settings;
   bool       suppliedViews = false;  // splitMode = SuppliedViews
   String     starsViewId, starlessViewId;
   String     solveIndexFile;         // empty: the Gaia process' configured astrometric index
   StringList gaiaDatabaseFiles;      // empty: DiscoverGaiaDatabases()
};

/// Runs a PCL-free stage. The stage logs through the Log it is given, which the runner makes
/// safe for its thread. RunInline() runs it on the calling thread with `log`.
using WorkRunner = std::function<void( const std::function<void( const gdg::Log& )>& )>;
WorkRunner RunInline( const gdg::Log& log );

/// What Prepare leaves for Preview and Execute.
struct PrepareSession
{
   IsoString           targetId;       // the target's main view id at Prepare time
   int                 width = 0, height = 0, channels = 0;
   FITSKeywordArray    keywords;       // the target's, for the output window
   gdg::Image          stars, starless;
   gdg::PrepResult     prep;
   GradeOptions        options;        // as prepared (prepare-time fields decide staleness)
   SolveReport         solve;
   std::shared_ptr<AstrometricMetadata> solution;

   /// Empty while the target still exists with Prepare's geometry; otherwise why the session
   /// cannot be used (Review Focus 4: "... was closed ... Prepare again").
   String StaleReason() const;

   /// The target's main view; throws pcl::Error with StaleReason() when stale.
   View Target() const;
};

/// The Review Focus 4 text for a target that is gone.
String TargetClosedMessage( const IsoString& id );

/// Prepare on `target` (a main view). Throws pcl::Error with the reason on any failure (the
/// solver's error, the split's, the catalogue's verbatim); every window it made is closed.
PrepareSession RunPrepare( const View& target, const GradeOptions& options, const gdg::Log& log, const WorkRunner& run );

/// The recombined image for `settings` (gdg::grade_composite of the session's layers).
gdg::Image RenderComposite( const PrepareSession& session, const gdg::GradeSettings& settings, const WorkRunner& run );

/// A new 32-bit float window <targetId>_depthgraded (PixInsight may append a suffix when the id
/// is taken) holding `composite`, with the target's keywords plus DEPTHTAG = the honesty text and
/// a HISTORY line, and the solution Prepare computed. Not shown.
ImageWindow CreateOutputWindow( const PrepareSession& session, const gdg::Image& composite );

/// Writes `composite` into `view` (outputMode ReplaceTarget: same geometry, the view's own
/// sample format) and adds DEPTHTAG and HISTORY to its window's keywords.
void WriteIntoView( View& view, const gdg::Image& composite );

/// The target keywords with any DEPTHTAG removed, plus DEPTHTAG and the HISTORY line.
FITSKeywordArray TaggedKeywords( const FITSKeywordArray& keywords );

} // namespace pcl

#endif // __GDG_PrepareSession_h
