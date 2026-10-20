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
#include <pcl/View.h>

#include <optional>
#include <string>

#include "FitsText.h"
#include "PlanetaryVersion.h"
#include "SerTime.h"

namespace ser = planetary::ser;
namespace fits = planetary::fits;

namespace pcl
{

DisplayFrame LoadFrameForDisplay( const ser::SerFile& file, int64_t index, bool debayer )
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

} // namespace

SERViewerInstance::SERViewerInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_filePath( TheSVFilePathParameter->DefaultValue() )
   , p_frameIndex( int32( TheSVFrameIndexParameter->DefaultValue() ) )
   , p_debayer( TheSVDebayerParameter->DefaultValue() )
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
   }
}

bool SERViewerInstance::CanExecuteOn( const View&, String& whyNot ) const
{
   whyNot = "SERViewer is a global process: it opens a SER frame as a new image and cannot run on a view.";
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
   const int64_t index = p_frameIndex;

   // Every reader failure (bad file, unsupported layout, index out of range,
   // short read) surfaces with the library's own reason. No window exists until
   // the frame has been read, so a reader failure leaves no half-made image.
   ser::Frame frame;
   ser::FileInfo info;
   std::optional<std::string> dateObs;
   bool dateFromFrame = false;
   try
   {
      const ser::SerFile file = ser::SerFile::Open( path.ToUTF8().c_str() );
      info = file.Info();
      frame = file.ReadFrame( index );
      if ( std::optional<int64_t> ticks = file.FrameTimestamp( index ) )
         dateObs = ser::TicksToIsoUtc( *ticks );
      dateFromFrame = bool( dateObs );
      if ( !dateObs )
         dateObs = ser::TicksToIsoUtc( info.header.dateTimeUtc );
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
      keywords << FITSHeaderKeyword( "INSTRUME", FitsString( info.header.instrument ), "SER header Instrument" );
   if ( !info.header.telescope.empty() )
      keywords << FITSHeaderKeyword( "TELESCOP", FitsString( info.header.telescope ), "SER header Telescope" );
   if ( !info.header.observer.empty() )
      keywords << FITSHeaderKeyword( "OBSERVER", FitsString( info.header.observer ), "SER header Observer" );
   if ( cfaKept )
      keywords << FITSHeaderKeyword( "BAYERPAT", FitsString( ser::BayerPatternKeyword( info.colorId ) ),
                                     "CFA pattern of the raw frame" );
   const std::string asciiName = fits::AsciiFromUtf8( fileName.ToUTF8().c_str() );
   AddHistory( keywords, "SERViewer: " + asciiName + " frame " + std::to_string( index ) );
   for ( const std::string& w : info.warnings )
      AddHistory( keywords, "SERViewer warning: " + fits::AsciiFromBytes( w ) );
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
   if ( p == TheSVFilePathParameter )   return p_filePath.Begin();
   if ( p == TheSVFrameIndexParameter ) return &p_frameIndex;
   if ( p == TheSVDebayerParameter )    return &p_debayer;
   return nullptr;
}

bool SERViewerInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type )
{
   if ( p != TheSVFilePathParameter )
      return false;
   p_filePath.Clear();
   if ( sizeOrLength > 0 )
      p_filePath.SetLength( sizeOrLength );
   return true;
}

size_type SERViewerInstance::ParameterLength( const MetaParameter* p, size_type ) const
{
   if ( p == TheSVFilePathParameter )
      return p_filePath.Length();
   return 0;
}

} // namespace pcl
