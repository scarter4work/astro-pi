// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryStackInstance.h"
#include "PlanetaryStackParameters.h"

#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>
#include <pcl/File.h>
#include <pcl/Image.h>
#include <pcl/MetaModule.h>
#include <pcl/StandardStatus.h>
#include <pcl/StatusMonitor.h>
#include <pcl/Thread.h>

#include <exception>
#include <memory>

#include "FitsCards.h"
#include "NewImageWindow.h"
#include "PlanetaryVersion.h"
#include "SerFile.h"
#include "SerInfoText.h"
#include "SerTime.h"

namespace ser = planetary::ser;
namespace stack = planetary::stack;
namespace preview = planetary::preview;
namespace stackcards = planetary::stackcards;
using namespace pcl::planetary_fits;

namespace pcl
{

namespace
{

// Console text is parsed for tags, so everything from a file, a sidecar or the
// engine goes in encoded.
String Html( const std::string& utf8 )
{
   return String::UTF8ToUTF16( utf8.c_str() ).EncodedHTMLSpecialChars();
}

String Num( int64_t n )
{
   return String( std::to_string( n ).c_str() );
}

// An interleaved engine image as a PCL image (separate planes): RGB for 3
// channels, else grey.
Image ImageOf( const ser::FloatImage& img )
{
   Image image( img.width, img.height, img.channels == 3 ? ColorSpace::RGB : ColorSpace::Gray );
   const size_t n = size_t( img.width ) * size_t( img.height );
   for ( int c = 0; c < img.channels; ++c )
   {
      float* plane = image.PixelData( c );
      for ( size_t i = 0; i < n; ++i )
         plane[i] = img.data[i * img.channels + c];
   }
   return image;
}

} // namespace

stack::MineralParams MineralParamsFor( double strength, double chromaBlur, const char* process )
{
   const auto check = [process]( double v, const MetaDouble* p )
   {
      if ( !(v >= p->MinimumValue() && v <= p->MaximumValue()) )
         throw Error( String( process ) + ": " + String( p->Id() ) + " " + String( v ) + " is outside " +
                      String( p->MinimumValue() ) + " to " + String( p->MaximumValue() ) );
   };
   check( strength, ThePSMineralStrengthParameter );
   check( chromaBlur, ThePSChromaBlurParameter );
   stack::MineralParams p;
   p.strength = strength;
   p.chromaBlurSigma = chromaBlur;
   return p;
}

void WriteStackContext( const String& path, const ser::FileInfo& info, int threads, const TargetChoice& target,
                        const stack::MineralParams* mineral )
{
   const String fileName = File::ExtractNameAndExtension( path );
   Console console;
   console.WriteLn( "<end><cbr>PlanetaryStack " PLANETARY_VERSION_STRING ": stacking " + fileName.EncodedHTMLSpecialChars() + ": " +
                    Num( info.frameCount ) + " frames, " + Num( info.header.width ) + "x" + Num( info.header.height ) +
                    ", " + String( ser::ColorIdName( info.colorId ) ) + ", " + Num( info.header.depth ) + "-bit; " +
                    Num( threads ) + " threads" );
   if ( stackcards::IsAvi( info ) )
      console.WriteLn( "PlanetaryStack: Bayer " + Html( preview::BayerText( info ) ) );
   console.WriteLn( "PlanetaryStack: target " + Html( stackcards::TargetText( target ) ) +
                    (mineral == nullptr ? String( "; no mineral moon" )
                                        : String( "; then the mineral moon: strength " ) +
                                             Html( stackcards::Fixed( mineral->strength, 2 ) ) + ", chroma blur " +
                                             Html( stackcards::Fixed( mineral->chromaBlurSigma, 2 ) ) + " px" ) );
}

int StackThreads()
{
   return Max( 1, Thread::NumberOfThreads( PCL_MAX_PROCESSORS, 1 ) );
}

StackTimes CaptureTimes( const ser::FrameSource& source, const stack::StackOutcome& outcome )
{
   StackTimes t;
   const ser::FileInfo& info = source.Info();
   if ( stackcards::IsAvi( info ) || !outcome.image || outcome.frames.stacked.empty() )
      return t;
   if ( info.hasTimestamps )
   {
      const std::optional<int64_t> first = source.FrameTimestamp( outcome.frames.stacked.front() );
      const std::optional<int64_t> last = source.FrameTimestamp( outcome.frames.stacked.back() );
      if ( first && last )
      {
         t.start = ser::TicksToIsoUtc( *first );
         t.end = ser::TicksToIsoUtc( *last );
         if ( t.start && t.end )
         {
            t.fromFrames = true;
            return t;
         }
         t.start = t.end = std::nullopt;
      }
   }
   t.start = ser::TicksToIsoUtc( info.header.dateTimeUtc );
   return t;
}

FITSKeywordArray StackKeywords( const String& path, const stack::StackOutcome& outcome, const StackTimes& times,
                                const TargetChoice& target )
{
   const std::string fileName = File::ExtractNameAndExtension( path ).ToUTF8().c_str();
   FITSKeywordArray keywords;
   for ( const stackcards::Card& card : stackcards::StackCards( fileName, outcome, times, target ) )
      keywords << FITSHeaderKeyword( card.name.c_str(),
                                     card.text ? FitsString( card.value ) : IsoString( card.value.c_str() ),
                                     card.comment.c_str() );
   for ( const std::string& text : stackcards::StackHistory( fileName, outcome, target ) )
      AddHistory( keywords, text );
   return keywords;
}

void WriteStackResult( const String& path, const stack::StackOutcome& outcome, stackcards::WarningsFor where )
{
   const stack::StackSummary& s = outcome.summary;
   Console console;
   console.Write( "<end><cbr>" );
   // One line per condition (stackcards::ShownWarnings: an undeclared mosaic's two become one).
   for ( const std::string& w : stackcards::ShownWarnings( outcome, where ) )
      console.WarningLn( "PlanetaryStack warning: " + Html( w ) );
   if ( !outcome.image )
   {
      console.WarningLn( "PlanetaryStack: no image from " + File::ExtractNameAndExtension( path ).EncodedHTMLSpecialChars() +
                         ": " + Html( stackcards::DiagnosisText( outcome.diagnosis ) ) );
      console.WriteLn( "PlanetaryStack: " + Html( stackcards::CountsLine( s, false/*stacked*/ ) ) );
      return;
   }
   console.WriteLn( "PlanetaryStack: " + Html( s.selection ) );
   console.WriteLn( "PlanetaryStack: " + Html( s.alignment ) );
   console.WriteLn( "PlanetaryStack: " + Html( stackcards::CountsLine( s, true/*stacked*/ ) ) );
}

IsoString OpenStackWindow( const String& path, const stack::StackOutcome& outcome, const StackTimes& times,
                           const TargetChoice& target )
{
   const String fileName = File::ExtractNameAndExtension( path );
   if ( !outcome.image )
      throw Error( "PlanetaryStack: internal error - no stack to open for " + fileName );
   const ser::FloatImage& stacked = *outcome.image;
   const int channels = stacked.channels;
   // The image and its keywords are complete before any window exists: the
   // window only receives a copy (ShowNewImageWindow).
   const FITSKeywordArray keywords = StackKeywords( path, outcome, times, target );
   const Image image = ImageOf( stacked );
   const IsoString viewId = ShowNewImageWindow( image, ViewIdStem( path ) + "_stack", keywords, "PlanetaryStack: " + fileName );

   Console().WriteLn( "<end><cbr>PlanetaryStack: " + fileName.EncodedHTMLSpecialChars() + " -> " + String( viewId ) + " (" +
                      Num( stacked.width ) + "x" + Num( stacked.height ) + ", " +
                      (channels == 3 ? String( "RGB" ) : stack::UndeclaredMosaic( outcome.info )
                                                            ? String( "mono: the raw colour mosaic, see the warning above" )
                                                            : String( "mono" )) +
                      ", 32-bit float, linear)" );
   return viewId;
}

MineralWindow MakeMineralWindow( const String& path, const stack::StackOutcome& outcome, const StackTimes& times,
                                 const TargetChoice& target, const stack::MineralOutcome& mineral )
{
   if ( mineral.image.channels != 3 )
      throw Error( "PlanetaryStack: internal error - the mineral moon of " + File::ExtractNameAndExtension( path ) +
                   " is not RGB" );
   MineralWindow w;
   w.keywords = StackKeywords( path, outcome, times, target );
   for ( const std::string& text : stackcards::MineralHistory( mineral ) )
      AddHistory( w.keywords, text );
   w.image = ImageOf( mineral.image );
   return w;
}

IsoString ShowMineralWindow( const String& path, const MineralWindow& window, const stack::MineralOutcome& mineral )
{
   const String fileName = File::ExtractNameAndExtension( path );
   Console console;
   double seconds = 0;
   String steps;
   for ( const auto& [name, s] : mineral.stepSeconds )
   {
      seconds += s;
      if ( !steps.IsEmpty() )
         steps += ", ";
      steps += Html( name + " " + stackcards::Fixed( s, 2 ) + " s" );
   }
   console.WriteLn( "<end><cbr>PlanetaryStack: mineral moon in " + Html( stackcards::Fixed( seconds, 2 ) ) + " s (" + steps + ")" );
   if ( !mineral.history.empty() )
      console.WriteLn( "PlanetaryStack: " + Html( mineral.history.back() ) );
   if ( !mineral.notice.empty() )
      console.WarningLn( "PlanetaryStack warning: " + Html( stackcards::MineralNoticeText( mineral.notice ) ) );
   const IsoString viewId = ShowNewImageWindow( window.image, ViewIdStem( path ) + "_mineral", window.keywords,
                                                "PlanetaryStack: " + fileName );
   console.WriteLn( "PlanetaryStack: " + fileName.EncodedHTMLSpecialChars() + " -> " + String( viewId ) + " (" +
                    Num( mineral.image.width ) + "x" + Num( mineral.image.height ) + ", RGB, 32-bit float, " +
                    (mineral.notice.empty() ? String( "mineral moon, display-ready)" )
                                            : String( "the stack clipped to [0, 1]: no mineral moon, see the warning above)" )) );
   return viewId;
}

stack::MineralOutcome MineralOutcomeOf( const stack::MineralCache& cache, ser::FloatImage image, const stack::StepRecord& colour,
                                        double colourSeconds )
{
   stack::MineralOutcome m;
   m.image = std::move( image );
   m.history = cache.history;
   m.history.push_back( colour.history );
   m.stepSeconds = cache.stepSeconds;
   m.stepSeconds.push_back( { "Mineral colour", colourSeconds } );
   m.notice = cache.notice;
   return m;
}

PlanetaryStackInstance::PlanetaryStackInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_filePath( ThePSFilePathParameter->DefaultValue() )
   , p_bayerPattern( pcl_enum( ThePSBayerPatternParameter->DefaultValueIndex() ) )
   , p_target( pcl_enum( ThePSTargetParameter->DefaultValueIndex() ) )
   , p_mineralMoon( ThePSMineralMoonParameter->DefaultValue() )
   , p_mineralStrength( ThePSMineralStrengthParameter->DefaultValue() )
   , p_chromaBlur( ThePSChromaBlurParameter->DefaultValue() )
{
}

PlanetaryStackInstance::PlanetaryStackInstance( const PlanetaryStackInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

void PlanetaryStackInstance::Assign( const ProcessImplementation& p )
{
   const PlanetaryStackInstance* x = dynamic_cast<const PlanetaryStackInstance*>( &p );
   if ( x != nullptr )
   {
      p_filePath = x->p_filePath;
      p_bayerPattern = x->p_bayerPattern;
      p_target = x->p_target;
      p_mineralMoon = x->p_mineralMoon;
      p_mineralStrength = x->p_mineralStrength;
      p_chromaBlur = x->p_chromaBlur;
   }
}

bool PlanetaryStackInstance::CanExecuteOn( const View&, String& whyNot ) const
{
   whyNot = "PlanetaryStack is a global process: it stacks a video into a new image and cannot run on a view.";
   return false;
}

bool PlanetaryStackInstance::CanExecuteGlobal( String& whyNot ) const
{
   if ( p_filePath.Trimmed().IsEmpty() )
   {
      whyNot = "PlanetaryStack: no video file selected (filePath is empty).";
      return false;
   }
   return true;
}

bool PlanetaryStackInstance::ExecuteGlobal()
{
   String whyNot;
   if ( !CanExecuteGlobal( whyNot ) )
      throw Error( whyNot );

   const String path = p_filePath.Trimmed();
   const std::string pathUtf8 = path.ToUTF8().c_str();
   const String fileName = File::ExtractNameAndExtension( path );
   const ser::BayerChoice bayer = BayerChoiceFor( p_bayerPattern, "PlanetaryStack" );
   const TargetChoice target = stackcards::ResolveTarget( TargetFor( p_target, "PlanetaryStack" ), pathUtf8 );
   std::optional<stack::MineralParams> mineralParams;
   if ( p_mineralMoon )
      mineralParams = MineralParamsFor( p_mineralStrength, p_chromaBlur, "PlanetaryStack" );
   const int threads = StackThreads();

   // Opened here for the context line, Mineral moon's colour check and, after
   // the stack, a SER's frame times; the engine opens its own readers (one per
   // worker).
   std::unique_ptr<ser::FrameSource> source;
   try
   {
      source = ser::OpenVideo( pathUtf8, bayer );
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "PlanetaryStack: " + fileName + ": " + String::UTF8ToUTF16( e.what() ) );
   }

   // Mineral moon on anything but a broadband colour Moon fails before any
   // stacking. The info is OpenVideo's: an AVI's Bayer pattern is resolved by
   // then (B1 review m2: the AVI reader alone reports Mono).
   if ( mineralParams )
      if ( const std::optional<std::string> why = stackcards::MineralRefusalFor( pathUtf8, source->Info(), target ) )
         throw Error( "PlanetaryStack: " + fileName + ": " + String::UTF8ToUTF16( why->c_str() ) );

   WriteStackContext( path, source->Info(), threads, target, mineralParams ? &*mineralParams : nullptr );
   Console console;

   // Progress: one StatusMonitor run per engine phase (the stack's three, then
   // the mineral moon's), with the engine's fixed
   // total. The console's Abort button makes the monitor throw ProcessAborted;
   // that is caught here, so the engine only ever sees cancelled() turn true
   // and leaves by its own cancel path (workers joined, readers closed,
   // StackCancelled).
   console.EnableAbort();
   StandardStatus status;
   StatusMonitor monitor;
   monitor.SetCallback( &status );
   bool aborted = false;
   std::string phase;
   int64_t shown = 0;
   stack::Progress progress;
   progress.report = [&]( const std::string& name, int64_t done, int64_t total )
   {
      if ( aborted || total <= 0 )
         return;
      try
      {
         if ( name != phase )
         {
            phase = name;
            shown = 0;
            monitor.Initialize( "PlanetaryStack: " + String( name.c_str() ), size_type( total ) );
         }
         if ( done > shown )
         {
            monitor += size_type( done - shown );
            shown = done;
         }
      }
      catch ( const ProcessAborted& )
      {
         aborted = true;
      }
   };
   progress.cancelled = [&aborted] { return aborted; };
   const auto cancelled = [&console]()
   {
      console.WriteLn( "<end><cbr>PlanetaryStack: cancelled; no image was made" );
      return ProcessAborted();
   };
   // An Abort pressed after the engine's last cancelled() poll -- during its
   // final blend, when no progress report runs to deliver it -- leaves RunStack
   // (or MakeMineralMoon) to return normally. Deliver the pending events and
   // honour it, before anything is written or opened (final review, module m3).
   const auto honourAbort = [&]()
   {
      if ( !aborted )
      {
         Module->ProcessEvents();
         aborted = console.AbortRequested();
      }
      if ( aborted )
         throw cancelled();
   };

   stack::StackOutcome outcome;
   try
   {
      outcome = stack::RunStack( pathUtf8, bayer, threads, progress );
   }
   catch ( const stack::StackCancelled& )
   {
      throw cancelled();
   }
   catch ( ... )
   {
      throw Error( "PlanetaryStack: " + fileName + ": " +
                   String::UTF8ToUTF16( preview::DescribeException( std::current_exception() ).c_str() ) );
   }
   honourAbort();

   WriteStackResult( path, outcome );
   // No image from a capture with nothing usable: fail loudly with the
   // diagnosis (no window).
   if ( !outcome.image )
      throw Error( "PlanetaryStack: " + fileName + ": no image: " +
                   String::UTF8ToUTF16( outcome.diagnosis.message.c_str() ) );

   StackTimes times;
   try
   {
      times = CaptureTimes( *source, outcome );
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "PlanetaryStack: " + fileName + ": " + String::UTF8ToUTF16( e.what() ) );
   }

   // The mineral moon before any window: an Abort or a failure here leaves none.
   std::optional<stack::MineralOutcome> mineral;
   if ( mineralParams )
   {
      try
      {
         mineral = stack::MakeMineralMoon( *outcome.image, threads, progress, *mineralParams );
      }
      catch ( const stack::StackCancelled& )
      {
         throw cancelled();
      }
      catch ( ... )
      {
         throw Error( "PlanetaryStack: " + fileName + ": mineral moon: " +
                      String::UTF8ToUTF16( preview::DescribeException( std::current_exception() ).c_str() ) );
      }
      honourAbort();
   }

   // Both or none: everything that can fail for the mineral window (its checks, keywords and pixel copy) is done
   // before the stack window is shown; what is left are the two windows themselves (B8 review M1).
   std::optional<MineralWindow> mineralWindow;
   if ( mineral )
      mineralWindow = MakeMineralWindow( path, outcome, times, target, *mineral );
   OpenStackWindow( path, outcome, times, target );
   if ( mineralWindow )
      ShowMineralWindow( path, *mineralWindow, *mineral );
   return true;
}

void* PlanetaryStackInstance::LockParameter( const MetaParameter* p, size_type )
{
   if ( p == ThePSFilePathParameter )        return p_filePath.Begin();
   if ( p == ThePSBayerPatternParameter )    return &p_bayerPattern;
   if ( p == ThePSTargetParameter )          return &p_target;
   if ( p == ThePSMineralMoonParameter )     return &p_mineralMoon;
   if ( p == ThePSMineralStrengthParameter ) return &p_mineralStrength;
   if ( p == ThePSChromaBlurParameter )      return &p_chromaBlur;
   return nullptr;
}


bool PlanetaryStackInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type )
{
   if ( p != ThePSFilePathParameter )
      return false;
   p_filePath.Clear();
   if ( sizeOrLength > 0 )
      p_filePath.SetLength( sizeOrLength );
   return true;
}

size_type PlanetaryStackInstance::ParameterLength( const MetaParameter* p, size_type ) const
{
   if ( p == ThePSFilePathParameter )
      return p_filePath.Length();
   return 0;
}

} // namespace pcl
