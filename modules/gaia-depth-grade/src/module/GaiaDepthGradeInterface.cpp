#include "GaiaDepthGradeInterface.h"

#include "GaiaDepthGradeParameters.h"
#include "GaiaDepthGradeProcess.h"

#include <pcl/Console.h>
#include <pcl/Cursor.h>
#include <pcl/ErrorHandler.h>
#include <pcl/FileDialog.h>
#include <pcl/Frame.h>
#include <pcl/Graphics.h>
#include <pcl/MetaModule.h>
#include <pcl/StandardStatus.h>
#include <pcl/StatusMonitor.h>
#include <pcl/Thread.h>

#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace pcl
{

GaiaDepthGradeInterface* TheGaiaDepthGradeInterface = nullptr;

namespace
{

const char* const kTitle = "Gaia Depth Grade"; // UI_TITLE
const char* const kPickTarget = "Pick a target and click Prepare.";

String S( const std::string& u8 )
{
   return String::UTF8ToUTF16( u8.c_str() );
}

/// The non-empty lines of a text, trimmed (the Gaia DR3 files box).
StringList NonEmptyLines( const String& text )
{
   StringList lines, out;
   text.Break( lines, '\n', true/*trim*/ );
   for ( const String& l : lines )
      if ( !l.IsEmpty() )
         out << l;
   return out;
}

/// The prepare-time options differ (the fields a Prepare depends on).
bool PrepareOptionsDiffer( const GradeOptions& a, const GradeOptions& b )
{
   return gdg::prepare_settings_differ( a.settings, b.settings ) || a.suppliedViews != b.suppliedViews ||
          a.starsViewId != b.starsViewId || a.starlessViewId != b.starlessViewId || a.solveIndexFile != b.solveIndexFile ||
          a.gaiaDatabaseFiles != b.gaiaDatabaseFiles;
}

String MessageOf( std::exception_ptr e )
{
   try
   {
      std::rethrow_exception( e );
   }
   catch ( const Exception& x )
   {
      return x.Message();
   }
   catch ( const std::exception& x )
   {
      return String::UTF8ToUTF16( x.what() );
   }
   catch ( ... )
   {
      return "unknown error";
   }
}

/// An 8-bit (h, w, channels) buffer as an opaque bitmap (grey for one channel).
Bitmap MakeBitmap( const std::vector<uint8_t>& px, int w, int h, int channels )
{
   if ( w < 1 || h < 1 )
      return Bitmap::Null();
   Bitmap bmp( w, h, BitmapFormat::RGB32 );
   for ( int y = 0; y < h; ++y )
   {
      RGBA* row = bmp.ScanLine( y );
      for ( int x = 0; x < w; ++x )
      {
         const size_t i = (size_t( y )*w + x)*channels;
         const uint8_t r = px[i], g = channels == 3 ? px[i + 1] : r, b = channels == 3 ? px[i + 2] : r;
         row[x] = RGBAColor( r, g, b );
      }
   }
   return bmp;
}

/// A thread-safe message queue: the worker's log lines, written to the console by the GUI thread.
struct LogQueue
{
   std::mutex                                 mutex;
   std::vector<std::pair<bool, std::string>>  lines; // (warning, text)

   void Push( bool warning, const std::string& s )
   {
      std::lock_guard<std::mutex> lock( mutex );
      lines.emplace_back( warning, s );
   }

   void Flush( const gdg::Log& to )
   {
      std::vector<std::pair<bool, std::string>> v;
      {
         std::lock_guard<std::mutex> lock( mutex );
         v.swap( lines );
      }
      for ( const auto& l : v )
         if ( l.first )
            gdg::log_warning( to, l.second );
         else
            gdg::log_info( to, l.second );
   }
};

/// The PCL-free stage of a Prepare, Preview or Execute, run on a pcl::Thread. Its exception (if
/// any) is kept and rethrown on the GUI thread.
class WorkThread : public Thread
{
public:

   WorkThread( std::function<void()> work )
      : m_work( std::move( work ) )
   {
   }

   void Run() override
   {
      try
      {
         m_work();
      }
      catch ( ... )
      {
         error = std::current_exception();
      }
      finished.store( true );
   }

   std::exception_ptr error;
   // Set as Run() ends: the GUI thread polls this, not Wait( ms ), which also answers true for a
   // thread that is not running yet.
   std::atomic<bool> finished{ false };

private:

   std::function<void()> m_work;
};

} // namespace

#ifdef GDG_PROBES
#define GDG_RECORD( action, ok ) RecordAction( action, ok )
#define GDG_WRITE_STATE() WriteState()
#else
#define GDG_RECORD( action, ok ) ((void)0)
#define GDG_WRITE_STATE() ((void)0)
#endif

// ----------------------------------------------------------------------------

GaiaDepthGradeInterface::GaiaDepthGradeInterface()
   : instance( TheGaiaDepthGradeProcess )
{
   TheGaiaDepthGradeInterface = this;
}

GaiaDepthGradeInterface::~GaiaDepthGradeInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

IsoString GaiaDepthGradeInterface::Id() const
{
   return "GaiaDepthGrade";
}

MetaProcess* GaiaDepthGradeInterface::Process() const
{
   return TheGaiaDepthGradeProcess;
}

InterfaceFeatures GaiaDepthGradeInterface::Features() const
{
   // New instance (drag), Apply to the active image (the full ExecuteOn), documentation, reset.
   return InterfaceFeature::Default;
}

void GaiaDepthGradeInterface::ResetInstance()
{
   GaiaDepthGradeInstance defaultInstance( TheGaiaDepthGradeProcess );
   ImportProcess( defaultInstance );
}

bool GaiaDepthGradeInterface::Launch( const MetaProcess& P, const ProcessImplementation*, bool& dynamic, unsigned& )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( kTitle );
      UpdateControls();
      // The default target: the active window, if any.
      const ImageWindow aw = ImageWindow::ActiveWindow();
      if ( !aw.IsNull() )
         SetTarget( aw.MainView() );
      else
         SetStatus( kPickTarget );
   }
   dynamic = false;
   return &P == TheGaiaDepthGradeProcess;
}

ProcessImplementation* GaiaDepthGradeInterface::NewProcess() const
{
   return new GaiaDepthGradeInstance( instance );
}

bool GaiaDepthGradeInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const GaiaDepthGradeInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not a GaiaDepthGrade instance.";
   return false;
}

bool GaiaDepthGradeInterface::RequiresInstanceValidation() const
{
   return true;
}

bool GaiaDepthGradeInterface::ImportProcess( const ProcessImplementation& p )
{
   if ( m_busy )
   {
      // A process icon dropped (or Reset) while Prepare / Preview / Execute runs: refused, so the
      // settings the run uses stay the ones it started with.
      Console().WarningLn( "<end><cbr>[GDG] ** Warning: Gaia Depth Grade is busy; the instance was not imported. "
                           "Try again when it has finished." );
      return false;
   }
   const GradeOptions before = instance.Options();
   instance.Assign( p );
   if ( GUI != nullptr )
   {
      if ( PrepareOptionsDiffer( before, instance.Options() ) )
         MarkPrepareStale();
      else
         MarkPreviewStale();
      UpdateControls();
   }
   return true;
}

// ----------------------------------------------------------------------------

void GaiaDepthGradeInterface::UpdateControls()
{
   if ( GUI == nullptr )
      return;
   struct Updating
   {
      bool& flag;
      bool  was;
      Updating( bool& f ) : flag( f ), was( f ) { flag = true; }
      ~Updating() { flag = was; }
   } updating( m_updating );
   GUI->Brightness_NumericControl.SetValue( instance.p_brightness );
   GUI->Size_NumericControl.SetValue( instance.p_size );
   GUI->Contrast_NumericControl.SetValue( instance.p_contrast );
   GUI->Saturation_NumericControl.SetValue( instance.p_saturation );
   GUI->HaloSigma_NumericControl.SetValue( instance.p_haloSigma );
   GUI->PLow_NumericControl.SetValue( instance.p_pLow );
   GUI->PHigh_NumericControl.SetValue( instance.p_pHigh );
   GUI->Nebula_CheckBox.SetChecked( instance.p_nebulaEnabled );
   GUI->NebulaStrength_NumericControl.SetValue( instance.p_nebulaStrength );
   GUI->NebulaAtmos_NumericControl.SetValue( instance.p_nebulaAtmos );
   GUI->NebulaStructure_NumericControl.SetValue( instance.p_nebulaStructure );
   GUI->Detect_NumericEdit.SetValue( instance.p_detectSigma );
   GUI->Match_NumericEdit.SetValue( instance.p_matchTolerance );
   GUI->Offline_CheckBox.SetChecked( instance.p_catalog == GDGCatalog::Offline );

   GUI->HaloGain_NumericControl.SetValue( instance.p_haloGain );
   GUI->DetectFWHM_NumericEdit.SetValue( instance.p_detectFWHM );
   GUI->MinMatchRate_NumericEdit.SetValue( instance.p_minMatchRate );
   GUI->Neutral_NumericEdit.SetValue( instance.p_neutralStrength );
   GUI->MagLimit_CheckBox.SetChecked( instance.p_magLimitEnabled );
   GUI->MagLimit_NumericEdit.SetValue( instance.p_magLimit );
   GUI->MagLimit_NumericEdit.Enable( instance.p_magLimitEnabled );
   GUI->ParallaxSNR_NumericEdit.SetValue( instance.p_parallaxSNR );
   GUI->HaloMinRefs_SpinBox.SetValue( instance.p_haloMinRefs );
   GUI->HaloRadius_NumericEdit.SetValue( instance.p_haloRadiusSigma );
   GUI->HaloBeta_NumericEdit.SetValue( instance.p_haloMoffatBeta );
   GUI->Split_ComboBox.SetCurrentItem( instance.p_splitMode );
   const bool supplied = instance.p_splitMode == GDGSplitMode::SuppliedViews;
   GUI->Stars_ViewList.Enable( supplied );
   GUI->Starless_ViewList.Enable( supplied );
   if ( !instance.p_starsViewId.IsEmpty() )
      GUI->Stars_ViewList.SelectView( View::ViewById( IsoString( instance.p_starsViewId ) ) );
   if ( !instance.p_starlessViewId.IsEmpty() )
      GUI->Starless_ViewList.SelectView( View::ViewById( IsoString( instance.p_starlessViewId ) ) );
   GUI->Output_ComboBox.SetCurrentItem( instance.p_outputMode );
   if ( GUI->Index_Edit.Text().Trimmed() != instance.p_solveIndexFile )
      GUI->Index_Edit.SetText( instance.p_solveIndexFile );
   // Rewritten only when it names other files: never under the user's caret while typing.
   if ( NonEmptyLines( GUI->Files_TextBox.Text() ) != instance.p_gaiaDatabaseFiles )
      GUI->Files_TextBox.SetText( String().ToNewLineSeparated( instance.p_gaiaDatabaseFiles ) );
   GUI->QA_Label.SetText( m_qaLine );

   // While busy every input is off (review I1); the handlers refuse too (m_busy).
   const bool idle = !m_busy;
   Control* const inputs[] = { &GUI->Brightness_NumericControl, &GUI->Size_NumericControl, &GUI->Contrast_NumericControl,
                               &GUI->Saturation_NumericControl, &GUI->HaloSigma_NumericControl, &GUI->PLow_NumericControl,
                               &GUI->PHigh_NumericControl, &GUI->Nebula_CheckBox, &GUI->NebulaStrength_NumericControl,
                               &GUI->NebulaAtmos_NumericControl, &GUI->NebulaStructure_NumericControl,
                               &GUI->Detect_NumericEdit, &GUI->Match_NumericEdit, &GUI->Offline_CheckBox,
                               &GUI->Advanced_SectionBar, &GUI->Advanced_Control, &GUI->Preview_Control,
                               &GUI->Target_ViewList, &GUI->Browse_PushButton, &GUI->Close_PushButton };
   for ( Control* c : inputs )
      c->Enable( idle );
   GUI->Prepare_PushButton.Enable( idle && !m_targetId.IsEmpty() );
   GUI->Preview_PushButton.Enable( idle && m_prepared );
   GUI->Execute_PushButton.Enable( idle && m_prepared );
}

void GaiaDepthGradeInterface::SetStatus( const String& msg )
{
   m_status = msg;
#ifdef GDG_PROBES
   m_statusLog << m_status;
#endif
   if ( GUI != nullptr )
      GUI->Status_Label.SetText( msg );
   Console().NoteLn( "<end><cbr>[GDG] " + msg.EncodedHTMLSpecialChars() );
   Module->ProcessEvents( true/*excludeUserInputEvents*/ );
   GDG_WRITE_STATE();
}

void GaiaDepthGradeInterface::SetError( const String& msg )
{
   m_status = "ERROR: " + msg;
#ifdef GDG_PROBES
   m_statusLog << m_status;
#endif
   if ( GUI != nullptr )
      GUI->Status_Label.SetText( m_status );
   Console().CriticalLn( "<end><cbr>[GDG] ERROR: " + msg.EncodedHTMLSpecialChars() );
   Module->ProcessEvents( true );
   GDG_WRITE_STATE();
}

void GaiaDepthGradeInterface::SetTarget( const View& view )
{
   if ( view.IsNull() )
      return;
   const View main = view.Window().MainView();
   m_targetId = main.Id();
   if ( GUI != nullptr )
   {
      const bool was = m_updating;
      m_updating = true; // SelectView may report the selection back (e_ViewSelected)
      GUI->Target_ViewList.SelectView( main );
      m_updating = was;
   }
   MarkPrepareStale();
   SetStatus( "Target: " + String( m_targetId ) + String::UTF8ToUTF16( " \xE2\x80\x94 click Prepare." ) );
}

void GaiaDepthGradeInterface::MarkPrepareStale()
{
   m_prepared = false;
   m_session.reset();
   m_qaLine.Clear();
   m_previewStale = true;
   UpdateControls();
   if ( GUI != nullptr )
      GUI->Preview_Control.Update();
}

void GaiaDepthGradeInterface::MarkPreviewStale()
{
   m_previewStale = true;
   if ( GUI != nullptr )
      GUI->Preview_Control.Update();
   GDG_WRITE_STATE();
}

void GaiaDepthGradeInterface::SetBusy( bool busy )
{
   m_busy = busy;
   if ( GUI != nullptr )
      SetCursor( pcl::Cursor( busy ? StdCursor::Wait : StdCursor::Arrow ) );
   UpdateControls();
   GDG_WRITE_STATE();
}

bool GaiaDepthGradeInterface::SessionMatchesSettings() const
{
   return !m_prepared || (m_session && !PrepareOptionsDiffer( m_session->options, instance.Options() ));
}

gdg::Log GaiaDepthGradeInterface::GuiLog()
{
   // Info lines are the status line's progress too (the dialog's setStatus), warnings only go
   // to the console. Both are mirrored there as [GDG] lines.
   const gdg::Log console = ConsoleLog();
   return gdg::Log{ [this, console]( const std::string& m )
                    {
                       gdg::log_info( console, m );
                       m_status = S( m );
#ifdef GDG_PROBES
                       m_statusLog << m_status;
#endif
                       if ( GUI != nullptr )
                          GUI->Status_Label.SetText( m_status );
                       Module->ProcessEvents( true );
                    },
                    [console]( const std::string& m )
                    {
                       gdg::log_warning( console, m );
                       Module->ProcessEvents( true );
                    } };
}

WorkRunner GaiaDepthGradeInterface::GuiRunner( const String& what )
{
   // The PCL-free stage on a pcl::Thread. The GUI thread keeps painting, writes the stage's log
   // lines (the console and the status line live on this thread), and drives a StatusMonitor
   // (unbounded: the core stages report no counts) on the console's standard status. Events are
   // processed meanwhile -- here without user input, but the StatusMonitor's abort check processes
   // events too -- so the window's inputs are disabled and its handlers refuse while busy (SetBusy).
   return [this, what]( const std::function<void( const gdg::Log& )>& work )
   {
      const gdg::Log to = GuiLog();
      auto queue = std::make_shared<LogQueue>();
      const gdg::Log wl{ [queue]( const std::string& m ) { queue->Push( false, m ); },
                         [queue]( const std::string& m ) { queue->Push( true, m ); } };
      WorkThread thread( [&work, wl]() { work( wl ); } );
      StandardStatus status;
      StatusMonitor monitor;
      monitor.SetCallback( &status );
      monitor.Initialize( "Gaia Depth Grade: " + what );
      // Nothing below may leave this loop while the thread runs (the thread object would be
      // destroyed under it): an exception of the GUI-side work is kept and rethrown afterwards.
      std::exception_ptr guiError;
      bool aborted = false;
      thread.Start();
      while ( !thread.finished.load() )
         try
         {
            thread.Wait( 20 );
            Module->ProcessEvents( true/*excludeUserInputEvents*/ );
            queue->Flush( to );
            if ( !aborted )
               ++monitor;
         }
         catch ( const ProcessAborted& )
         {
            // The console's Abort (StandardStatus throws ProcessAborted): the PCL-free stage cannot
            // be interrupted, so it runs to its end and its result is discarded below.
            aborted = true;
         }
         catch ( ... )
         {
            if ( !guiError )
               guiError = std::current_exception();
         }
      thread.Wait(); // Run() has set `finished`; let it return
      queue->Flush( to );
      if ( thread.error )
         std::rethrow_exception( thread.error );
      if ( guiError )
         std::rethrow_exception( guiError );
      if ( aborted )
         throw ProcessAborted();
      monitor.Complete();
   };
}

// ----------------------------------------------------------------------------

bool GaiaDepthGradeInterface::FailAction( const char* action )
{
   SetBusy( false );
   SetError( MessageOf( std::current_exception() ) );
   GDG_RECORD( action, false );
   return false;
}

bool GaiaDepthGradeInterface::DoPrepare()
{
   const View target = m_targetId.IsEmpty() ? View::Null() : View::ViewById( m_targetId );
   if ( target.IsNull() )
   {
      SetError( m_targetId.IsEmpty() ? String( "no target selected" ) : TargetClosedMessage( m_targetId ) );
      GDG_RECORD( "prepare", false );
      return false;
   }
   MarkPrepareStale();
   const IsoString targetId = m_targetId;
   const GradeOptions options = instance.Options();
   SetBusy( true );
   std::shared_ptr<const PrepareSession> session;
   try
   {
      SetStatus( String::UTF8ToUTF16( "Preparing: solving + StarXTerminator + Gaia match (~50s)\xE2\x80\xA6" ) );
      session = std::make_shared<const PrepareSession>(
         RunPrepare( target, options, GuiLog(), GuiRunner( "Prepare (detection + Gaia cross-match)" ) ) );
      // The settings or the target cannot change while busy; if they did anyway, this session
      // describes neither and is dropped.
      if ( m_targetId != targetId || PrepareOptionsDiffer( options, instance.Options() ) )
         throw Error( "The settings or the target changed while Prepare ran. Prepare again." );
   }
   catch ( ... )
   {
      return FailAction( "prepare" );
   }
   m_session = session;
   gdg::default_inset_center( m_session->width, m_session->height, m_insetCx, m_insetCy );
   instance.SetQA( m_session->prep.qa );
   m_qaLine = S( gdg::qa_summary( m_session->prep.qa ) );
   m_prepared = true;
   SetBusy( false );
   SetStatus( m_qaLine );
   GDG_RECORD( "prepare", true );
   DoPreview();
   return true;
}

bool GaiaDepthGradeInterface::DoPreview()
{
   if ( !m_prepared || !m_session )
   {
      SetError( "Prepare first" );
      GDG_RECORD( "preview", false );
      return false;
   }
   const String stale = m_session->StaleReason(); // Review Focus 4
   if ( !stale.IsEmpty() )
   {
      MarkPrepareStale();
      SetError( stale );
      GDG_RECORD( "preview", false );
      return false;
   }
   SetBusy( true );
   try
   {
      SetStatus( String::UTF8ToUTF16( "Rendering preview\xE2\x80\xA6" ) );
      const gdg::GradeSettings settings = instance.Settings();
      const std::shared_ptr<const PrepareSession> session = m_session; // held for the render
      const gdg::InsetRegion region = gdg::inset_region( session->width, session->height, m_insetCx, m_insetCy, kInsetSize );
      gdg::PreviewPixels px;
      GuiRunner( "Preview" )( [&]( const gdg::Log& wl )
      {
         const gdg::Image comp = gdg::grade_composite( session->stars, session->starless, session->prep, settings, wl );
         px = gdg::preview_pixels( comp, kPreviewWidth, kPreviewHeight, region );
      } );
      m_previewBmp = MakeBitmap( px.full, px.width, px.height, px.channels );
      m_insetBmp = MakeBitmap( px.inset, px.inset_width, px.inset_height, px.channels );
      m_previewStale = false;
   }
   catch ( ... )
   {
      return FailAction( "preview" );
   }
   SetBusy( false );
   GUI->Preview_Control.Update();
   GUI->Inset_Control.Update();
   SetStatus( "Preview updated. Adjust the look, Preview again, or Execute." );
   GDG_RECORD( "preview", true );
   return true;
}

bool GaiaDepthGradeInterface::DoExecute()
{
   if ( !m_prepared || !m_session )
   {
      SetError( "Prepare first" );
      GDG_RECORD( "execute", false );
      return false;
   }
   const String stale = m_session->StaleReason(); // Review Focus 4
   if ( !stale.IsEmpty() )
   {
      MarkPrepareStale();
      SetError( stale );
      GDG_RECORD( "execute", false );
      return false;
   }
   SetBusy( true );
   ImageWindow result;
   try
   {
      SetStatus( String::UTF8ToUTF16( "Executing: full-res render + recombine\xE2\x80\xA6" ) );
      const std::shared_ptr<const PrepareSession> session = m_session; // held for the render
      const gdg::Image comp = RenderComposite( *session, instance.Settings(), GuiRunner( "Execute (full-resolution render)" ) );
      result = CreateOutputWindow( *session, comp );
   }
   catch ( ... )
   {
      return FailAction( "execute" );
   }
   SetBusy( false );
   const String id = result.MainView().Id();
   result.Show();
   SetStatus( "Done: created " + id );
   GDG_RECORD( "execute", true );
   return true;
}

void GaiaDepthGradeInterface::DoBrowse()
{
   OpenFileDialog d;
   d.SetCaption( "Open master" );
   d.LoadImageFilters();
   if ( !d.Execute() || d.FileNames().IsEmpty() )
      return;
   const String path = d.FileNames()[0];
   Array<ImageWindow> w;
   try
   {
      w = ImageWindow::Open( path );
   }
   catch ( ... )
   {
      SetError( "failed to open " + path + ": " + MessageOf( std::current_exception() ) );
      return;
   }
   if ( w.IsEmpty() || w[0].IsNull() )
   {
      SetError( "failed to open " + path );
      return;
   }
   {
      // The new windows reach the view lists through events, and the lists report their own
      // selection changes as they regenerate (a <No View Selected> that is nobody's pick: measured
      // 2026-11-13, it cleared the target just set). Those are absorbed here, then the target set.
      const bool was = m_updating;
      m_updating = true;
      w[0].Show();
      GUI->Target_ViewList.Regenerate( true/*mainViews*/, false/*previews*/ );
      Module->ProcessEvents( true/*excludeUserInputEvents*/ );
      m_updating = was;
   }
   SetTarget( w[0].MainView() );
}

void GaiaDepthGradeInterface::ReadFilesText()
{
   const StringList files = NonEmptyLines( GUI->Files_TextBox.Text() );
   if ( files != instance.p_gaiaDatabaseFiles )
   {
      instance.p_gaiaDatabaseFiles = files;
      MarkPrepareStale();
   }
}

// ----------------------------------------------------------------------------
// Event handlers. Each refuses while busy (review I1) and while UpdateControls writes the
// controls (m_updating).

void GaiaDepthGradeInterface::e_Click( Button& sender, bool )
{
   if ( GUI == nullptr || m_busy )
      return;
   if ( sender == GUI->Prepare_PushButton )
      DoPrepare();
   else if ( sender == GUI->Preview_PushButton )
      DoPreview();
   else if ( sender == GUI->Execute_PushButton )
      DoExecute();
   else if ( sender == GUI->Close_PushButton )
      Hide();
   else if ( sender == GUI->Browse_PushButton )
      DoBrowse();
   else if ( sender == GUI->Index_ToolButton )
   {
      OpenFileDialog d;
      d.SetCaption( "Astrometric index file" );
      d.SetFilter( FileFilter( "Astrometric index files", ".xpsd" ) );
      if ( d.Execute() && !d.FileNames().IsEmpty() )
      {
         instance.p_solveIndexFile = d.FileNames()[0];
         GUI->Index_Edit.SetText( instance.p_solveIndexFile );
         MarkPrepareStale();
      }
   }
   GDG_WRITE_STATE();
}

void GaiaDepthGradeInterface::e_Check( Button& sender, Button::check_state )
{
   if ( GUI == nullptr || m_updating || m_busy )
      return;
   const bool on = static_cast<CheckBox&>( sender ).IsChecked();
   if ( sender == GUI->Nebula_CheckBox )
   {
      instance.p_nebulaEnabled = on;
      MarkPreviewStale();
   }
   else if ( sender == GUI->Offline_CheckBox )
   {
      instance.p_catalog = on ? GDGCatalog::Offline : GDGCatalog::Online;
      MarkPrepareStale();
   }
   else if ( sender == GUI->MagLimit_CheckBox )
   {
      instance.p_magLimitEnabled = on;
      GUI->MagLimit_NumericEdit.Enable( on );
      MarkPrepareStale();
   }
}

void GaiaDepthGradeInterface::e_ValueUpdated( NumericEdit& sender, double v )
{
   if ( GUI == nullptr || m_updating || m_busy )
      return;
   // Live (render) controls: the preview is stale.
   struct Live { NumericEdit* c; double* p; };
   const Live live[] = { { &GUI->Brightness_NumericControl, &instance.p_brightness },
                         { &GUI->Size_NumericControl, &instance.p_size },
                         { &GUI->Contrast_NumericControl, &instance.p_contrast },
                         { &GUI->Saturation_NumericControl, &instance.p_saturation },
                         { &GUI->HaloSigma_NumericControl, &instance.p_haloSigma },
                         { &GUI->PLow_NumericControl, &instance.p_pLow },
                         { &GUI->PHigh_NumericControl, &instance.p_pHigh },
                         { &GUI->NebulaStrength_NumericControl, &instance.p_nebulaStrength },
                         { &GUI->NebulaAtmos_NumericControl, &instance.p_nebulaAtmos },
                         { &GUI->NebulaStructure_NumericControl, &instance.p_nebulaStructure },
                         { &GUI->HaloGain_NumericControl, &instance.p_haloGain },
                         { &GUI->Neutral_NumericEdit, &instance.p_neutralStrength },
                         { &GUI->HaloRadius_NumericEdit, &instance.p_haloRadiusSigma },
                         { &GUI->HaloBeta_NumericEdit, &instance.p_haloMoffatBeta } };
   for ( const Live& l : live )
      if ( sender == *l.c )
      {
         *l.p = v;
         MarkPreviewStale();
         return;
      }
   // Prepare-time controls.
   const Live prep[] = { { &GUI->Detect_NumericEdit, &instance.p_detectSigma },
                         { &GUI->Match_NumericEdit, &instance.p_matchTolerance },
                         { &GUI->DetectFWHM_NumericEdit, &instance.p_detectFWHM },
                         { &GUI->MinMatchRate_NumericEdit, &instance.p_minMatchRate },
                         { &GUI->MagLimit_NumericEdit, &instance.p_magLimit },
                         { &GUI->ParallaxSNR_NumericEdit, &instance.p_parallaxSNR } };
   for ( const Live& l : prep )
      if ( sender == *l.c )
      {
         *l.p = v;
         MarkPrepareStale();
         return;
      }
}

void GaiaDepthGradeInterface::e_SpinValueUpdated( SpinBox& sender, int v )
{
   if ( GUI == nullptr || m_updating || m_busy )
      return;
   if ( sender == GUI->HaloMinRefs_SpinBox )
   {
      instance.p_haloMinRefs = v;
      MarkPreviewStale();
   }
}

void GaiaDepthGradeInterface::e_ItemSelected( ComboBox& sender, int item )
{
   if ( GUI == nullptr || m_updating || m_busy )
      return;
   if ( sender == GUI->Split_ComboBox )
   {
      instance.p_splitMode = item;
      MarkPrepareStale();
   }
   else if ( sender == GUI->Output_ComboBox )
      instance.p_outputMode = item; // process execution only: no staleness
}

void GaiaDepthGradeInterface::e_ViewSelected( ViewList& sender, View& view )
{
   if ( GUI == nullptr || m_updating || m_busy )
      return;
   if ( sender == GUI->Target_ViewList )
   {
      if ( !view.IsNull() )
         SetTarget( view );
      else if ( (GUI->Target_ViewList.IsFocused() || GUI->Target_ViewList.IsUnderMouse()) &&
                (m_targetId.IsEmpty() || !View::ViewById( m_targetId ).IsNull()) )
      {
         // <No View Selected> picked by the user (review M4): no target, nothing to Prepare. Not a
         // pick: the list regenerating as windows come and go (it is then neither focused nor under
         // the pointer), and a target whose window was closed, which keeps its id so that
         // Preview/Execute/Prepare report it closed (Review Focus 4).
         m_targetId.Clear();
         MarkPrepareStale();
         SetStatus( kPickTarget );
      }
   }
   else if ( sender == GUI->Stars_ViewList )
   {
      instance.p_starsViewId = view.IsNull() ? String() : String( view.Id() );
      MarkPrepareStale();
   }
   else if ( sender == GUI->Starless_ViewList )
   {
      instance.p_starlessViewId = view.IsNull() ? String() : String( view.Id() );
      MarkPrepareStale();
   }
}

void GaiaDepthGradeInterface::e_EditCompleted( Edit& sender )
{
   if ( GUI == nullptr || m_updating || m_busy )
      return;
   if ( sender == GUI->Index_Edit )
   {
      const String t = sender.Text().Trimmed();
      if ( t != instance.p_solveIndexFile )
      {
         instance.p_solveIndexFile = t;
         MarkPrepareStale();
      }
   }
}

void GaiaDepthGradeInterface::e_TextUpdated( TextBox& sender, const String& )
{
   if ( GUI == nullptr || m_updating || m_busy )
      return;
   if ( sender == GUI->Files_TextBox )
      ReadFilesText();
}

void GaiaDepthGradeInterface::e_Paint( Control& sender, const pcl::Rect& )
{
   if ( GUI == nullptr )
      return;
   Graphics g( sender );
   g.FillRect( sender.BoundsRect(), Brush( 0xff101010 ) );
   if ( sender == GUI->Preview_Control )
   {
      if ( !m_previewBmp.IsNull() )
         g.DrawBitmap( 0, 0, m_previewBmp );
      if ( m_previewStale )
      {
         g.SetPen( Pen( 0xa0ff8000, 2 ) );
         g.DrawRect( 1, 1, sender.Width() - 1, sender.Height() - 1 );
      }
   }
   else if ( sender == GUI->Inset_Control )
   {
      if ( !m_insetBmp.IsNull() )
         g.DrawBitmap( 0, 0, m_insetBmp );
   }
   g.EndPaint();
}

void GaiaDepthGradeInterface::e_MousePress( Control& sender, const pcl::Point& pos, int, unsigned, unsigned )
{
   if ( GUI == nullptr || m_busy || sender != GUI->Preview_Control )
      return;
   if ( !m_prepared || !m_session || m_previewBmp.IsNull() )
      return;
   // The picture is fitted inside the control (uniform scale, drawn at the origin): a click beside
   // it picks nothing.
   if ( pos.x < 0 || pos.y < 0 || pos.x >= m_previewBmp.Width() || pos.y >= m_previewBmp.Height() )
      return;
   gdg::inset_center_from_click( pos.x, pos.y, m_session->width, m_previewBmp.Width(), m_insetCx, m_insetCy );
   DoPreview();
}

void GaiaDepthGradeInterface::e_ToggleSection( SectionBar&, Control&, bool start )
{
   if ( start )
      SetVariableHeight();
   else
   {
      AdjustToContents();
      SetFixedHeight();
   }
}

// ----------------------------------------------------------------------------

GaiaDepthGradeInterface::GUIData::GUIData( GaiaDepthGradeInterface& w )
{
   const int labelWidth = w.Font().Width( String( "Nebula depth:" ) + 'M' ); // the dialog's minWidth 110
   const int prepLabelWidth = w.Font().Width( String( "detect_threshold_sigma:" ) + 'M' );

   // Target row.
   Target_Label.SetText( "Target:" );
   Target_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   Target_ViewList.Regenerate( true/*mainViews*/, false/*previews*/ );
   Target_ViewList.OnViewSelected( (ViewList::view_event_handler)&GaiaDepthGradeInterface::e_ViewSelected, w );
   Browse_PushButton.SetText( String::UTF8ToUTF16( "Browse\xE2\x80\xA6" ) );
   Browse_PushButton.OnClick( (Button::click_event_handler)&GaiaDepthGradeInterface::e_Click, w );
   Target_Sizer.SetSpacing( 6 );
   Target_Sizer.Add( Target_Label );
   Target_Sizer.Add( Target_ViewList, 100 );
   Target_Sizer.Add( Browse_PushButton );

   // Preview and inset.
   Preview_Control.SetFixedSize( kPreviewWidth, kPreviewHeight );
   Preview_Control.SetToolTip( "Recombined preview (autostretched). Click to move the 1:1 inset." );
   Preview_Control.OnPaint( (Control::paint_event_handler)&GaiaDepthGradeInterface::e_Paint, w );
   Preview_Control.OnMousePress( (Control::mouse_button_event_handler)&GaiaDepthGradeInterface::e_MousePress, w );
   Inset_Control.SetFixedSize( kInsetSize, kInsetSize );
   Inset_Control.SetToolTip( "1:1 zoom of the clicked region." );
   Inset_Control.OnPaint( (Control::paint_event_handler)&GaiaDepthGradeInterface::e_Paint, w );
   Inset_Label.SetText( "1:1 inset" );
   Inset_Sizer.Add( Inset_Label );
   Inset_Sizer.Add( Inset_Control );
   Inset_Sizer.AddStretch();
   Preview_Sizer.SetSpacing( 8 );
   Preview_Sizer.Add( Preview_Control );
   Preview_Sizer.Add( Inset_Sizer );
   Preview_Sizer.AddStretch();

   // Sliders (the dialog's liveSlider: label, the parameter's range and precision; 200 steps).
   const auto slider = [&]( NumericControl& c, const char* label, const GDGReal* p, int sliderSteps = 200,
                            const char* toolTip = nullptr )
   {
      c.label.SetText( label );
      c.label.SetMinWidth( labelWidth );
      c.SetReal();
      c.SetRange( p->MinimumValue(), p->MaximumValue() );
      c.SetPrecision( p->Precision() );
      c.slider.SetRange( 0, sliderSteps );
      c.slider.SetScaledMinWidth( 200 );
      if ( toolTip != nullptr )
         c.SetToolTip( toolTip );
      c.OnValueUpdated( (NumericEdit::value_event_handler)&GaiaDepthGradeInterface::e_ValueUpdated, w );
   };
   slider( Brightness_NumericControl, "Brightness:", TheGDGBrightnessParameter );
   slider( Size_NumericControl, "Size:", TheGDGSizeParameter );
   slider( Contrast_NumericControl, "Contrast:", TheGDGContrastParameter );
   slider( Saturation_NumericControl, "Saturation:", TheGDGSaturationParameter );
   slider( HaloSigma_NumericControl, "Halo sigma:", TheGDGHaloSigmaParameter );
   slider( PLow_NumericControl, "p_low:", TheGDGPLowParameter );
   slider( PHigh_NumericControl, "p_high:", TheGDGPHighParameter );
   Nebula_CheckBox.SetText( "Grade the nebula too (depth from structure, scaled to Gaia)" );
   Nebula_CheckBox.OnCheck( (Button::check_event_handler)&GaiaDepthGradeInterface::e_Check, w );
   slider( NebulaStrength_NumericControl, "Nebula depth:", TheGDGNebulaStrengthParameter );
   slider( NebulaAtmos_NumericControl, "  atmospheric:", TheGDGNebulaAtmosParameter );
   slider( NebulaStructure_NumericControl, "  structure:", TheGDGNebulaStructureParameter );

   Live_Label.SetText( String::UTF8ToUTF16( "Look (live \xE2\x80\x94 Preview to apply)" ) );
   Live_Label.SetStyleSheet( "font-weight: bold;" );
   Live_Sizer.SetSpacing( 4 );
   Live_Sizer.Add( Live_Label );
   Live_Sizer.Add( Brightness_NumericControl );
   Live_Sizer.Add( Size_NumericControl );
   Live_Sizer.Add( Contrast_NumericControl );
   Live_Sizer.Add( Saturation_NumericControl );
   Live_Sizer.Add( HaloSigma_NumericControl );
   Live_Sizer.Add( PLow_NumericControl );
   Live_Sizer.Add( PHigh_NumericControl );
   Live_Sizer.Add( Nebula_CheckBox );
   Live_Sizer.Add( NebulaStrength_NumericControl );
   Live_Sizer.Add( NebulaAtmos_NumericControl );
   Live_Sizer.Add( NebulaStructure_NumericControl );

   // Numeric fields (the dialog's prepEdit, and the Advanced engine fields).
   const auto edit = [&]( NumericEdit& c, const char* label, const GDGReal* p, int width, const char* toolTip = nullptr )
   {
      c.label.SetText( label );
      c.label.SetMinWidth( width );
      c.SetReal();
      c.SetRange( p->MinimumValue(), p->MaximumValue() );
      c.SetPrecision( p->Precision() );
      if ( toolTip != nullptr )
         c.SetToolTip( toolTip );
      c.OnValueUpdated( (NumericEdit::value_event_handler)&GaiaDepthGradeInterface::e_ValueUpdated, w );
   };
   edit( Detect_NumericEdit, "detect_threshold_sigma:", TheGDGDetectSigmaParameter, prepLabelWidth );
   edit( Match_NumericEdit, "match_tolerance_px:", TheGDGMatchToleranceParameter, prepLabelWidth );
   Offline_CheckBox.SetText( String::UTF8ToUTF16( "Offline (local Gaia DR3) \xE2\x80\x94 fast" ) );
   Offline_CheckBox.SetToolTip( "Checked: cone-query the local Gaia DR3 database (instant, offline; parallax distances). "
                                "Unchecked: the online Bailer-Jones catalogue (more sources, but the ESA archive can be slow "
                                "or down)." );
   Offline_CheckBox.OnCheck( (Button::check_event_handler)&GaiaDepthGradeInterface::e_Check, w );
   Prep_Label.SetText( "Matching (changing these needs re-Prepare)" );
   Prep_Label.SetStyleSheet( "font-weight: bold;" );
   Prep_Sizer.SetSpacing( 4 );
   Prep_Sizer.Add( Prep_Label );
   Prep_Sizer.Add( Detect_NumericEdit );
   Prep_Sizer.Add( Match_NumericEdit );
   Prep_Sizer.Add( Offline_CheckBox );
   Prep_Sizer.AddStretch();

   Controls_Sizer.SetSpacing( 18 );
   Controls_Sizer.Add( Live_Sizer, 100 );
   Controls_Sizer.Add( Prep_Sizer, 100 );

   // Advanced: the remaining engine parameters and the split/output/catalogue settings.
   const int advWidth = w.Font().Width( String( "Moffat beta:" ) + 'M' );
   slider( HaloGain_NumericControl, "Halo gain:", TheGDGHaloGainParameter, 400,
           "<p>Overall intensity of the star-shine halo on enlarged stars, independent of Size (the footprint). Live.</p>" );

   edit( DetectFWHM_NumericEdit, "Detect FWHM:", TheGDGDetectFWHMParameter, advWidth,
         "<p>FWHM in pixels of the star detection kernel (DAOFIND). Needs re-Prepare.</p>" );
   edit( MinMatchRate_NumericEdit, "Min match rate:", TheGDGMinMatchRateParameter, advWidth,
         "<p>Below this fraction of detections matched to Gaia, the grade is flagged LOW MATCH (unreliable). "
         "Needs re-Prepare.</p>" );
   edit( Neutral_NumericEdit, "Neutral:", TheGDGNeutralStrengthParameter, advWidth,
         "<p>Depth strength given to stars without a Gaia distance (0 = no change). Live.</p>" );
   Engine1_Sizer.SetSpacing( 12 );
   Engine1_Sizer.Add( DetectFWHM_NumericEdit );
   Engine1_Sizer.Add( MinMatchRate_NumericEdit );
   Engine1_Sizer.Add( Neutral_NumericEdit );
   Engine1_Sizer.AddStretch();

   MagLimit_CheckBox.SetText( "G limit:" );
   MagLimit_CheckBox.SetToolTip( "<p>Drop Gaia sources fainter than this G magnitude (Bailer-Jones distances grow "
                                 "unreliable faintward). Unchecked: no cut, full depth. Needs re-Prepare.</p>" );
   MagLimit_CheckBox.OnCheck( (Button::check_event_handler)&GaiaDepthGradeInterface::e_Check, w );
   edit( MagLimit_NumericEdit, "", TheGDGMagLimitParameter, 0 );
   edit( ParallaxSNR_NumericEdit, "Parallax S/N:", TheGDGParallaxSNRParameter, advWidth,
         "<p>Offline only: Gaia sources whose parallax signal-to-noise is below this are dropped (loudly). "
         "Needs re-Prepare.</p>" );
   Engine2_Sizer.SetSpacing( 12 );
   Engine2_Sizer.Add( MagLimit_CheckBox );
   Engine2_Sizer.Add( MagLimit_NumericEdit );
   Engine2_Sizer.Add( ParallaxSNR_NumericEdit );
   Engine2_Sizer.AddStretch();

   HaloMinRefs_Label.SetText( "Halo refs:" );
   HaloMinRefs_SpinBox.SetRange( int( TheGDGHaloMinRefsParameter->MinimumValue() ), int( TheGDGHaloMinRefsParameter->MaximumValue() ) );
   HaloMinRefs_SpinBox.SetToolTip( "<p>The star-shine halo is measured from at least this many clean reference stars of the "
                                   "frame; with fewer, a Moffat halo is used (with a warning). Live.</p>" );
   HaloMinRefs_SpinBox.OnValueUpdated( (SpinBox::value_event_handler)&GaiaDepthGradeInterface::e_SpinValueUpdated, w );
   edit( HaloRadius_NumericEdit, "Halo radius:", TheGDGHaloRadiusSigmaParameter, advWidth,
         "<p>The halo profile's extent in units of Halo sigma. Live.</p>" );
   edit( HaloBeta_NumericEdit, "Moffat beta:", TheGDGHaloMoffatBetaParameter, advWidth,
         "<p>Slope of the fallback Moffat halo. Live.</p>" );
   Engine3_Sizer.SetSpacing( 12 );
   Engine3_Sizer.Add( HaloMinRefs_Label );
   Engine3_Sizer.Add( HaloMinRefs_SpinBox );
   Engine3_Sizer.Add( HaloRadius_NumericEdit );
   Engine3_Sizer.Add( HaloBeta_NumericEdit );
   Engine3_Sizer.AddStretch();

   Split_Label.SetText( "Star split:" );
   Split_ComboBox.AddItem( "StarXTerminator" );
   Split_ComboBox.AddItem( "Supplied stars and starless views" );
   Split_ComboBox.SetToolTip( "<p>StarXTerminator splits the target (stars + unscreened linear stars layer). Or supply "
                              "your own stars and starless views (StarNet2, or any other tool), with the target's "
                              "geometry. Needs re-Prepare.</p>" );
   Split_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&GaiaDepthGradeInterface::e_ItemSelected, w );
   Split_Sizer.SetSpacing( 6 );
   Split_Sizer.Add( Split_Label );
   Split_Sizer.Add( Split_ComboBox );
   Split_Sizer.AddStretch();

   Stars_Label.SetText( "Stars:" );
   Stars_ViewList.Regenerate( true, false );
   Stars_ViewList.OnViewSelected( (ViewList::view_event_handler)&GaiaDepthGradeInterface::e_ViewSelected, w );
   Starless_Label.SetText( "Starless:" );
   Starless_ViewList.Regenerate( true, false );
   Starless_ViewList.OnViewSelected( (ViewList::view_event_handler)&GaiaDepthGradeInterface::e_ViewSelected, w );
   Supplied_Sizer.SetSpacing( 6 );
   Supplied_Sizer.Add( Stars_Label );
   Supplied_Sizer.Add( Stars_ViewList, 100 );
   Supplied_Sizer.Add( Starless_Label );
   Supplied_Sizer.Add( Starless_ViewList, 100 );

   Output_Label.SetText( "Process output:" );
   Output_ComboBox.AddItem( "New window <id>_depthgraded" );
   Output_ComboBox.AddItem( "Replace the target image" );
   Output_ComboBox.SetToolTip( "<p>What the process does when it is executed on an image (Apply, a process icon, an "
                               "ImageContainer batch, a script): a new window, or the graded image written into the "
                               "target. The Execute button of this window always opens a new window.</p>" );
   Output_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&GaiaDepthGradeInterface::e_ItemSelected, w );
   Output_Sizer.SetSpacing( 6 );
   Output_Sizer.Add( Output_Label );
   Output_Sizer.Add( Output_ComboBox );
   Output_Sizer.AddStretch();

   Index_Label.SetText( "Astrometric index:" );
   Index_Edit.SetToolTip( "<p>The astrometric index file for blind plate solving (images without coordinates). Empty: "
                          "the one selected in the Gaia process preferences.</p>" );
   Index_Edit.OnEditCompleted( (Edit::edit_event_handler)&GaiaDepthGradeInterface::e_EditCompleted, w );
   Index_ToolButton.SetIcon( w.ScaledResource( ":/icons/select-file.png" ) );
   Index_ToolButton.SetScaledFixedSize( 20, 20 );
   Index_ToolButton.SetToolTip( "<p>Select the astrometric index file.</p>" );
   Index_ToolButton.OnClick( (Button::click_event_handler)&GaiaDepthGradeInterface::e_Click, w );
   Index_Sizer.SetSpacing( 6 );
   Index_Sizer.Add( Index_Label );
   Index_Sizer.Add( Index_Edit, 100 );
   Index_Sizer.Add( Index_ToolButton );

   Files_Label.SetText( "Gaia DR3 files:" );
   Files_TextBox.SetScaledMinHeight( 48 );
   Files_TextBox.SetToolTip( "<p>The Gaia DR3 database files (.xpsd), one per line. Empty: the files configured for "
                             "PixInsight's Gaia process (DR3, then DR3SP). Needs re-Prepare.</p>" );
   Files_TextBox.OnTextUpdated( (TextBox::unicode_event_handler)&GaiaDepthGradeInterface::e_TextUpdated, w );
   Files_Sizer.SetSpacing( 6 );
   Files_Sizer.Add( Files_Label );
   Files_Sizer.Add( Files_TextBox, 100 );

   Advanced_Sizer.SetSpacing( 4 );
   Advanced_Sizer.Add( HaloGain_NumericControl );
   Advanced_Sizer.Add( Engine1_Sizer );
   Advanced_Sizer.Add( Engine2_Sizer );
   Advanced_Sizer.Add( Engine3_Sizer );
   Advanced_Sizer.Add( Split_Sizer );
   Advanced_Sizer.Add( Supplied_Sizer );
   Advanced_Sizer.Add( Output_Sizer );
   Advanced_Sizer.Add( Index_Sizer );
   Advanced_Sizer.Add( Files_Sizer );
   Advanced_Control.SetSizer( Advanced_Sizer );
   Advanced_SectionBar.SetTitle( "Advanced" );
   Advanced_SectionBar.SetSection( Advanced_Control );
   Advanced_SectionBar.OnToggleSection( (SectionBar::section_event_handler)&GaiaDepthGradeInterface::e_ToggleSection, w );

   // Buttons and status.
   Prepare_PushButton.SetText( "Prepare" );
   Prepare_PushButton.SetToolTip( "Solve + StarXTerminator + Gaia match. Run once per target (dense fields: the first Gaia "
                                  "query can take a few minutes, then it's cached)." );
   Prepare_PushButton.OnClick( (Button::click_event_handler)&GaiaDepthGradeInterface::e_Click, w );
   Preview_PushButton.SetText( "Preview / Update" );
   Preview_PushButton.OnClick( (Button::click_event_handler)&GaiaDepthGradeInterface::e_Click, w );
   Execute_PushButton.SetText( "Execute" );
   Execute_PushButton.OnClick( (Button::click_event_handler)&GaiaDepthGradeInterface::e_Click, w );
   Close_PushButton.SetText( "Close" );
   Close_PushButton.OnClick( (Button::click_event_handler)&GaiaDepthGradeInterface::e_Click, w );
   Buttons_Sizer.SetSpacing( 8 );
   Buttons_Sizer.Add( Prepare_PushButton );
   Buttons_Sizer.Add( Preview_PushButton );
   Buttons_Sizer.AddStretch();
   Buttons_Sizer.Add( Execute_PushButton );
   Buttons_Sizer.Add( Close_PushButton );

   Status_Label.SetText( kPickTarget );
   Status_Label.SetStyle( FrameStyle::Box );
   Status_Label.SetTextAlignment( TextAlign::Left | TextAlign::VertCenter );
   Status_Label.SetMinHeight( 28 );
   QA_Label.SetTextAlignment( TextAlign::Left | TextAlign::VertCenter );
   QA_Label.SetMinHeight( 20 );
   QA_Label.SetToolTip( "<p>The last Prepare's match: detections matched to Gaia, the match rate and the median offset. "
                        "LOW MATCH: below the minimum match rate, the depth grade is unreliable.</p>" );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 8 );
   Global_Sizer.Add( Target_Sizer );
   Global_Sizer.Add( Preview_Sizer );
   Global_Sizer.Add( Controls_Sizer );
   Global_Sizer.Add( Advanced_SectionBar );
   Global_Sizer.Add( Advanced_Control );
   Global_Sizer.Add( Buttons_Sizer );
   Global_Sizer.Add( Status_Label );
   Global_Sizer.Add( QA_Label );

   Advanced_Control.Hide();

   w.SetSizer( Global_Sizer );
   w.SetMinWidth( 1040 ); // this.minWidth
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   w.SetFixedHeight();

#ifdef GDG_PROBES
   State_Timer.SetInterval( 0.5 );
   State_Timer.SetPeriodic( true );
   State_Timer.OnTimer( (Timer::timer_event_handler)&GaiaDepthGradeInterface::e_Timer, w );
   State_Timer.Start();
#endif
}

} // namespace pcl
