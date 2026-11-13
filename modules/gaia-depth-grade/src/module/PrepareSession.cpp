#include "PrepareSession.h"

#include "ImageBridge.h"
#include "LocalGaia.h"
#include "PclSky.h"
#include "StarSplit.h"
#include "TapClient.h"

#include "core/Footprint.h"
#include "core/Format.h"

#include <pcl/ErrorHandler.h>

#include <optional>

namespace pcl
{

namespace
{

std::string U8( const String& s )
{
   return std::string( s.ToUTF8().c_str() );
}

/// Force-closes a window this run made (a no-op for a null one).
struct WindowGuard
{
   ImageWindow window;
   ~WindowGuard()
   {
      if ( !window.IsNull() )
         try
         {
            window.ForceClose();
         }
         catch ( ... )
         {
         }
   }
   void Release() { window = ImageWindow(); }
};

/// A copy of the target's main view in a new, hidden window: pixels and keywords (the PJSR
/// duplicateWindow; the solver seeds from the keywords).
ImageWindow Duplicate( const View& target, const IsoString& id )
{
   const ImageVariant src = target.Image();
   ImageWindow w( src.Width(), src.Height(), src.NumberOfNominalChannels(), src.BitsPerSample(), src.IsFloatSample(),
                  src.IsColor(), false/*initialProcessing*/, id );
   if ( w.IsNull() )
      throw Error( "Could not create the working window " + String( id ) );
   ImageVariant dst = w.MainView().Image();
   dst.CopyImage( src );
   w.SetKeywords( target.Window().Keywords() );
   return w;
}

} // namespace

WorkRunner RunInline( const gdg::Log& log )
{
   return [log]( const std::function<void( const gdg::Log& )>& work ) { work( log ); };
}

String TargetClosedMessage( const IsoString& id )
{
   return "The target image '" + String( id ) + "' was closed after Prepare. Select a target and Prepare again.";
}

String PrepareSession::StaleReason() const
{
   if ( targetId.IsEmpty() )
      return "Nothing is prepared. Click Prepare.";
   const View v = View::ViewById( targetId );
   if ( v.IsNull() || !v.IsMainView() )
      return TargetClosedMessage( targetId );
   const ImageVariant img = v.Image();
   if ( img.Width() != width || img.Height() != height || img.NumberOfNominalChannels() != channels )
      return "The target image '" + String( targetId ) + "' changed geometry after Prepare. Prepare again.";
   return String();
}

View PrepareSession::Target() const
{
   const String why = StaleReason();
   if ( !why.IsEmpty() )
      throw Error( why );
   return View::ViewById( targetId );
}

PrepareSession RunPrepare( const View& target, const GradeOptions& options, const gdg::Log& log, const WorkRunner& run )
{
   if ( target.IsNull() )
      throw Error( "no target selected" );
   if ( !target.IsMainView() )
      throw Error( "Gaia Depth Grade works on a whole image (a main view), not on the preview " + String( target.FullId() ) );
   const ImageVariant timg = target.Image();
   if ( timg.IsComplexSample() )
      throw Error( "Gaia Depth Grade cannot process complex images." );

   PrepareSession s;
   s.targetId = target.Id();
   s.width = timg.Width();
   s.height = timg.Height();
   s.channels = timg.NumberOfNominalChannels();
   s.keywords = target.Window().Keywords();
   s.options = options;

   // The supplied layers are checked before a minute of solving.
   View suppliedStars, suppliedStarless;
   if ( options.suppliedViews )
   {
      suppliedStars = SuppliedLayer( options.starsViewId, "stars", s.width, s.height, s.channels );
      suppliedStarless = SuppliedLayer( options.starlessViewId, "starless", s.width, s.height, s.channels );
   }
   else if ( !StarXTerminatorInstalled() )
      throw Error( kStarXTerminatorRequired );

   // 1. The working copy; the master is only read.
   WindowGuard work;
   work.window = Duplicate( target, s.targetId + "_gdgwork" );
   const IsoString workId = work.window.MainView().Id();
   gdg::log_info( log, std::string( "working copy: " ) + workId.c_str() );

   // 2. Plate solve.
   SolverParams sp;
   sp.gaiaDatabaseFiles = options.gaiaDatabaseFiles;
   sp.indexFile = options.solveIndexFile;
   sp.onlineCatalogFallback = options.settings.online;
   s.solve = SolveImage( work.window, sp, log );
   if ( !s.solve.ok )
   {
      // The remedy-naming errors reach the user verbatim (Review Focus 1; queryLocalGaiaDR3's).
      if ( s.solve.error == kUnsolvableMessage || s.solve.error == kNoLocalGaiaMessage )
         throw Error( s.solve.error );
      throw Error( "plate solve failed for " + String( workId ) + ": " + s.solve.error );
   }
   gdg::log_info( log, gdg::strformat( "plate solved (%s, %s): %d stars, RMS %.3f px, %.1f s",
                                       U8( s.solve.method ).c_str(), U8( s.solve.catalog ).c_str(), s.solve.matches,
                                       s.solve.rmsPx, s.solve.seconds ) );
   s.solution = std::make_shared<AstrometricMetadata>( work.window );
   if ( !s.solution->IsValid() )
      throw Error( "plate solve failed for " + String( workId ) + ": no valid astrometric solution was written" );

   // 3. The split.
   if ( options.suppliedViews )
   {
      s.stars = ImageBridge::ToCore( suppliedStars.Image() );
      s.starless = ImageBridge::ToCore( suppliedStarless.Image() );
   }
   else
   {
      WindowGuard starsWindow;
      starsWindow.window = SplitWithStarXTerminator( work.window, log );
      s.stars = ImageBridge::ToCore( starsWindow.window.MainView().Image() );
      s.starless = ImageBridge::ToCore( work.window.MainView().Image() );
   }
   if ( !s.stars.same_shape( s.starless ) )
      throw Error( String().Format( "the stars layer (%d x %d x %d) and the starless layer (%d x %d x %d) differ",
                                    s.stars.width, s.stars.height, s.stars.channels, s.starless.width,
                                    s.starless.height, s.starless.channels ) );

   // 4. The catalogue for the field, fetched here (the database files and the network live on
   //    this thread; C14), then detection and cross-match on the runner.
   const PclSky sky( *s.solution );
   const gdg::GradeConfig cfg = gdg::grade_config( options.settings );
   const gdg::Footprint fp = gdg::field_footprint( sky, s.stars.height, s.stars.width );
   gdg::Catalog catalog;
   if ( options.settings.online )
   {
      gdg::log_info( log, "Running prepare (detect + online Gaia query). On a dense field the async query can take "
                          "several minutes the first time; the result is cached so re-runs are fast..." );
      OnlineGaiaSource src( cfg.gaia_mag_limit, log );
      catalog = src.distances_for( fp );
   }
   else
   {
      gdg::log_info( log, "Querying local Gaia DR3 (offline)..." );
      const StringList files = options.gaiaDatabaseFiles.IsEmpty() ? DiscoverGaiaDatabases() : options.gaiaDatabaseFiles;
      LocalGaiaSource src( files, cfg.gaia_mag_limit, cfg.gaia_parallax_snr, log );
      catalog = src.distances_for( fp );
      gdg::log_info( log, gdg::strformat( "Local Gaia DR3: %d sources. Running prepare (detect + match)...",
                                          int( catalog.size() ) ) );
   }
   gdg::FixedCatalogSource fixed( fp, std::move( catalog ) );
   run( [&]( const gdg::Log& wl ) { s.prep = gdg::prepare_grade( s.stars, sky, cfg, fixed, wl ); } );
   return s; // the working and stars windows are force-closed here (guards)
}

gdg::Image RenderComposite( const PrepareSession& s, const gdg::GradeSettings& settings, const WorkRunner& run )
{
   gdg::Image out;
   run( [&]( const gdg::Log& wl ) { out = gdg::grade_composite( s.stars, s.starless, s.prep, settings, wl ); } );
   return out;
}

FITSKeywordArray TaggedKeywords( const FITSKeywordArray& keywords )
{
   FITSKeywordArray k;
   for ( const FITSHeaderKeyword& kw : keywords )
      if ( kw.name.Trimmed().CompareIC( "DEPTHTAG" ) != 0 )
         k << kw;
   k << FITSHeaderKeyword( "DEPTHTAG", "'" + IsoString( gdg::kHonesty ) + "'", "Gaia Depth Grade" )
     << FITSHeaderKeyword( "HISTORY", IsoString(), IsoString( gdg::kHonesty ) );
   return k;
}

ImageWindow CreateOutputWindow( const PrepareSession& s, const gdg::Image& composite )
{
   pcl::Image img;
   ImageBridge::ToPcl( composite, img );
   ImageWindow w( img.Width(), img.Height(), img.NumberOfChannels(), 32, true/*float*/, img.IsColor(),
                  false/*initialProcessing*/, s.targetId + "_depthgraded" );
   if ( w.IsNull() )
      throw Error( "Could not create the output window " + String( s.targetId ) + "_depthgraded" );
   try
   {
      ImageVariant v = w.MainView().Image();
      v.CopyImage( img );
      w.SetKeywords( TaggedKeywords( s.keywords ) );
      if ( s.solution && s.solution->IsValid() )
      {
         s.solution->Write( w, false/*notify*/ );
         w.RegenerateAstrometricSolution( false/*allowGUIMessages*/ );
      }
   }
   catch ( ... )
   {
      w.ForceClose();
      throw;
   }
   return w;
}

void WriteIntoView( View& view, const gdg::Image& composite )
{
   pcl::Image img;
   ImageBridge::ToPcl( composite, img );
   ImageVariant v = view.Image();
   if ( v.Width() != img.Width() || v.Height() != img.Height() || v.NumberOfNominalChannels() != img.NumberOfChannels() )
      throw Error( "The graded image does not match the target's geometry." );
   v.CopyImage( img );
   ImageWindow w = view.Window();
   w.SetKeywords( TaggedKeywords( w.Keywords() ) );
}

} // namespace pcl
