// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SERViewerInstance.h"
#include "SERViewerParameters.h"

#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/File.h>
#include <pcl/Image.h>
#include <pcl/ImageWindow.h>
#include <pcl/ProgressBarStatus.h>
#include <pcl/StandardStatus.h>
#include <pcl/StatusMonitor.h>
#include <pcl/View.h>

#include <memory>
#include <optional>
#include <string>

#include "FitsCards.h"
#include "FitsText.h"
#include "NewImageWindow.h"
#include "PlanetaryVersion.h"
#include "SerExport.h"
#include "SerFile.h"
#include "SerInfoText.h"
#include "SerTime.h"

namespace ser = planetary::ser;
namespace fits = planetary::fits;
namespace preview = planetary::preview;
using namespace pcl::planetary_fits;

namespace pcl
{

DisplayFrame LoadFrameForDisplay( const ser::FrameSource& file, int64_t index, bool debayer )
{
   const ser::FileInfo& info = file.Info();
   const ser::Frame frame = file.ReadFrame( index );
   DisplayFrame shown;
   shown.rangeWarning = ser::FrameRangeWarning( frame, index, info.normDepth );
   if ( debayer && ser::IsBayer( info.colorId ) )
      shown.image = ser::DebayerBilinear( frame, info.colorId, info.normDepth );
   else
      shown.image = ser::Normalize( frame, info.normDepth );
   return shown;
}

namespace
{

String FromUtf8( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

String Num( int64_t n )
{
   return String( std::to_string( n ).c_str() );
}

} // namespace

ser::ExportResult ConvertToSer( const ser::FrameSource& source, const String& path, const String& savePath,
                                bool allowOverwrite, SaveProgress progressIn )
{
   const String fileName = File::ExtractNameAndExtension( path );
   const ser::FileInfo& info = source.Info();
   if ( info.container == ser::Container::Ser )
      throw Error( "SERViewer: " + fileName + ": saveSerPath converts AVI input only" );

   // What is about to be written goes on the console before the export starts.
   // Auto with no pattern writes the raw CFA as MONO: the reader's warning is
   // worded for viewing, so ConversionWarning rewords it to say what is written.
   const String layout = ser::ColorIdName( info.colorId );
   Console console;
   console.WriteLn( "<end><cbr>SERViewer " PLANETARY_VERSION_STRING ": converting " + fileName.EncodedHTMLSpecialChars() +
                    " to SER; Bayer " + FromUtf8( preview::BayerText( info ) ).EncodedHTMLSpecialChars() );
   for ( const std::string& w : info.warnings )
      console.WarningLn( "SERViewer warning: " + FromUtf8( preview::ConversionWarning( w ) ).EncodedHTMLSpecialChars() );

   const String saveName = File::ExtractNameAndExtension( savePath );
   std::unique_ptr<StatusCallback> status;
   String what;
   if ( progressIn == SaveProgress::Console )
   {
      console.EnableAbort();
      status = std::make_unique<StandardStatus>();
      what = "SERViewer: writing " + saveName.EncodedHTMLSpecialChars(); // console text is parsed for tags
   }
   else
   {
      status = std::make_unique<ProgressBarStatus>( "SERViewer: Save as SER" );
      what = "Writing " + saveName;
   }
   StatusMonitor monitor;
   monitor.SetCallback( status.get() );
   monitor.Initialize( what, size_type( info.frameCount ) );

   // StatusMonitor reports an abort by throwing ProcessAborted; ExportSer is
   // told by returning false, so it removes its partial file and throws
   // ExportCancelled, which leaves here as ProcessAborted again.
   const ser::ExportProgress progress = [&monitor]( int64_t done, int64_t ) -> bool
   {
      if ( done == 0 )
         return true;
      try
      {
         ++monitor;
      }
      catch ( const ProcessAborted& )
      {
         return false;
      }
      return true;
   };

   ser::ExportResult result;
   try
   {
      result = ser::ExportSer( source, path.ToUTF8().c_str(), savePath.ToUTF8().c_str(),
                               allowOverwrite, progress );
   }
   catch ( const ser::ExportCancelled& )
   {
      throw ProcessAborted();
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "SERViewer: " + fileName + ": " + FromUtf8( e.what() ) );
   }

   console.WriteLn( "<end><cbr>SERViewer: wrote " + Num( result.framesWritten ) + " frames (" + layout + ") to " +
                    savePath.EncodedHTMLSpecialChars() );
   return result;
}

SERViewerInstance::SERViewerInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_filePath( TheSVFilePathParameter->DefaultValue() )
   , p_frameIndex( int32( TheSVFrameIndexParameter->DefaultValue() ) )
   , p_debayer( TheSVDebayerParameter->DefaultValue() )
   , p_bayerPattern( pcl_enum( TheSVBayerPatternParameter->DefaultValueIndex() ) )
   , p_saveSerPath( TheSVSaveSerPathParameter->DefaultValue() )
{
}

SERViewerInstance::SERViewerInstance( const SERViewerInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

void SERViewerInstance::Assign( const ProcessImplementation& p )
{
   const SERViewerInstance* x = dynamic_cast<const SERViewerInstance*>( &p );
   if ( x != nullptr )
   {
      p_filePath = x->p_filePath;
      p_frameIndex = x->p_frameIndex;
      p_debayer = x->p_debayer;
      p_bayerPattern = x->p_bayerPattern;
      p_saveSerPath = x->p_saveSerPath;
   }
}

bool SERViewerInstance::CanExecuteOn( const View&, String& whyNot ) const
{
   whyNot = "SERViewer is a global process: it opens a video frame as a new image and cannot run on a view.";
   return false;
}

bool SERViewerInstance::CanExecuteGlobal( String& whyNot ) const
{
   if ( p_filePath.Trimmed().IsEmpty() )
   {
      whyNot = "SERViewer: no video file selected (filePath is empty).";
      return false;
   }
   return true;
}

bool SERViewerInstance::ExecuteGlobal()
{
   String whyNot;
   if ( !CanExecuteGlobal( whyNot ) )
      throw Error( whyNot );

   const String path = p_filePath.Trimmed();
   const String fileName = File::ExtractNameAndExtension( path );
   const String savePath = p_saveSerPath.Trimmed();
   const int64_t index = p_frameIndex;
   const ser::BayerChoice bayer = BayerChoiceFor( p_bayerPattern, "SERViewer" );

   // Every reader failure (bad file, unsupported layout, index out of range,
   // short read) surfaces with the library's own reason. No window exists until
   // the frame has been read, so a reader failure leaves no half-made image.
   std::unique_ptr<ser::FrameSource> source;
   try
   {
      source = ser::OpenVideo( path.ToUTF8().c_str(), bayer );
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "SERViewer: " + fileName + ": " + FromUtf8( e.what() ) );
   }

   // A process icon never overwrites: an existing savePath is an error.
   if ( !savePath.IsEmpty() )
   {
      ConvertToSer( *source, path, savePath, false/*allowOverwrite*/, SaveProgress::Console );
      return true;
   }

   ser::Frame frame;
   const ser::FileInfo& info = source->Info();
   const bool isAvi = info.container != ser::Container::Ser;
   // An AVI has no trustworthy clock: no DATE-OBS (the file name's start time
   // is local with an unknown zone).
   std::optional<std::string> dateObs;
   bool dateFromFrame = false;
   try
   {
      frame = source->ReadFrame( index );
      if ( !isAvi )
      {
         if ( std::optional<int64_t> ticks = source->FrameTimestamp( index ) )
            dateObs = ser::TicksToIsoUtc( *ticks );
         dateFromFrame = bool( dateObs );
         if ( !dateObs )
            dateObs = ser::TicksToIsoUtc( info.header.dateTimeUtc );
      }
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "SERViewer: " + fileName + ": " + FromUtf8( e.what() ) );
   }

   const bool debayered = ser::IsBayer( info.colorId ) && p_debayer;
   const bool cfaKept = ser::IsBayer( info.colorId ) && !p_debayer;
   const int channels = debayered ? 3 : frame.channels;
   const std::string rangeWarning = ser::FrameRangeWarning( frame, index, info.normDepth );

   FITSKeywordArray keywords;
   if ( dateObs )
      keywords << FITSHeaderKeyword( "DATE-OBS", FitsString( *dateObs ),
                                     dateFromFrame ? "UTC time of this frame (SER timestamp trailer)"
                                                   : "UTC start of the capture (SER header)" );
   if ( !info.header.instrument.empty() )
      keywords << FITSHeaderKeyword( "INSTRUME", FitsString( info.header.instrument ),
                                     isAvi ? "ASIAIR sidecar camera" : "SER header Instrument" );
   if ( !info.header.telescope.empty() )
      keywords << FITSHeaderKeyword( "TELESCOP", FitsString( info.header.telescope ), "SER header Telescope" );
   if ( !info.header.observer.empty() )
      keywords << FITSHeaderKeyword( "OBSERVER", FitsString( info.header.observer ), "SER header Observer" );
   if ( cfaKept )
      keywords << FITSHeaderKeyword( "BAYERPAT", FitsString( ser::BayerPatternKeyword( info.colorId ) ),
                                     "CFA pattern of the raw frame" );
   const std::string asciiName = fits::AsciiFromUtf8( fileName.ToUTF8().c_str() );
   AddHistory( keywords, "SERViewer: " + asciiName + " frame " + std::to_string( index ) );
   if ( isAvi && ser::IsBayer( info.colorId ) )
      AddHistory( keywords, "SERViewer: Bayer " + fits::FitsAscii( preview::BayerText( info ) ) );
   // SER header text has no defined encoding; AVI warnings are built from
   // UTF-8 file names and sidecar text.
   for ( const std::string& w : info.warnings )
      AddHistory( keywords, "SERViewer warning: " + (isAvi ? fits::FitsAscii( w ) : fits::AsciiFromBytes( w )) );
   if ( !rangeWarning.empty() )
      AddHistory( keywords, "SERViewer warning: " + rangeWarning );

   // The frame, normalised (and debayered), is ready before any window exists:
   // the window only receives a copy of it (ShowNewImageWindow).
   Image image( frame.width, frame.height, channels == 3 ? ColorSpace::RGB : ColorSpace::Gray );
   float* planes[3] = {};
   for ( int c = 0; c < channels; ++c )
      planes[c] = image.PixelData( c );
   try
   {
      if ( debayered )
         ser::DebayerBilinearInto( frame, info.colorId, info.normDepth, planes, 1 );
      else
         ser::NormalizeInto( frame, info.normDepth, planes, 1 );
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "SERViewer: " + fileName + ": " + FromUtf8( e.what() ) );
   }
   const IsoString viewId = ShowNewImageWindow( image, ViewIdStem( path ) + "_f" + IsoString( std::to_string( index ).c_str() ),
                                                keywords, "SERViewer: " + fileName );

   // Console text is parsed for tags, so file names and warnings go in encoded.
   Console console;
   const String depth = Num( info.header.depth ) + "-bit" +
      (info.normDepth != info.header.depth ? ", normalised as " + Num( info.normDepth ) + "-bit" : String());
   console.WriteLn( "<end><cbr>SERViewer " PLANETARY_VERSION_STRING ": " + fileName.EncodedHTMLSpecialChars() +
                    " frame " + Num( index ) + " of " + Num( info.frameCount ) + " -> " + String( viewId ) + " (" +
                    Num( frame.width ) + "x" + Num( frame.height ) + ", " +
                    String( ser::ColorIdName( info.colorId ) ) + (cfaKept ? " raw CFA" : "") + ", " + depth + ")" );
   if ( isAvi )
      console.WriteLn( "SERViewer: Bayer " + FromUtf8( preview::BayerText( info ) ).EncodedHTMLSpecialChars() );
   if ( info.warnings.empty() )
      console.WriteLn( "SERViewer: no reader warnings" );
   else
      for ( const std::string& w : info.warnings )
         console.WarningLn( "SERViewer warning: " + FromUtf8( w ).EncodedHTMLSpecialChars() );
   if ( !rangeWarning.empty() )
      console.WarningLn( "SERViewer warning: " + FromUtf8( rangeWarning ).EncodedHTMLSpecialChars() );

   return true;
}

void* SERViewerInstance::LockParameter( const MetaParameter* p, size_type )
{
   if ( p == TheSVFilePathParameter )     return p_filePath.Begin();
   if ( p == TheSVFrameIndexParameter )   return &p_frameIndex;
   if ( p == TheSVDebayerParameter )      return &p_debayer;
   if ( p == TheSVBayerPatternParameter ) return &p_bayerPattern;
   if ( p == TheSVSaveSerPathParameter )  return p_saveSerPath.Begin();
   return nullptr;
}

bool SERViewerInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type )
{
   String* s = nullptr;
   if ( p == TheSVFilePathParameter )
      s = &p_filePath;
   else if ( p == TheSVSaveSerPathParameter )
      s = &p_saveSerPath;
   else
      return false;
   s->Clear();
   if ( sizeOrLength > 0 )
      s->SetLength( sizeOrLength );
   return true;
}

size_type SERViewerInstance::ParameterLength( const MetaParameter* p, size_type ) const
{
   if ( p == TheSVFilePathParameter )
      return p_filePath.Length();
   if ( p == TheSVSaveSerPathParameter )
      return p_saveSerPath.Length();
   return 0;
}

} // namespace pcl
