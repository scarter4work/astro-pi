// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryStackInterface.h"
#include "PlanetaryStackParameters.h"
#include "PlanetaryStackProcess.h"

#include <pcl/Brush.h>
#include <pcl/Console.h>
#include <pcl/File.h>
#include <pcl/Graphics.h>
#include <pcl/Math.h>
#include <pcl/MessageBox.h>
#include <pcl/Pen.h>

#include <chrono>
#include <cstdio>
#include <exception>

#include "SerInfoText.h"
#include "StackCards.h"
#include "VideoControls.h"

namespace ser = planetary::ser;
namespace stack = planetary::stack;
namespace stackcards = planetary::stackcards;

namespace pcl
{

PlanetaryStackInterface* ThePlanetaryStackInterface = nullptr;

namespace
{

const char* const kIdleText = "No video loaded.\nClick Open... to choose a .ser or .avi video, then Stack!.";
const char* const kNoImageNote = "No image -- the diagnosis is below.";

// The progress bar's fill. The GUI smoke (test/gui_drive.py FILL) finds a
// running stack by exactly this colour.
const RGBA kProgressFill = RGBAColor( 58, 110, 165 );

// Run::progress: the phase (1-3, the engine's in Engine.h's order; 4 the
// mineral moon's; 0 before the first report) and its done / total counts, in
// one atomic so they always agree: phase << 60 | total << 30 | done (each count
// at most 2^30 - 1).
struct ProgressSnapshot
{
   int phase = 0;
   int64_t done = 0, total = 0;
};

const char* const kPhases[] = { "Starting", stack::kPhaseMeasuring, stack::kPhaseAligning, stack::kPhaseStacking,
                                stack::kPhaseMineral };
const int kMineralPhase = 4;

uint64_t PackProgress( const std::string& phase, int64_t done, int64_t total )
{
   uint64_t index = 0;
   for ( int i = 1; i <= kMineralPhase; ++i )
      if ( phase == kPhases[i] )
         index = uint64_t( i );
   const int64_t limit = (int64_t( 1 ) << 30) - 1;
   const uint64_t t = uint64_t( Range<int64_t>( total, 0, limit ) );
   const uint64_t d = uint64_t( Range<int64_t>( done, 0, int64_t( t ) ) );
   return index << 60 | t << 30 | d;
}

ProgressSnapshot UnpackProgress( uint64_t packed )
{
   const uint64_t mask = (uint64_t( 1 ) << 30) - 1;
   return { int( packed >> 60 ), int64_t( packed & mask ), int64_t( (packed >> 30) & mask ) };
}

// The bar's fill: the whole run, each of its `steps` an equal share, so it only
// grows (a bar that restarts at every phase reads as a run that ended; the text
// names the step and how far into it). A Mineral run (1 step) is its one phase.
double OverallFraction( const ProgressSnapshot& p, int steps )
{
   if ( p.phase < 1 )
      return 0.0;
   const int before = steps <= 1 ? 0 : p.phase - 1;
   return (before + stackcards::ProgressFraction( p.done, p.total )) / Max( 1, steps );
}

const char* PhaseName( int index )
{
   return kPhases[index >= 0 && index <= kMineralPhase ? index : 0];
}

// The Target combo's items (index == PSTarget value); Auto's text names what
// the file name gives (UpdateControls).
const char* const kTargetItems[] = { "Auto", "Other", "Moon", "Mercury", "Venus", "Mars", "Jupiter", "Saturn", "Uranus",
                                     "Neptune" };

const char* const kMineralTip =
   "<p>Also make the mineral moon: the stack colour-calibrated, stretched for display and sharpened, with the "
   "Moon's faint colour differences (the maria's titanium blues, the highlands' iron browns) amplified by Strength. "
   "The preview shows it; Execute opens it as a second window (&lt;name&gt;_mineral) beside the linear stack. "
   "Checking or unchecking it never stacks again. Needs the Moon, a colour capture and a broadband filter.</p>";

bool SameParams( const stack::MineralParams& a, const stack::MineralParams& b )
{
   return a.strength == b.strength && a.chromaBlurSigma == b.chromaBlurSigma;
}

} // namespace

// ----------------------------------------------------------------------------

StackProgressBar::StackProgressBar( Control& parent )
   : Control( parent )
{
   SetScaledMinHeight( 22 );
   OnPaint( (Control::paint_event_handler)&StackProgressBar::e_Paint, *this );
}

void StackProgressBar::SetRunning( double fraction, const String& text )
{
   m_running = true;
   m_fraction = Range( fraction, 0.0, 1.0 );
   m_text = text;
   Update();
}

void StackProgressBar::SetIdle( const String& text )
{
   m_running = false;
   m_fraction = 0;
   m_text = text;
   Update();
}

void StackProgressBar::e_Paint( Control& sender, const pcl::Rect& /*updateRect*/ )
{
   GuardedCall( [this]()
   {
      const pcl::Rect r = BoundsRect();
      Graphics G( *this );
      G.FillRect( r, Brush( RGBAColor( 255, 255, 255 ) ) );
      const pcl::Rect inner( r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1 );
      const int fill = m_running ? RoundInt( m_fraction * inner.Width() ) : 0;
      const pcl::Rect done( inner.x0, inner.y0, inner.x0 + fill, inner.y1 );
      if ( fill > 0 )
         G.FillRect( done, Brush( kProgressFill ) );
      G.SetPen( Pen( RGBAColor( 128, 128, 128 ) ) );
      G.DrawRect( r.x0, r.y0, r.x1 - 1, r.y1 - 1 );
      if ( m_text.IsEmpty() )
         return;
      // Dark text on the empty part, white on the filled part.
      G.SetClipRect( inner.x0 + fill, inner.y0, inner.x1, inner.y1 );
      G.SetPen( Pen( RGBAColor( 0, 0, 0 ) ) );
      G.DrawTextRect( inner, m_text, TextAlign::Center|TextAlign::VertCenter );
      if ( fill > 0 )
      {
         G.SetClipRect( done );
         G.SetPen( Pen( RGBAColor( 255, 255, 255 ) ) );
         G.DrawTextRect( inner, m_text, TextAlign::Center|TextAlign::VertCenter );
      }
   },
   []( const String& message )
   {
      Console().CriticalLn( "<end><cbr>*** PlanetaryStack progress bar: " + message.EncodedHTMLSpecialChars() );
   } );
}

// ----------------------------------------------------------------------------

PlanetaryStackInterface::PlanetaryStackInterface()
   : instance( ThePlanetaryStackProcess )
{
   ThePlanetaryStackInterface = this;
}

PlanetaryStackInterface::~PlanetaryStackInterface()
{
   // Never destroy a joinable std::thread (std::terminate); no control is touched here.
   JoinStack();
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

IsoString PlanetaryStackInterface::Id() const
{
   return "PlanetaryStack";
}

MetaProcess* PlanetaryStackInterface::Process() const
{
   return ThePlanetaryStackProcess;
}

InterfaceFeatures PlanetaryStackInterface::Features() const
{
   return InterfaceFeature::DefaultGlobal;
}

void PlanetaryStackInterface::ApplyInstanceGlobal() const
{
   // Apply Global is Execute: the stack shown, opened without stacking again.
   // (The core calls this const; the window's state is what it acts on.)
   PlanetaryStackInterface* self = const_cast<PlanetaryStackInterface*>( this );
   self->Guarded( [self]() { self->ExecuteStack(); }, true/*interactive*/ );
}

void PlanetaryStackInterface::ResetInstance()
{
   PlanetaryStackInstance defaultInstance( ThePlanetaryStackProcess );
   ImportProcess( defaultInstance );
}

bool PlanetaryStackInterface::Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& )
{
   if ( GUI == nullptr )
      // Nothing may cross into the core: a window that cannot be built is said on
      // the console and not launched. All or nothing: GUI is set only once every
      // step has run, so a failure leaves no half-made window behind.
      GuardedCall( [this]()
      {
         std::unique_ptr<GUIData> gui = std::make_unique<GUIData>( *this );
         SetWindowTitle( "PlanetaryStack" );
         OnShow( (Control::event_handler)&PlanetaryStackInterface::e_Show, *this );
         OnHide( (Control::event_handler)&PlanetaryStackInterface::e_Hide, *this );
         GUI = gui.get();
         try
         {
            ShowText();
            UpdateControls();
         }
         catch ( ... )
         {
            GUI = nullptr;
            throw;
         }
         gui.release();
      },
      []( const String& message )
      {
         Console().CriticalLn( "<end><cbr>*** PlanetaryStack: cannot open the window: " + message.EncodedHTMLSpecialChars() );
      } );

   dynamic = false;
   return GUI != nullptr;
}

ProcessImplementation* PlanetaryStackInterface::NewProcess() const
{
   return new PlanetaryStackInstance( instance );
}

bool PlanetaryStackInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const PlanetaryStackInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Must be an instance of PlanetaryStack.";
   return false;
}

bool PlanetaryStackInterface::RequiresInstanceValidation() const
{
   return true;
}

bool PlanetaryStackInterface::ImportProcess( const ProcessImplementation& p )
{
   if ( GUI == nullptr )
   {
      instance.Assign( p );
      return true;
   }
   Guarded( [this, &p]()
   {
      // A run of the old file and pattern means nothing for the new ones.
      StopStack( "a process instance was imported" );
      instance.Assign( p );
      // An out-of-range value is reported by ReadInfo (BayerChoiceFor).
      if ( instance.p_bayerPattern >= 0 && instance.p_bayerPattern < PSBayerPattern::NumberOfItems )
         GUI->Bayer_ComboBox.SetCurrentItem( instance.p_bayerPattern );
      // The target, the check box and the sliders follow the instance (UpdateControls);
      // an out-of-range target is reported by ReadInfo (TargetFor).
      m_mineralOff.clear();
      // A hidden interface holds nothing: e_Show reads the file when it appears.
      if ( IsVisible() )
         LoadFromInstance();
      else
         ClearAll();
   }, false/*interactive*/ );
   return true;
}

template <class F>
void PlanetaryStackInterface::Guarded( F&& f, bool interactive )
{
   GuardedCall( f, [this, interactive]( const String& message ) { Fail( message, interactive ); } );
}

void PlanetaryStackInterface::Fail( const String& message, bool interactive )
{
   // Each step on its own, so a failure in one never skips the others; a step
   // that fails is noted on stderr (nothing may cross into the core).
   auto attempt = [&message]( const char* step, auto&& f )
   {
      try
      {
         f();
      }
      catch ( ... )
      {
         std::fprintf( stderr, "PlanetaryStack: %s failed while reporting: %s\n", step, ToUtf8( message ).c_str() );
      }
   };
   attempt( "stopping the stack", [this]() { StopStack( "of an error" ); } );
   // A finished result of the file and pattern shown stays (final review, module
   // m4): a failure to open its window, or any other step after it, is not about
   // the stack, and dropping it would cost a stack of minutes to redo.
   attempt( "clearing the window", [this]()
   {
      if ( !HoldsResultForInstance() )
         ClearAll();
   } );
   attempt( "showing the error", [this, &message]()
   {
      m_error = ToUtf8( message );
      ShowText();
      UpdateControls();
   } );
   attempt( "writing the console", [&message]()
   {
      Console().CriticalLn( "<end><cbr>*** PlanetaryStack: " + message.EncodedHTMLSpecialChars() );
   } );
   if ( interactive )
      attempt( "showing a message box", [&message]()
      {
         MessageBox( message, "PlanetaryStack", StdIcon::Error, StdButton::Ok ).Execute();
      } );
}

void PlanetaryStackInterface::LoadFromInstance()
{
   ClearAll();
   if ( !instance.p_filePath.Trimmed().IsEmpty() )
      ReadInfo( false/*interactive*/ );
}

bool PlanetaryStackInterface::HoldsResultForInstance() const
{
   return m_outcome && m_outcomePath == instance.p_filePath.Trimmed() && m_outcomePattern == instance.p_bayerPattern;
}

bool PlanetaryStackInterface::ReadInfo( bool interactive )
{
   const String path = instance.p_filePath.Trimmed();
   const String fileName = File::ExtractNameAndExtension( path );
   m_error.clear();
   ser::FileInfo info;
   try
   {
      TargetFor( instance.p_target, "PlanetaryStack" );   // an imported out-of-range value fails here, loudly
      // OpenVideo's info: an AVI's Bayer pattern is resolved by then (Mineral moon's
      // colour check needs it; B1 review m2).
      info = ser::OpenVideo( ToUtf8( path ), BayerChoiceFor( instance.p_bayerPattern, "PlanetaryStack" ) )->Info();
   }
   catch ( const ser::SerError& e )
   {
      // The result of this file and pattern stays, with the last info read: the
      // file cannot be read now, which says nothing about the stack made from it.
      if ( !HoldsResultForInstance() )
         ClearAll();
      const String message = fileName + ": " + FromUtf8( e.what() ) + "\nPath: " + path;
      m_error = ToUtf8( message );
      ShowText();
      UpdateControls();
      if ( interactive )
         MessageBox( message, "PlanetaryStack", StdIcon::Error, StdButton::Ok ).Execute();
      return false;
   }
   // A stack stays only while it is what Stack! would make now: the same file
   // and the same capture (stackcards::SameCapture: size, depth, frames, the
   // effective pattern and the raw-mosaic standing). When it goes for the same
   // file, the window says why, so an empty preview is never a mystery.
   if ( m_outcome && (m_outcomePath != path || !stackcards::SameCapture( m_outcome->info, info )) )
   {
      const bool sameFile = m_outcomePath == path;
      const bool patternChanged = m_outcome->info.colorId != info.colorId ||
                                  stack::UndeclaredMosaic( m_outcome->info ) != stack::UndeclaredMosaic( info );
      DropResult();
      if ( sameFile && GUI != nullptr )
      {
         const String as = ser::IsBayer( info.colorId ) ? String( ser::BayerPatternKeyword( info.colorId ) )
                                                        : String( "mono (raw CFA)" );
         m_status = patternChanged ? "Bayer pattern changed: click Stack! to stack as " + as + "."
                                   : String( "The video changed on disk: click Stack! to stack it again." );
         GUI->Preview_Control.Clear( m_status );
         GUI->Progress_Bar.SetIdle( m_status );
      }
   }
   // A result that stays is what Stack! would make with the pattern now chosen
   // (Save pattern...'s Auto names the same pattern through the new sidecar).
   if ( m_outcome )
      m_outcomePattern = instance.p_bayerPattern;
   m_info = info;
   m_infoLine = stackcards::StackInfoLine( ToUtf8( fileName ), info, ToUtf8( path ) );
   m_warnings.clear();
   for ( const std::string& w : info.warnings )
      m_warnings += "\nWarning: " + planetary::preview::StackWindowWarning( w );
   ApplyMineralRefusal();
   ShowText();
   UpdateControls();
   return true;
}

TargetChoice PlanetaryStackInterface::Target() const
{
   return stackcards::ResolveTarget( TargetFor( instance.p_target, "PlanetaryStack" ), ToUtf8( instance.p_filePath.Trimmed() ) );
}

std::optional<std::string> PlanetaryStackInterface::MineralRefusal() const
{
   if ( !m_info )
      return std::string( "Mineral moon: open a Moon capture first." );
   return stackcards::MineralRefusalFor( ToUtf8( instance.p_filePath.Trimmed() ), *m_info, Target() );
}

void PlanetaryStackInterface::ApplyMineralRefusal()
{
   if ( !instance.p_mineralMoon || !m_info )
      return;
   if ( const std::optional<std::string> why = MineralRefusal() )
   {
      instance.p_mineralMoon = false;
      m_mineralOff = *why;
      ShowPreview();
   }
}

stack::MineralParams PlanetaryStackInterface::CurrentParams() const
{
   return MineralParamsFor( instance.p_mineralStrength, instance.p_chromaBlur, "PlanetaryStack" );
}

void PlanetaryStackInterface::ShowPreview()
{
   if ( GUI == nullptr || !m_outcome || !m_outcome->image )
      return;
   if ( instance.p_mineralMoon && m_mineralShown )
      GUI->Preview_Control.ReplaceBitmap( MakeDisplayPreviewBitmap( m_mineralShown->image ) );
   else
      GUI->Preview_Control.ReplaceBitmap( MakeStackPreviewBitmap( *m_outcome->image ) );
}

stack::MineralOutcome PlanetaryStackInterface::CurrentMineral() const
{
   const stack::MineralParams params = CurrentParams();
   if ( m_mineralShown && SameParams( params, m_shownParams ) )
      return *m_mineralShown;
   if ( !m_mineral )
      throw Error( "PlanetaryStack: internal error - no mineral moon data to colour" );
   stack::StepRecord record;
   const auto t0 = std::chrono::steady_clock::now();
   ser::FloatImage image = stack::ApplyMineralColour( *m_mineral, params, StackThreads(), &record );
   return MineralOutcomeOf( *m_mineral, std::move( image ), record,
                            std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
}

void PlanetaryStackInterface::OpenWindows()
{
   // Both or none (B8 review M2): the mineral moon at the current parameters (possibly coloured now) and its
   // window's contents are made before the stack window is shown.
   const TargetChoice target = Target();
   std::optional<stack::MineralOutcome> mineral;
   std::optional<MineralWindow> mineralWindow;
   if ( instance.p_mineralMoon && m_mineral )
   {
      mineral = CurrentMineral();
      mineralWindow = MakeMineralWindow( m_outcomePath, *m_outcome, m_times, target, *mineral );
   }
   OpenStackWindow( m_outcomePath, *m_outcome, m_times, target );
   if ( mineralWindow )
      ShowMineralWindow( m_outcomePath, *mineralWindow, *mineral );
}

void PlanetaryStackInterface::RequestColour()
{
   if ( !m_mineral || GUI == nullptr )
      return;
   if ( m_colour )
   {
      m_colourPending = true;
      return;
   }
   std::unique_ptr<ColourRun> run = std::make_unique<ColourRun>();
   run->cache = m_mineral;
   run->params = CurrentParams();
   run->threads = StackThreads();
   // PCL-free: ApplyMineralColour on the cache the run holds; then `finished`.
   ColourRun* r = run.get();
   m_colour = std::move( run );
   m_colourPending = false;
   try
   {
      m_colourWorker = std::thread( [r]()
      {
         try
         {
            stack::StepRecord record;
            const auto t0 = std::chrono::steady_clock::now();
            ser::FloatImage image = stack::ApplyMineralColour( *r->cache, r->params, r->threads, &record );
            r->mineral = MineralOutcomeOf( *r->cache, std::move( image ), record,
                                           std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
         }
         catch ( ... )
         {
            r->error = planetary::preview::DescribeException( std::current_exception() );
         }
         r->finished.store( true, std::memory_order_release );
      } );
   }
   catch ( ... )
   {
      m_colour.reset();
      throw;
   }
   GUI->Progress_Timer.Start();
}

void PlanetaryStackInterface::PollColour()
{
   if ( !m_colour || !m_colour->finished.load( std::memory_order_acquire ) )
      return;
   m_colourWorker.join();
   std::unique_ptr<ColourRun> run = std::move( m_colour );
   // A result for a stack since dropped or replaced is nobody's, and so is a request queued behind it: the
   // new stack's mineral moon is coloured at the parameters current then (B8 review M4).
   if ( run->cache != m_mineral )
   {
      m_colourPending = false;
      return;
   }
   if ( !run->error.empty() )
   {
      m_error = "PlanetaryStack: mineral moon: " + run->error;
      Console().CriticalLn( "<end><cbr>*** " + FromUtf8( m_error ).EncodedHTMLSpecialChars() );
   }
   else
   {
      m_mineralShown = std::move( run->mineral );
      m_shownParams = run->params;
      ShowPreview();
   }
   ShowText();
   UpdateControls();
   // The newest values, if they moved while this one ran.
   if ( m_colourPending )
   {
      m_colourPending = false;
      if ( !m_mineralShown || !SameParams( CurrentParams(), m_shownParams ) )
         RequestColour();
   }
}

void PlanetaryStackInterface::JoinColour()
{
   if ( m_colourWorker.joinable() )
      m_colourWorker.join();
   m_colour.reset();
   m_colourPending = false;
}

void PlanetaryStackInterface::DropResult()
{
   m_outcome.reset();
   m_outcomePath.Clear();
   m_outcomePattern = 0;
   m_times = StackTimes();
   m_status.Clear();
   m_mineral.reset();
   m_mineralShown.reset();
   m_mineralError.clear();
   if ( GUI != nullptr )
   {
      GUI->Preview_Control.Clear();
      GUI->Progress_Bar.SetIdle( m_status );
   }
}

void PlanetaryStackInterface::ClearAll()
{
   DropResult();
   m_info.reset();
   m_infoLine.clear();
   m_warnings.clear();
   m_error.clear();
   m_mineralOff.clear();
   if ( GUI != nullptr )
   {
      ShowText();
      UpdateControls();
   }
}

void PlanetaryStackInterface::ShowText()
{
   if ( GUI == nullptr )
      return;
   std::string t = m_info ? m_infoLine : std::string( kIdleText );
   if ( m_outcome )
      t += "\n\n" + stackcards::ResultText( *m_outcome );
   else
      t += m_warnings;
   if ( m_outcome && instance.p_mineralMoon && m_mineralShown )
      t += "\n" + stackcards::MineralResultText( *m_mineralShown );
   if ( !m_mineralError.empty() )
      t += "\nMineral moon failed: " + m_mineralError;
   if ( !m_mineralOff.empty() )
      t += "\nMineral moon turned off: " + m_mineralOff;
   if ( !m_error.empty() )
      t += "\n\nError: " + m_error;
   GUI->Info_TextBox.SetPlainText( FromUtf8( planetary::preview::RawSafe( t ) ) );
   // From the top: the file line and the first warning are what matters most
   // (the box leaves its caret, and so its view, at the end of the text).
   GUI->Info_TextBox.Home();
}

void PlanetaryStackInterface::UpdateControls()
{
   if ( GUI == nullptr )
      return;
   const bool running = Running();
   const bool loaded = bool( m_info );
   const bool avi = loaded && stackcards::IsAvi( *m_info );
   const bool image = m_outcome && m_outcome->image;
   GUI->Open_PushButton.Enable( !running );
   GUI->Bayer_Label.Enable( !running && avi );
   GUI->Bayer_ComboBox.Enable( !running && avi );
   // Save pattern... only where there is no sidecar to read (never one to
   // overwrite), and only once a pattern is chosen (Auto and None have none to
   // save) and its stack is shown: the stack's colours are how you tell the
   // pattern is right.
   const bool noSidecar = loaded && AviWithoutSidecar( *m_info );
   GUI->SavePattern_PushButton.SetVisible( noSidecar );
   GUI->SavePattern_PushButton.Enable( noSidecar && !running && image && ser::IsBayer( m_info->colorId ) &&
                                       instance.p_bayerPattern >= PSBayerPattern::RGGB &&
                                       instance.p_bayerPattern <= PSBayerPattern::BGGR );
   GUI->Stack_PushButton.Enable( !running && loaded );
   GUI->Cancel_PushButton.Enable( running && !m_run->cancel.load() );
   // Execute opens the stack shown, or stacks first when none is; a diagnosis
   // has nothing to open.
   GUI->Execute_PushButton.Enable( !running && loaded && !(m_outcome && !m_outcome->image) );

   // Target: Auto's item names the file name's target.
   std::string autoText = kTargetItems[PSTarget::Auto];
   if ( loaded )
   {
      const TargetChoice fromName = stackcards::ResolveTarget( std::nullopt, ToUtf8( instance.p_filePath.Trimmed() ) );
      autoText = fromName.inFileName ? std::string( ser::TargetName( fromName.target ) ) + " (from file name)"
                                     : std::string( "Auto: not in file name" );
   }
   GUI->Target_ComboBox.SetItemText( PSTarget::Auto, FromUtf8( autoText ) );
   if ( instance.p_target >= 0 && instance.p_target < PSTarget::NumberOfItems &&
        GUI->Target_ComboBox.CurrentItem() != instance.p_target )
      GUI->Target_ComboBox.SetCurrentItem( instance.p_target );
   GUI->Target_Label.Enable( !running );
   GUI->Target_ComboBox.Enable( !running );
   // Mineral moon: only for a broadband colour Moon; the tooltip says why not.
   std::optional<std::string> refusal;
   try
   {
      refusal = MineralRefusal();
   }
   catch ( const Error& e )   // an out-of-range target (ReadInfo reports it)
   {
      refusal = ToUtf8( e.Message() );
   }
   GUI->Mineral_CheckBox.SetChecked( instance.p_mineralMoon );
   GUI->Mineral_CheckBox.Enable( !running && !refusal );
   GUI->Mineral_CheckBox.SetToolTip( refusal ? "<p>" + FromUtf8( *refusal ).EncodedHTMLSpecialChars() + "</p>"
                                             : String( kMineralTip ) );
   GUI->Strength_NumericControl.SetValue( instance.p_mineralStrength );
   GUI->ChromaBlur_NumericControl.SetValue( instance.p_chromaBlur );
   GUI->Strength_NumericControl.Enable( !running && instance.p_mineralMoon );
   GUI->ChromaBlur_NumericControl.Enable( !running && instance.p_mineralMoon );

   const bool zoom = GUI->Preview_Control.HasBitmap();
   GUI->Fit_PushButton.Enable( zoom );
   GUI->Zoom100_PushButton.Enable( zoom );
   GUI->Zoom200_PushButton.Enable( zoom );
   GUI->ZoomOut_PushButton.Enable( zoom );
   GUI->ZoomIn_PushButton.Enable( zoom );
   GUI->Zoom_Label.SetText( !zoom ? String() : GUI->Preview_Control.IsFit() ? String( "Zoom: Fit" )
                                  : String().Format( "Zoom: %.0f%%", 100 * GUI->Preview_Control.ShownZoom() ) );
}

void PlanetaryStackInterface::StartStack( bool openWhenDone )
{
   if ( Running() || !m_info )
      return;
   const String path = instance.p_filePath.Trimmed();
   const TargetChoice target = Target();
   std::optional<stack::MineralParams> params;
   if ( instance.p_mineralMoon )
   {
      if ( const std::optional<std::string> why = MineralRefusal() )
         throw Error( "PlanetaryStack: " + File::ExtractNameAndExtension( path ) + ": " + FromUtf8( *why ) );
      params = CurrentParams();
   }
   DropResult();
   m_error.clear();

   std::unique_ptr<Run> run = std::make_unique<Run>();
   run->job = Job::Stack;
   run->path = ToUtf8( path );
   run->bayer = BayerChoiceFor( instance.p_bayerPattern, "PlanetaryStack" );
   run->pattern = instance.p_bayerPattern;
   run->threads = StackThreads();
   run->frames = m_info->frameCount;
   run->mineral = bool( params );
   run->steps = params ? 4 : 3;
   if ( params )
      run->params = *params;
   WriteStackContext( path, *m_info, run->threads, target, params ? &*params : nullptr );
   m_run = std::move( run );
   StartWorker( openWhenDone );
}

void PlanetaryStackInterface::StartMineral( bool openWhenDone )
{
   if ( Running() || !m_outcome || !m_outcome->image )
      return;
   std::unique_ptr<Run> run = std::make_unique<Run>();
   run->job = Job::Mineral;
   run->path = ToUtf8( m_outcomePath );
   run->pattern = m_outcomePattern;
   run->threads = StackThreads();
   run->steps = 1;
   run->mineral = true;
   run->params = CurrentParams();
   run->input = *m_outcome->image;
   m_mineralError.clear();
   Console().WriteLn( "<end><cbr>PlanetaryStack: the mineral moon of " +
                      File::ExtractNameAndExtension( m_outcomePath ).EncodedHTMLSpecialChars() + ": strength " +
                      String( stackcards::Fixed( run->params.strength, 2 ).c_str() ) + ", chroma blur " +
                      String( stackcards::Fixed( run->params.chromaBlurSigma, 2 ).c_str() ) + " px" );
   m_run = std::move( run );
   StartWorker( openWhenDone );
}

void PlanetaryStackInterface::StartWorker( bool openWhenDone )
{
   // The worker: a Stack run's RunStack and, for a SER's DATE-OBS, the frame
   // times (CaptureTimes, PCL-free) through a reader of its own, then (with
   // `mineral`) PrepareMineral and the colour step; a Mineral run's
   // PrepareMineral and colour step on its copy of the stack. It writes only the
   // run's atomics and, before `finished`, its results; it never touches a PCL
   // object. The engine's report() and cancelled() run on this thread.
   Run* r = m_run.get();
   m_openWhenDone = openWhenDone;
   try
   {
      m_worker = std::thread( [r]()
      {
         stack::Progress progress;
         progress.report = [r]( const std::string& phase, int64_t done, int64_t total )
         {
            r->progress.store( PackProgress( phase, done, total ), std::memory_order_relaxed );
         };
         progress.cancelled = [r]() { return r->cancel.load( std::memory_order_relaxed ); };
         try
         {
            const ser::FloatImage* stacked = nullptr;
            if ( r->job == Job::Stack )
            {
               stack::StackOutcome outcome = stack::RunStack( r->path, r->bayer, r->threads, progress );
               if ( outcome.image && !stackcards::IsAvi( outcome.info ) )
                  r->times = CaptureTimes( *ser::OpenVideo( r->path, r->bayer ), outcome );
               r->outcome = std::move( outcome );
               if ( r->mineral && r->outcome->image )
                  stacked = &*r->outcome->image;
            }
            else
               stacked = &*r->input;
            if ( stacked != nullptr )
            {
               // A stack the mineral moon refuses (NaN samples) stands: the reason is shown.
               try
               {
                  std::shared_ptr<stack::MineralCache> cache =
                     std::make_shared<stack::MineralCache>( stack::PrepareMineral( *stacked, r->threads, progress ) );
                  stack::StepRecord record;
                  const auto t0 = std::chrono::steady_clock::now();
                  ser::FloatImage image = stack::ApplyMineralColour( *cache, r->params, r->threads, &record );
                  r->mineralMade = MineralOutcomeOf( *cache, std::move( image ), record,
                                                     std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
                  r->cache = std::move( cache );
                  progress.report( stack::kPhaseMineral, 1, 1 );
               }
               catch ( const stack::StackCancelled& )
               {
                  throw;
               }
               catch ( ... )
               {
                  r->mineralError = planetary::preview::DescribeException( std::current_exception() );
               }
            }
         }
         catch ( const stack::StackCancelled& )
         {
            r->cancelled = true;
         }
         catch ( ... )
         {
            r->error = planetary::preview::DescribeException( std::current_exception() );
         }
         r->finished.store( true, std::memory_order_release );
      } );
   }
   catch ( ... )
   {
      m_run.reset();
      m_openWhenDone = false;
      throw;
   }
   GUI->Progress_Bar.SetRunning( 0, PhaseName( 0 ) );
   GUI->Progress_Timer.Start();
   ShowText();
   UpdateControls();
   GUI->Cancel_PushButton.Focus();
}

void PlanetaryStackInterface::PollStack()
{
   if ( !Running() )
      return;
   if ( m_run->finished.load( std::memory_order_acquire ) )
   {
      FinishStack();
      return;
   }
   if ( m_run->cancel.load() )
      return; // "Cancelling..." stays until the worker is done
   const ProgressSnapshot p = UnpackProgress( m_run->progress.load( std::memory_order_relaxed ) );
   GUI->Progress_Bar.SetRunning( OverallFraction( p, m_run->steps ),
                                 p.phase == 0 ? String( PhaseName( 0 ) )
                                              : FromUtf8( stackcards::ProgressText( PhaseName( p.phase ), p.done, p.total,
                                                                                    m_run->frames, m_run->steps ) ) );
}

void PlanetaryStackInterface::FinishStack()
{
   m_worker.join();
   std::unique_ptr<Run> run = std::move( m_run );
   const bool open = m_openWhenDone;
   m_openWhenDone = false;
   const String path = FromUtf8( run->path );
   const String fileName = File::ExtractNameAndExtension( path );
   // A Cancel after the worker's last poll (its final blend, the colour step)
   // still stands: nothing it made is kept (as ExecuteGlobal's Abort).
   const bool cancelled = run->cancelled || run->cancel.load();

   if ( run->job == Job::Mineral )
   {
      // The stack shown stays whatever happens to its mineral moon.
      if ( cancelled )
      {
         Console().WriteLn( "<end><cbr>PlanetaryStack: mineral moon cancelled; the stack is kept" );
         instance.p_mineralMoon = false;
         m_status = "Mineral moon cancelled: the stack is shown.";
         ShowPreview();
      }
      else if ( !run->error.empty() || !run->mineralError.empty() )
      {
         m_mineralError = run->error.empty() ? run->mineralError : run->error;
         Console().CriticalLn( "<end><cbr>*** PlanetaryStack: " + fileName.EncodedHTMLSpecialChars() + ": mineral moon: " +
                               FromUtf8( m_mineralError ).EncodedHTMLSpecialChars() );
         m_status = "Mineral moon failed: see below.";
      }
      else
      {
         m_mineral = run->cache;
         m_mineralShown = std::move( run->mineralMade );
         m_shownParams = run->params;
         if ( !m_mineralShown->notice.empty() )
            Console().WarningLn( "<end><cbr>PlanetaryStack warning: " +
                                 FromUtf8( stackcards::MineralNoticeText( m_mineralShown->notice ) ).EncodedHTMLSpecialChars() );
         m_status = m_mineralShown->notice.empty() ? String( "Mineral moon shown." )
                                                   : String( "Mineral moon: no lunar disk; see below." );
         ShowPreview();
      }
   }
   else if ( cancelled )
   {
      Console().WriteLn( "<end><cbr>PlanetaryStack: cancelled; no image was made" );
      m_status = "Cancelled: no image was made.";
   }
   else if ( !run->error.empty() )
   {
      m_error = ToUtf8( "PlanetaryStack: " + fileName + ": " ) + run->error;
      Console().CriticalLn( "<end><cbr>*** " + FromUtf8( m_error ).EncodedHTMLSpecialChars() );
      m_status = "Failed: see the error below.";
   }
   else
   {
      m_outcome = std::move( run->outcome );
      m_outcomePath = path;
      m_outcomePattern = run->pattern;
      m_times = run->times;
      WriteStackResult( path, *m_outcome, stackcards::WarningsFor::Window );
      if ( m_outcome->image )
      {
         m_status = String().Format( "Stacked in %.2f s.", m_outcome->summary.seconds );
         if ( run->cache )
         {
            m_mineral = run->cache;
            m_mineralShown = std::move( run->mineralMade );
            m_shownParams = run->params;
            if ( !m_mineralShown->notice.empty() )
            {
               Console().WarningLn( "<end><cbr>PlanetaryStack warning: " +
                                    FromUtf8( stackcards::MineralNoticeText( m_mineralShown->notice ) ).EncodedHTMLSpecialChars() );
               m_status += " Mineral moon: no lunar disk; see below.";
            }
            else
               m_status += " Mineral moon shown.";
         }
         else if ( !run->mineralError.empty() )
         {
            m_mineralError = run->mineralError;
            Console().CriticalLn( "<end><cbr>*** PlanetaryStack: " + fileName.EncodedHTMLSpecialChars() + ": mineral moon: " +
                                  FromUtf8( m_mineralError ).EncodedHTMLSpecialChars() );
            m_status += " Mineral moon failed: see below.";
         }
         // A new stack is shown at Fit.
         GUI->Preview_Control.SetBitmap( instance.p_mineralMoon && m_mineralShown
                                            ? MakeDisplayPreviewBitmap( m_mineralShown->image )
                                            : MakeStackPreviewBitmap( *m_outcome->image ) );
      }
      else
      {
         GUI->Preview_Control.Clear( kNoImageNote );
         m_status = "No image: see the diagnosis below.";
      }
   }
   GUI->Progress_Bar.SetIdle( m_status );
   ShowText();
   UpdateControls();
   // The next step has the focus: Execute after a stack, Stack! after a diagnosis, cancel or error.
   if ( m_outcome && m_outcome->image )
      GUI->Execute_PushButton.Focus();
   else
      GUI->Stack_PushButton.Focus();
   // The slider moved while the mineral moon was being made: the colour step for the newest values.
   if ( m_mineralShown && !SameParams( CurrentParams(), m_shownParams ) )
      RequestColour();
   if ( open && !cancelled && m_outcome && m_outcome->image )
   {
      if ( instance.p_mineralMoon && !m_mineralError.empty() )
         throw Error( "PlanetaryStack: " + File::ExtractNameAndExtension( m_outcomePath ) + ": mineral moon: " +
                      FromUtf8( m_mineralError ) );
      OpenWindows();
   }
}

void PlanetaryStackInterface::JoinStack()
{
   if ( m_run )
      m_run->cancel.store( true );
   if ( m_worker.joinable() )
      m_worker.join();
   m_run.reset();
   m_openWhenDone = false;
   JoinColour();
}

void PlanetaryStackInterface::StopStack( const char* why )
{
   if ( !Running() )
      return;
   JoinStack();
   Console().WriteLn( String( "<end><cbr>PlanetaryStack: cancelled because " ) + why + "; no image was made" );
   if ( GUI != nullptr )
   {
      GUI->Progress_Timer.Stop();
      m_status = "Cancelled: no image was made.";
      GUI->Progress_Bar.SetIdle( m_status );
      ShowText();
      UpdateControls();
      GUI->Stack_PushButton.Focus();
   }
}

void PlanetaryStackInterface::ExecuteStack()
{
   if ( Running() )
   {
      // Only Apply Global reaches here mid-run (Execute is disabled): the stack
      // opens when it is done.
      m_openWhenDone = true;
      Console().WriteLn( "<end><cbr>PlanetaryStack: the stack opens when this run is done" );
      return;
   }
   if ( !m_info )
      throw Error( "PlanetaryStack: no video loaded: click Open... to choose one." );
   // The file read again first: a stack of a file since replaced under the same
   // name (other frames or geometry) is dropped (ReadInfo, SameCapture), not opened.
   // A file that cannot be read now opens nothing; its stack stays (ReadInfo has
   // said why), and Execute opens it once the file reads again.
   if ( !ReadInfo( true/*interactive*/ ) || !m_info )
      return;
   if ( m_outcome )
   {
      if ( !m_outcome->image )
      {
         // Nothing to open: as loud as ExecuteGlobal's failure, with the diagnosis
         // left on screen (a thrown Error would clear the window it explains).
         const String message = "PlanetaryStack: " + File::ExtractNameAndExtension( m_outcomePath ) + ": no image: " +
                                FromUtf8( stackcards::DiagnosisText( m_outcome->diagnosis ) );
         Console().CriticalLn( "<end><cbr>*** " + message.EncodedHTMLSpecialChars() );
         MessageBox( message, "PlanetaryStack", StdIcon::Error, StdButton::Ok ).Execute();
         return;
      }
      // The stack shown (and, Mineral moon checked, its mineral moon): opened as
      // ExecuteGlobal would, without stacking again.
      if ( instance.p_mineralMoon )
      {
         if ( const std::optional<std::string> why = MineralRefusal() )
            throw Error( "PlanetaryStack: " + File::ExtractNameAndExtension( m_outcomePath ) + ": " + FromUtf8( *why ) );
         if ( !m_mineralError.empty() )
            throw Error( "PlanetaryStack: " + File::ExtractNameAndExtension( m_outcomePath ) + ": mineral moon: " +
                         FromUtf8( m_mineralError ) );
         // Checked after the stack and not made yet: made first (never a restack).
         if ( !m_mineral )
         {
            StartMineral( true/*openWhenDone*/ );
            return;
         }
      }
      OpenWindows();
      return;
   }
   StartStack( true/*openWhenDone*/ );
}

void PlanetaryStackInterface::SavePattern()
{
   if ( Running() || !m_info || !AviWithoutSidecar( *m_info ) || !ser::IsBayer( m_info->colorId ) ||
        !(m_outcome && m_outcome->image) )
      return;
   String failure;
   switch ( SaveBayerPattern( instance.p_filePath, m_info->colorId, "PlanetaryStack", failure ) )
   {
   case SavePatternResult::Declined:
      return;
   case SavePatternResult::Failed:
      m_error = ToUtf8( failure );
      ShowText();
      return;
   case SavePatternResult::Written:
      break;
   }
   // Read again with Auto: the new sidecar names the same pattern, so the info
   // block gives it as the source and the stack shown stays.
   instance.p_bayerPattern = PSBayerPattern::Auto;
   GUI->Bayer_ComboBox.SetCurrentItem( PSBayerPattern::Auto );
   ReadInfo( true/*interactive*/ );
}

void PlanetaryStackInterface::e_Click( Button& sender, bool /*checked*/ )
{
   Guarded( [this, &sender]()
   {
      if ( sender == GUI->Open_PushButton )
      {
         String path;
         if ( AskForVideo( instance.p_filePath, path ) )
         {
            // A new file starts with Bayer pattern Auto (its own sidecar decides)
            // and target Auto (its own name decides); an imported instance keeps
            // its bayerPattern and target (ImportProcess). Mineral moon stays as
            // chosen unless the new capture refuses it (ReadInfo says so).
            instance.p_filePath = path;
            instance.p_bayerPattern = PSBayerPattern::Auto;
            instance.p_target = PSTarget::Auto;
            m_mineralOff.clear();
            // The last run's line (a cancel, a failure) was about the old file.
            m_status.Clear();
            GUI->Progress_Bar.SetIdle( m_status );
            GUI->Bayer_ComboBox.SetCurrentItem( PSBayerPattern::Auto );
            GUI->Target_ComboBox.SetCurrentItem( PSTarget::Auto );
            ReadInfo( true/*interactive*/ );
         }
      }
      else if ( sender == GUI->Stack_PushButton )
         StartStack( false/*openWhenDone*/ );
      else if ( sender == GUI->Cancel_PushButton )
      {
         if ( Running() )
         {
            // The bar keeps its fill until the worker is done (PollStack then shows the idle line).
            m_run->cancel.store( true );
            GUI->Progress_Bar.SetRunning( OverallFraction( UnpackProgress( m_run->progress.load() ), m_run->steps ),
                                          "Cancelling..." );
            UpdateControls();
         }
      }
      else if ( sender == GUI->Execute_PushButton )
         ExecuteStack();
      else if ( sender == GUI->SavePattern_PushButton )
         SavePattern();
      else if ( sender == GUI->Mineral_CheckBox )
      {
         const bool checked = GUI->Mineral_CheckBox.IsChecked();
         if ( checked == bool( instance.p_mineralMoon ) )
            return;
         if ( checked )
            if ( const std::optional<std::string> why = MineralRefusal() )
            {
               // It cannot run here (the box is disabled for this; a click racing a change).
               m_mineralOff = *why;
               ShowText();
               UpdateControls();
               return;
            }
         instance.p_mineralMoon = checked;
         m_mineralOff.clear();
         // Neither way restacks: a stack shown gets its mineral moon made once
         // (a Mineral run), then shown or hidden; the colour follows the sliders.
         if ( checked && m_outcome && m_outcome->image && !m_mineral && m_mineralError.empty() )
            StartMineral( false/*openWhenDone*/ );
         else
         {
            if ( checked && m_mineralShown && !SameParams( CurrentParams(), m_shownParams ) )
               RequestColour();
            ShowPreview();
            ShowText();
            UpdateControls();
         }
      }
      else
      {
         if ( sender == GUI->Fit_PushButton )
            GUI->Preview_Control.ZoomToFit();
         else if ( sender == GUI->Zoom100_PushButton )
            GUI->Preview_Control.SetZoom( 1 );
         else if ( sender == GUI->Zoom200_PushButton )
            GUI->Preview_Control.SetZoom( 2 );
         else if ( sender == GUI->ZoomOut_PushButton )
            GUI->Preview_Control.ZoomOut();
         else if ( sender == GUI->ZoomIn_PushButton )
            GUI->Preview_Control.ZoomIn();
         UpdateControls();
      }
   }, true/*interactive*/ );
}

void PlanetaryStackInterface::e_ItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->Bayer_ComboBox && itemIndex != instance.p_bayerPattern )
      Guarded( [this, itemIndex]()
      {
         instance.p_bayerPattern = pcl_enum( itemIndex );
         // Read again with the new pattern; a stack made with another one goes.
         if ( m_info )
            ReadInfo( true/*interactive*/ );
         else
            UpdateControls();
      }, true/*interactive*/ );
   else if ( sender == GUI->Target_ComboBox && itemIndex != instance.p_target )
      Guarded( [this, itemIndex]()
      {
         // The target names the stack (OBJECT) and gates Mineral moon; the stack
         // itself does not depend on it, so it stays.
         instance.p_target = pcl_enum( itemIndex );
         m_mineralOff.clear();
         ApplyMineralRefusal();
         ShowText();
         UpdateControls();
      }, true/*interactive*/ );
}

void PlanetaryStackInterface::e_ValueUpdated( NumericEdit& sender, double value )
{
   Guarded( [this, &sender, value]()
   {
      if ( sender == GUI->Strength_NumericControl )
         instance.p_mineralStrength = value;
      else if ( sender == GUI->ChromaBlur_NumericControl )
         instance.p_chromaBlur = value;
      else
         return;
      // Only the colour step runs again, on the mineral data already made; a run
      // in progress picks the newest values up when it ends (FinishStack).
      if ( instance.p_mineralMoon && m_mineral && !Running() )
         RequestColour();
   }, false/*interactive*/ );
}

void PlanetaryStackInterface::e_Timer( Timer& )
{
   Guarded( [this]()
   {
      PollColour();
      PollStack();
      if ( !Running() && !m_colour )
         GUI->Progress_Timer.Stop();
   }, false/*interactive*/ );
}

void PlanetaryStackInterface::e_Show( Control& )
{
   if ( !m_info )
      Guarded( [this]() { LoadFromInstance(); }, false/*interactive*/ );
}

void PlanetaryStackInterface::e_Hide( Control& )
{
   // Nothing runs, and no stack is kept, behind a closed window. Only a window
   // hidden on purpose is closed: measured (PI 1.9.5, GUI smoke), the title-bar
   // close hides it with IsVisible() false, and collapsing it to its title bar
   // sends no hide at all. A hide from the window system (the main window
   // minimised) leaves Qt's visibility set: the run goes on.
   if ( IsVisible() )
      return;
   Guarded( [this]()
   {
      StopStack( "the window was closed" );
      ClearAll();
   }, false/*interactive*/ );
}

PlanetaryStackInterface::GUIData::GUIData( PlanetaryStackInterface& w )
{
   Open_PushButton.SetText( "Open..." );
   Open_PushButton.SetToolTip( "<p>Choose a planetary or lunar video: SER (.ser) or uncompressed 8-bit greyscale or raw "
                               "Bayer AVI (.avi). Its frames, size and colour are shown below; then click Stack!.</p>" );
   Open_PushButton.OnClick( (Button::click_event_handler)&PlanetaryStackInterface::e_Click, w );

   Bayer_Label.SetText( "Bayer pattern:" );
   Bayer_Label.SetTextAlignment( TextAlign::Left|TextAlign::VertCenter );

   // Item index == PSBayerPattern value.
   SetUpBayerCombo( Bayer_ComboBox );
   Bayer_ComboBox.SetCurrentItem( w.instance.p_bayerPattern );
   Bayer_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&PlanetaryStackInterface::e_ItemSelected, w );

   SavePattern_PushButton.SetText( "Save pattern..." );
   SavePattern_PushButton.SetToolTip( "<p>AVI without a sidecar only: remember the Bayer pattern chosen above for this "
                                      "video. It writes a small text file next to the video (&lt;name&gt;.avi.txt, "
                                      "the sidecar Auto reads), so Auto uses this pattern whenever the file is opened, "
                                      "here and in SERViewer. Enabled once a pattern is chosen and its stack is shown: "
                                      "the stack's colours are how you tell the pattern is right. Asks first; never "
                                      "replaces an existing sidecar.</p>" );
   SavePattern_PushButton.OnClick( (Button::click_event_handler)&PlanetaryStackInterface::e_Click, w );

   File_Sizer.SetSpacing( 6 );
   File_Sizer.Add( Open_PushButton );
   File_Sizer.AddSpacing( 12 );
   File_Sizer.Add( Bayer_Label );
   File_Sizer.Add( Bayer_ComboBox );
   File_Sizer.Add( SavePattern_PushButton );
   File_Sizer.AddStretch();

   Stack_PushButton.SetText( "Stack!" );
   Stack_PushButton.SetToolTip( "<p>Stack the video: every frame is measured, the sharpest are chosen from the "
                                "capture's own statistics, aligned to the target and region by region, and stacked. "
                                "Nothing to set. The bar shows how far it is; the window stays usable.</p>" );
   Stack_PushButton.OnClick( (Button::click_event_handler)&PlanetaryStackInterface::e_Click, w );

   Progress_Bar.SetToolTip( "<p>The stack's progress: measuring every frame, aligning, stacking.</p>" );

   Cancel_PushButton.SetText( "Cancel" );
   Cancel_PushButton.SetToolTip( "<p>Stop the stack. Nothing is left behind: no image, no file.</p>" );
   Cancel_PushButton.OnClick( (Button::click_event_handler)&PlanetaryStackInterface::e_Click, w );

   Run_Sizer.SetSpacing( 6 );
   Run_Sizer.Add( Stack_PushButton );
   Run_Sizer.Add( Progress_Bar, 100 );
   Run_Sizer.Add( Cancel_PushButton );

   Preview_Control.SetScaledMinSize( 400, 300 );
   Preview_Control.SetToolTip( "<p>The stack, stretched for display only (the image Execute opens is linear). "
                               "Drag to move around it when zoomed in.</p>" );

   Fit_PushButton.SetText( "Fit" );
   Fit_PushButton.SetToolTip( "<p>The whole stack in the preview.</p>" );
   Zoom100_PushButton.SetText( "100%" );
   Zoom100_PushButton.SetToolTip( "<p>One stack pixel per screen pixel.</p>" );
   Zoom200_PushButton.SetText( "200%" );
   Zoom200_PushButton.SetToolTip( "<p>Each stack pixel two screen pixels wide.</p>" );
   ZoomOut_PushButton.SetText( "-" );
   ZoomOut_PushButton.SetToolTip( "<p>Zoom out.</p>" );
   ZoomIn_PushButton.SetText( "+" );
   ZoomIn_PushButton.SetToolTip( "<p>Zoom in.</p>" );
   for ( PushButton* b : { &Fit_PushButton, &Zoom100_PushButton, &Zoom200_PushButton, &ZoomOut_PushButton,
                           &ZoomIn_PushButton } )
      b->OnClick( (Button::click_event_handler)&PlanetaryStackInterface::e_Click, w );
   Zoom_Label.SetTextAlignment( TextAlign::Right|TextAlign::VertCenter );
   Zoom_Label.SetScaledMinWidth( 80 );
   ZoomOut_PushButton.SetScaledFixedWidth( 40 );
   ZoomIn_PushButton.SetScaledFixedWidth( 40 );

   Zoom_Sizer.SetSpacing( 4 );
   Zoom_Sizer.Add( Fit_PushButton );
   Zoom_Sizer.Add( Zoom100_PushButton );
   Zoom_Sizer.Add( Zoom200_PushButton );
   Zoom_Sizer.Add( ZoomOut_PushButton );
   Zoom_Sizer.Add( ZoomIn_PushButton );
   Zoom_Sizer.AddStretch();
   Zoom_Sizer.Add( Zoom_Label );

   const int labelWidth = w.Font().Width( String( "Chroma blur:" ) ) + w.LogicalPixelsToPhysical( 6 );
   const int editWidth = w.Font().Width( String( "00.00" ) ) + w.LogicalPixelsToPhysical( 16 );

   Target_Label.SetText( "Target:" );
   Target_Label.SetTextAlignment( TextAlign::Right|TextAlign::VertCenter );
   Target_Label.SetMinWidth( labelWidth );
   // Item index == PSTarget value.
   for ( const char* item : kTargetItems )
      Target_ComboBox.AddItem( item );
   Target_ComboBox.SetCurrentItem( w.instance.p_target );
   Target_ComboBox.SetMinWidth( w.Font().Width( String( "Neptune (from file name)" ) ) + w.LogicalPixelsToPhysical( 32 ) );
   Target_ComboBox.SetToolTip( "<p>What the video is of. The first item reads it from an ASIAIR capture name "
                               "(&lt;date&gt;-&lt;time&gt;-&lt;Target&gt;-Bin1 &lt;Filter&gt; &lt;temp&gt;F); choose another "
                               "when the name is wrong or not one. It names the stack (its OBJECT keyword) and decides "
                               "whether Mineral moon can run. Changing it never stacks again.</p>" );
   Target_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&PlanetaryStackInterface::e_ItemSelected, w );

   Mineral_CheckBox.SetText( "Mineral moon" );
   Mineral_CheckBox.SetToolTip( kMineralTip );
   Mineral_CheckBox.OnClick( (Button::click_event_handler)&PlanetaryStackInterface::e_Click, w );

   Target_Sizer.SetSpacing( 6 );
   Target_Sizer.Add( Target_Label );
   Target_Sizer.Add( Target_ComboBox );
   Target_Sizer.AddSpacing( 12 );
   Target_Sizer.Add( Mineral_CheckBox );
   Target_Sizer.AddStretch();

   // The mineral moon's colour: either slider re-runs only its colour step.
   Strength_NumericControl.label.SetText( "Strength:" );
   Strength_NumericControl.label.SetMinWidth( labelWidth );
   Strength_NumericControl.slider.SetRange( 0, 150 );
   Strength_NumericControl.slider.SetScaledMinWidth( 250 );
   Strength_NumericControl.SetReal();
   Strength_NumericControl.SetRange( ThePSMineralStrengthParameter->MinimumValue(), ThePSMineralStrengthParameter->MaximumValue() );
   Strength_NumericControl.SetPrecision( 2 );
   Strength_NumericControl.EnableFixedPrecision();   // "14.00", as "8.00": never "14.0"
   Strength_NumericControl.edit.SetFixedWidth( editWidth );
   Strength_NumericControl.SetToolTip( "<p>How strongly the Moon's colour differences are amplified (the colour "
                                       "gain k): 0 is grey, 8 the default, 15 the most. Only the colour is computed "
                                       "again; the stack and its sharpening stay.</p>" );
   Strength_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&PlanetaryStackInterface::e_ValueUpdated, w );

   ChromaBlur_NumericControl.label.SetText( "Chroma blur:" );
   ChromaBlur_NumericControl.label.SetMinWidth( labelWidth );
   ChromaBlur_NumericControl.slider.SetRange( 0, 110 );
   ChromaBlur_NumericControl.slider.SetScaledMinWidth( 250 );
   ChromaBlur_NumericControl.SetReal();
   ChromaBlur_NumericControl.SetRange( ThePSChromaBlurParameter->MinimumValue(), ThePSChromaBlurParameter->MaximumValue() );
   ChromaBlur_NumericControl.SetPrecision( 2 );
   ChromaBlur_NumericControl.EnableFixedPrecision();
   ChromaBlur_NumericControl.edit.SetFixedWidth( editWidth );
   ChromaBlur_NumericControl.SetToolTip( "<p>How far the colour is smoothed, in pixels (frames up to 2000 px wide; "
                                         "scaled for wider ones): more hides the colour's noise, less keeps small "
                                         "colour features. The detail always comes from the sharp luminance. Only the "
                                         "colour is computed again.</p>" );
   ChromaBlur_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&PlanetaryStackInterface::e_ValueUpdated, w );

   Info_TextBox.SetReadOnly();
   // Never focused (mouse selection and copy still work): a button that disables
   // itself (Stack!, Cancel) would otherwise hand Qt's focus here, with a caret and a
   // focus ring in the result. Focus moves explicitly instead (StartStack, FinishStack).
   Info_TextBox.SetFocusStyle( FocusStyle::NoFocus );
   Info_TextBox.SetScaledMinSize( 400, 200 );

   Execute_PushButton.SetText( "Execute" );
   Execute_PushButton.SetToolTip( "<p>Open the stack as a new image window: 32-bit float, linear, with FITS keywords "
                                  "(source video, frames used, how they were chosen and aligned). When nothing is "
                                  "stacked yet, it stacks first.</p>" );
   Execute_PushButton.OnClick( (Button::click_event_handler)&PlanetaryStackInterface::e_Click, w );

   Execute_Sizer.AddStretch();
   Execute_Sizer.Add( Execute_PushButton );

   // The GUI smoke (test/gui_drive.py stack_controls()) clicks relative to the
   // preview: two rows above it (Open... / Bayer pattern / Save pattern..., then
   // Stack! / the progress bar / Cancel), the zoom row below it, then Target /
   // Mineral moon, Strength and Chroma blur, the info
   // block and Execute. A layout change here means re-measuring those offsets.
   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( File_Sizer );
   Global_Sizer.Add( Run_Sizer );
   Global_Sizer.Add( Preview_Control, 100 );
   Global_Sizer.Add( Zoom_Sizer );
   Global_Sizer.Add( Target_Sizer );
   Global_Sizer.Add( Strength_NumericControl );
   Global_Sizer.Add( ChromaBlur_NumericControl );
   Global_Sizer.Add( Info_TextBox );
   Global_Sizer.Add( Execute_Sizer );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   // Laid out with Save pattern... shown, so its row keeps room for it (as in
   // SERViewer); hidden until an AVI with no sidecar is open (UpdateControls).
   w.SetMinWidth( w.Width() );
   SavePattern_PushButton.Hide();

   Progress_Timer.SetInterval( 0.1 );
   Progress_Timer.SetPeriodic();
   Progress_Timer.OnTimer( (Timer::timer_event_handler)&PlanetaryStackInterface::e_Timer, w );
}

} // namespace pcl
