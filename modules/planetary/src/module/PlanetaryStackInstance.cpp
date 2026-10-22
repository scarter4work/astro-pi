// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryStackInstance.h"
#include "PlanetaryStackParameters.h"

#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>
#include <pcl/File.h>
#include <pcl/Image.h>
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

} // namespace

void WriteStackContext( const String& path, const ser::FileInfo& info, int threads )
{
   const String fileName = File::ExtractNameAndExtension( path );
   Console console;
   console.WriteLn( "<end><cbr>PlanetaryStack " PLANETARY_VERSION_STRING ": stacking " + fileName.EncodedHTMLSpecialChars() + ": " +
                    Num( info.frameCount ) + " frames, " + Num( info.header.width ) + "x" + Num( info.header.height ) +
                    ", " + String( ser::ColorIdName( info.colorId ) ) + ", " + Num( info.header.depth ) + "-bit; " +
                    Num( threads ) + " threads" );
   if ( stackcards::IsAvi( info ) )
      console.WriteLn( "PlanetaryStack: Bayer " + Html( preview::BayerText( info ) ) );
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

FITSKeywordArray StackKeywords( const String& path, const stack::StackOutcome& outcome, const StackTimes& times )
{
   const std::string fileName = File::ExtractNameAndExtension( path ).ToUTF8().c_str();
   FITSKeywordArray keywords;
   for ( const stackcards::Card& card : stackcards::StackCards( fileName, outcome, times ) )
      keywords << FITSHeaderKeyword( card.name.c_str(),
                                     card.text ? FitsString( card.value ) : IsoString( card.value.c_str() ),
                                     card.comment.c_str() );
   for ( const std::string& text : stackcards::StackHistory( fileName, outcome ) )
      AddHistory( keywords, text );
   return keywords;
}

void WriteStackResult( const String& path, const stack::StackOutcome& outcome )
{
   const stack::StackSummary& s = outcome.summary;
   Console console;
   console.Write( "<end><cbr>" );
   for ( const std::string& w : s.warnings )
      console.WarningLn( "PlanetaryStack warning: " + Html( preview::StackWarning( w ) ) );
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

IsoString OpenStackWindow( const String& path, const stack::StackOutcome& outcome, const StackTimes& times )
{
   const String fileName = File::ExtractNameAndExtension( path );
   if ( !outcome.image )
      throw Error( "PlanetaryStack: internal error - no stack to open for " + fileName );
   const ser::FloatImage& stacked = *outcome.image;
   const int channels = stacked.channels;
   // The image and its keywords are complete before any window exists: the
   // window only receives a copy (ShowNewImageWindow). The stack is
   // interleaved; the image's planes are separate.
   const FITSKeywordArray keywords = StackKeywords( path, outcome, times );
   Image image( stacked.width, stacked.height, channels == 3 ? ColorSpace::RGB : ColorSpace::Gray );
   const size_t n = size_t( stacked.width ) * size_t( stacked.height );
   for ( int c = 0; c < channels; ++c )
   {
      float* plane = image.PixelData( c );
      for ( size_t i = 0; i < n; ++i )
         plane[i] = stacked.data[i * channels + c];
   }
   const IsoString viewId = ShowNewImageWindow( image, ViewIdStem( path ) + "_stack", keywords, "PlanetaryStack: " + fileName );

   Console().WriteLn( "<end><cbr>PlanetaryStack: " + fileName.EncodedHTMLSpecialChars() + " -> " + String( viewId ) + " (" +
                      Num( stacked.width ) + "x" + Num( stacked.height ) + ", " +
                      (channels == 3 ? String( "RGB" ) : stack::UndeclaredMosaic( outcome.info )
                                                            ? String( "mono: the raw colour mosaic, see the warning above" )
                                                            : String( "mono" )) +
                      ", 32-bit float, linear)" );
   return viewId;
}

PlanetaryStackInstance::PlanetaryStackInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_filePath( ThePSFilePathParameter->DefaultValue() )
   , p_bayerPattern( pcl_enum( ThePSBayerPatternParameter->DefaultValueIndex() ) )
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
   const String fileName = File::ExtractNameAndExtension( path );
   const ser::BayerChoice bayer = BayerChoiceFor( p_bayerPattern, "PlanetaryStack" );
   const int threads = StackThreads();

   // Opened here for the context line and, after the stack, a SER's frame
   // times; the engine opens its own readers (one per worker).
   std::unique_ptr<ser::FrameSource> source;
   try
   {
      source = ser::OpenVideo( path.ToUTF8().c_str(), bayer );
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "PlanetaryStack: " + fileName + ": " + String::UTF8ToUTF16( e.what() ) );
   }

   WriteStackContext( path, source->Info(), threads );
   Console console;

   // Progress: one StatusMonitor run per engine phase, with the engine's fixed
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

   stack::StackOutcome outcome;
   try
   {
      outcome = stack::RunStack( path.ToUTF8().c_str(), bayer, threads, progress );
   }
   catch ( const stack::StackCancelled& )
   {
      console.WriteLn( "<end><cbr>PlanetaryStack: cancelled; no image was made" );
      throw ProcessAborted();
   }
   catch ( ... )
   {
      throw Error( "PlanetaryStack: " + fileName + ": " +
                   String::UTF8ToUTF16( preview::DescribeException( std::current_exception() ).c_str() ) );
   }

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
   OpenStackWindow( path, outcome, times );
   return true;
}

void* PlanetaryStackInstance::LockParameter( const MetaParameter* p, size_type )
{
   if ( p == ThePSFilePathParameter )     return p_filePath.Begin();
   if ( p == ThePSBayerPatternParameter ) return &p_bayerPattern;
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
