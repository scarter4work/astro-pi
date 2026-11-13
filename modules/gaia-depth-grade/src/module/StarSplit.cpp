#include "StarSplit.h"

#include <pcl/ErrorHandler.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace pcl
{

const char* const kStarXTerminatorRequired =
   "StarXTerminator is required (a paid PixInsight module) but is not installed. Install it, then re-run "
   "Gaia Depth Grade.";

bool StarXTerminatorInstalled()
{
#ifdef GDG_PROBES
   if ( const char* hide = std::getenv( "GDG_TEST_HIDE_SXT" ) )
      if ( std::strcmp( hide, "1" ) == 0 )
         return false;
#endif
   // A const array: range-for over the temporary would call the non-const begin() (EnsureUnique),
   // which GCC 16 flags with a false -Walloc-size-larger-than.
   const Array<Process> processes = Process::AllProcesses();
   for ( const Process& P : processes )
      if ( P.Id() == "StarXTerminator" )
         return true;
   return false;
}

namespace
{

std::set<IsoString> OpenWindowIds()
{
   std::set<IsoString> ids;
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
      ids.insert( w.MainView().Id() );
   return ids;
}

} // namespace

ImageWindow SplitWithStarXTerminator( ImageWindow& work, const gdg::Log& log )
{
   if ( !StarXTerminatorInstalled() )
      throw Error( kStarXTerminatorRequired );

   const std::set<IsoString> before = OpenWindowIds();

   ProcessInstance sxt( Process( IsoString( "StarXTerminator" ) ) );
   // Parameter ids read from the installed process (probe introspect, 2026-11-10): the shipped
   // script's `sxt.stars = true` named no parameter (a no-op); the stars image is output_stars.
   // Row 0: the core refuses a value set with the default "no row" index (as SolverAlign, C17).
   if ( !sxt.SetParameterValue( true, IsoString( "output_stars" ), 0 ) ||
        !sxt.SetParameterValue( true, IsoString( "unscreen" ), 0 ) )
      throw Error( "StarXTerminator: cannot set output_stars / unscreen (an incompatible StarXTerminator version?)" );

   View view = work.MainView();
   const IsoString id = view.Id();
   if ( !sxt.ExecuteOn( view, false/*swapFile*/ ) ) // modifies the view in place -> starless
      throw Error( "StarXTerminator failed on " + String( id ) );

   ImageWindow stars;
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
   {
      const IsoString wid = w.MainView().Id();
      if ( before.count( wid ) == 0 )
      {
         if ( stars.IsNull() )
         {
            stars = w;
            gdg::log_info( log, std::string( "stars window: " ) + wid.c_str() );
         }
         else
         {
            ImageWindow extra = w;
            extra.ForceClose();
         }
      }
   }
   if ( stars.IsNull() )
      throw Error( "StarXTerminator produced no new stars window" );
   return stars;
}

View SuppliedLayer( const String& id, const char* what, int width, int height, int channels )
{
   if ( id.Trimmed().IsEmpty() )
      throw Error( String( "Split mode is SuppliedViews but no " ) + what + " view is set (" +
                   (std::strcmp( what, "stars" ) == 0 ? "starsViewId" : "starlessViewId") + ")." );
   View v = View::ViewById( IsoString( id.Trimmed() ) );
   if ( v.IsNull() )
      throw Error( String( "The " ) + what + " view '" + id.Trimmed() + "' does not exist." );
   const ImageVariant img = v.Image();
   if ( img.Width() != width || img.Height() != height || img.NumberOfNominalChannels() != channels )
      throw Error( String().Format( "The %s view '", what ) + id.Trimmed() +
                   String().Format( "' is %d x %d x %d; the target is %d x %d x %d. The stars and starless layers "
                                    "must have the target's geometry.",
                                    img.Width(), img.Height(), img.NumberOfNominalChannels(), width, height, channels ) );
   if ( img.IsComplexSample() )
      throw Error( String( "The " ) + what + " view '" + id.Trimmed() + "' is a complex image." );
   return v;
}

} // namespace pcl
