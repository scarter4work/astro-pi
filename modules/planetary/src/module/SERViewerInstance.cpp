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
#include <pcl/StandardStatus.h>
#include <pcl/StatusMonitor.h>
#include <pcl/View.h>

#include <memory>
#include <optional>
#include <string>

#include "FitsText.h"
#include "PlanetaryVersion.h"
#include "SerExport.h"
#include "SerFile.h"
#include "SerTime.h"

namespace ser = planetary::ser;
namespace fits = planetary::fits;

namespace pcl
{

ser::BayerChoice BayerChoiceFor( pcl_enum bayerPattern )
{
   switch ( bayerPattern )
   {
   case SVBayerPattern::None: return ser::BayerChoice::None;
   case SVBayerPattern::RGGB: return ser::BayerChoice::RGGB;
   case SVBayerPattern::GRBG: return ser::BayerChoice::GRBG;
   case SVBayerPattern::GBRG: return ser::BayerChoice::GBRG;
   case SVBayerPattern::BGGR: return ser::BayerChoice::BGGR;
   case SVBayerPattern::Auto: return ser::BayerChoice::Auto;
   default:
      throw Error( "SERViewer: invalid bayerPattern value " + String( int( bayerPattern ) ) );
   }
}

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

// A view id from the file stem: [A-Za-z0-9_] only, never starting with a digit.
IsoString WindowIdFor( const String& path, int64_t frameIndex )
{
   const String stem = File::ExtractName( path );
   IsoString id;
   for ( size_type i = 0; i < stem.Length(); ++i )
   {
      const char16_type c = stem[i];
      const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
      id += ok ? char( c ) : '_';
   }
   if ( id.IsEmpty() )
      id = "SER";
   else if ( id[0] >= '0' && id[0] <= '9' )
      id.Prepend( '_' );
   return id + "_f" + IsoString( std::to_string( frameIndex ).c_str() );
}

// A FITS string value: printable ASCII only (header bytes are not guaranteed
// to be), embedded quotes doubled, enclosed in quotes.
IsoString FitsString( const std::string& s )
{
   IsoString v = "'";
   for ( char c : fits::AsciiFromBytes( s ) )
   {
      if ( c == '\'' )
         v += "''";
      else
         v += c;
   }
   v += '\'';
   return v;
}

// HISTORY text (ASCII already) on as many cards as it needs: a FITS card holds
// 72 characters of it. Pieces break at spaces; readers join them with one space.
void AddHistory( FITSKeywordArray& keywords, const std::string& text )
{
   for ( const std::string& piece : fits::SplitCardText( text ) )
      keywords << FITSHeaderKeyword( "HISTORY", IsoString(), IsoString( piece.c_str() ) );
}

// "BGGR (sidecar RG, Flip=Vert → BGGR)" / "none (none)": an AVI's effective
// Bayer pattern and where it came from (UTF-8).
std::string AviBayerText( const ser::FileInfo& info )
{
   const std::string pattern = ser::IsBayer( info.colorId ) ? ser::BayerPatternKeyword( info.colorId ) : "none";
   return pattern + " (" + (info.avi ? info.avi->bayerSource : std::string()) + ")";
}

// UTF-8 text bound for a FITS card: the reader's arrow becomes "->", any
// other code point outside printable ASCII one '?'.
std::string FitsAscii( std::string s )
{
   const std::string arrow = "\xE2\x86\x92";
   for ( size_t at = s.find( arrow ); at != std::string::npos; at = s.find( arrow, at + 2 ) )
      s.replace( at, arrow.size(), "->" );
   return fits::AsciiFromUtf8( s );
}

// saveSerPath: writes the AVI `source` as a SER at `savePath`, with progress on
// the console (its Abort cancels; the partial file is removed). Opens no window.
bool ConvertToSer( const ser::FrameSource& source, const String& path, const String& savePath )
{
   const String fileName = File::ExtractNameAndExtension( path );
   const ser::FileInfo& info = source.Info();
   if ( info.container == ser::Container::Ser )
      throw Error( "SERViewer: " + fileName + ": saveSerPath converts AVI input only" );

   Console console;
   console.EnableAbort();
   StandardStatus status;
   StatusMonitor monitor;
   monitor.SetCallback( &status );
   monitor.Initialize( "SERViewer: writing " + File::ExtractNameAndExtension( savePath ).EncodedHTMLSpecialChars(),
                       size_type( info.frameCount ) );

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
                               false/*allowOverwrite*/, progress );
   }
   catch ( const ser::ExportCancelled& )
   {
      throw ProcessAborted();
   }
   catch ( const ser::SerError& e )
   {
      throw Error( "SERViewer: " + fileName + ": " + FromUtf8( e.what() ) );
   }

   console.WriteLn( "<end><cbr>SERViewer " PLANETARY_VERSION_STRING ": " + fileName.EncodedHTMLSpecialChars() +
                    ", Bayer " + FromUtf8( AviBayerText( info ) ).EncodedHTMLSpecialChars() );
   for ( const std::string& w : info.warnings )
      console.WarningLn( "SERViewer warning: " + FromUtf8( w ).EncodedHTMLSpecialChars() );
   console.WriteLn( "SERViewer: wrote " + Num( result.framesWritten ) + " frames to " +
                    savePath.EncodedHTMLSpecialChars() );
   return true;
}

} // namespace

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
      whyNot = "SERViewer: no SER file selected (filePath is empty).";
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
   const ser::BayerChoice bayer = BayerChoiceFor( p_bayerPattern );

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

   if ( !savePath.IsEmpty() )
      return ConvertToSer( *source, path, savePath );

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
      AddHistory( keywords, "SERViewer: Bayer " + FitsAscii( AviBayerText( info ) ) );
   // SER header text has no defined encoding; AVI warnings are built from
   // UTF-8 file names and sidecar text.
   for ( const std::string& w : info.warnings )
      AddHistory( keywords, "SERViewer warning: " + (isAvi ? FitsAscii( w ) : fits::AsciiFromBytes( w )) );
   if ( !rangeWarning.empty() )
      AddHistory( keywords, "SERViewer warning: " + rangeWarning );

   ImageWindow window( frame.width, frame.height, channels, 32, true/*float*/, channels == 3, true/*initialProcessing*/,
                       WindowIdFor( path, index ) );
   if ( window.IsNull() )
      throw Error( "SERViewer: " + fileName + ": PixInsight could not create an image window" );

   IsoString viewId;
   try
   {
      View view = window.MainView();
      ImageVariant v = view.Image();
      if ( !v.IsFloatSample() || v.BitsPerSample() != 32 || v.NumberOfChannels() != channels )
         throw Error( "SERViewer: internal error - the new window is not a 32-bit float image" );
      Image& image = static_cast<Image&>( *v );
      // Normalised (and debayered) straight into the image's planes: no
      // intermediate float copy of the frame next to the image.
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
      window.SetKeywords( keywords );
      viewId = view.Id();
   }
   catch ( ... )
   {
      // The window is still hidden: drop it rather than leave a half-made image.
      window.ForceClose();
      throw;
   }

   window.Show();

   // Console text is parsed for tags, so file names and warnings go in encoded.
   Console console;
   const String depth = Num( info.header.depth ) + "-bit" +
      (info.normDepth != info.header.depth ? ", normalised as " + Num( info.normDepth ) + "-bit" : String());
   console.WriteLn( "<end><cbr>SERViewer " PLANETARY_VERSION_STRING ": " + fileName.EncodedHTMLSpecialChars() +
                    " frame " + Num( index ) + " of " + Num( info.frameCount ) + " -> " + String( viewId ) + " (" +
                    Num( frame.width ) + "x" + Num( frame.height ) + ", " +
                    String( ser::ColorIdName( info.colorId ) ) + (cfaKept ? " raw CFA" : "") + ", " + depth + ")" );
   if ( isAvi )
      console.WriteLn( "SERViewer: Bayer " + FromUtf8( AviBayerText( info ) ).EncodedHTMLSpecialChars() );
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
