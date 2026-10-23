// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// SERViewer info-block text. PCL-free and header-only, so it is unit tested
// without PixInsight (test/unit/test_preview.cpp).

#ifndef __SerInfoText_h
#define __SerInfoText_h

#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#include <optional>
#include <string>

#include "AsiairSidecar.h"
#include "SerFile.h"
#include "SerTime.h"

namespace planetary::preview
{

// A reader warning reworded for Save as SER: ResolveBayer's "<reason>; showing the
// raw CFA as mono -- choose..." says what is written instead; any other warning
// is returned as is.
inline std::string ConversionWarning( const std::string& warning )
{
   const std::string tail = ser::kRawCfaAsMonoTail;
   if ( warning.size() <= tail.size() || warning.compare( warning.size() - tail.size(), tail.size(), tail ) != 0 )
      return warning;
   return warning.substr( 0, warning.size() - tail.size() ) +
          "; the SER will be written as MONO (raw CFA) -- set the Bayer pattern to write a Bayer SER";
}

// An AVI's effective Bayer pattern and where it came from, for people:
// "RGGB (sidecar)", "RGGB (chosen)", "none (chosen)". The
// reader's "none" token (Auto found no pattern) becomes the reason: a sidecar
// Bayer value it did not recognise, a sidecar that could not be read, or no
// pattern to go on at all. The reader's "sidecar (SERViewer)" (a sidecar that
// Save pattern... wrote) becomes "sidecar written by SERViewer". UTF-8.
inline std::string BayerText( const ser::FileInfo& info )
{
   const std::string pattern = ser::IsBayer( info.colorId ) ? ser::BayerPatternKeyword( info.colorId ) : "none";
   std::string source = info.avi ? info.avi->bayerSource : std::string();
   if ( source == "sidecar (SERViewer)" )
      source = "sidecar written by SERViewer";
   else if ( source == "none" )
   {
      const ser::SidecarStatus status = info.avi->sidecarStatus;
      if ( info.avi->sidecar && !info.avi->sidecar->bayer.empty() )
         source = "sidecar value not recognised";
      else if ( status == ser::SidecarStatus::Unreadable || status == ser::SidecarStatus::TooLarge )
         source = "sidecar could not be read; choose one under Bayer pattern";
      else if ( status == ser::SidecarStatus::NotAFile )
         source = "sidecar path is not a file; choose one under Bayer pattern";
      else
         source = "no sidecar pattern; choose one under Bayer pattern";
   }
   return pattern + " (" + source + ")";
}

// One "Label: value" line per field, UTF-8, no trailing newline. For a SER the
// first frame's time comes from the timestamp trailer (nullopt: the file has
// none); the header's DateTime_UTC is shown on its own line, since an opened
// frame falls back to it for DATE-OBS. An AVI has no clock: instead it shows its
// container, frame rate, Bayer pattern and where that came from, the ASIAIR
// sidecar's fields, and the file name's start time (local, zone unknown).
// Every reader warning gets its own line.
inline std::string InfoText( const std::string& fileNameUtf8, const ser::FileInfo& info,
                             std::optional<int64_t> firstFrameTicks )
{
   auto time = []( std::optional<int64_t> ticks ) -> std::string
   {
      std::optional<std::string> iso;
      if ( ticks )
         iso = ser::TicksToIsoUtc( *ticks );
      return iso ? *iso : std::string( "none" );
   };

   const bool isAvi = info.container != ser::Container::Ser;
   const ser::AviDetails avi = info.avi.value_or( ser::AviDetails() );

   std::string t = "File: " + fileNameUtf8 + "\n";
   if ( isAvi )
      t += std::string( "Container: " ) +
           (info.container == ser::Container::AviOpenDml ? "AVI (OpenDML)" : "AVI") + "\n";
   t += "Size: " + std::to_string( info.header.width ) + " \xc3\x97 " + std::to_string( info.header.height ) + "\n";
   t += "Frames: " + std::to_string( info.frameCount ) + "\n";
   if ( isAvi )
   {
      char fps[64];
      std::snprintf( fps, sizeof fps, "%.2f", avi.frameRate );
      t += std::string( "Frame rate: " ) + fps + " fps\n";
   }
   t += std::string( "Colour: " ) + ser::ColorIdName( info.colorId ) + "\n";
   if ( isAvi )
      t += "Bayer: " + BayerText( info ) + "\n";
   t += "Bit depth: " + std::to_string( info.header.depth );
   if ( info.normDepth != info.header.depth )
      t += " (normalised to the full " + std::to_string( info.normDepth ) + "-bit range)";
   t += "\n";
   t += "Byte order: ";
   if ( info.bytesPerSample == 1 )
      t += "n/a (8-bit samples)";
   else
      t += info.byteOrder == ser::ByteOrder::Big ? "big-endian" : "little-endian";
   t += "\n";
   if ( isAvi )
   {
      auto field = [&t]( const char* label, const std::string& value )
      {
         if ( !value.empty() )
            t += std::string( label ) + ": " + value + "\n";
      };
      if ( avi.sidecar )
      {
         field( "Camera", avi.sidecar->camera );
         field( "Gain", avi.sidecar->gain );
         field( "Exposure", avi.sidecar->exposure );
         field( "Flip (sidecar; image not flipped)", avi.sidecar->flip );
      }
      if ( avi.filenameStart )
         t += "Start: " + *avi.filenameStart + " (from filename, time zone unknown)\n";
   }
   else
   {
      t += "First frame UTC: " + time( firstFrameTicks ) + "\n";
      t += "Header UTC: " + time( info.header.dateTimeUtc ) + "\n";
   }
   if ( info.warnings.empty() )
      t += "Warnings: none";
   else
      for ( size_t i = 0; i < info.warnings.size(); ++i )
         t += (i > 0 ? "\n" : "") + std::string( "Warning: " ) + info.warnings[i];
   return t;
}

// The info block for one shown frame: the file's InfoText, plus a line for
// the frame's own range warning (ser::FrameRangeWarning) when it has one.
inline std::string FrameInfoText( const std::string& fileInfo, const std::string& frameWarning )
{
   if ( frameWarning.empty() )
      return fileInfo;
   return fileInfo + "\nWarning: " + frameWarning;
}

// Text for pcl::TextBox::SetPlainText, which wraps it in <raw>...</raw>: the
// '<' of anything the console parser would read as a closing raw tag
// ("</raw", any case, whitespace allowed around the '/') closes the block,
// is emitted as "&lt;" and the block reopens, so the text shows verbatim.
inline std::string RawSafe( const std::string& text )
{
   auto space = []( char c ) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
   auto lower = []( char c ) { return c >= 'A' && c <= 'Z' ? char( c - 'A' + 'a' ) : c; };
   std::string out;
   const size_t n = text.size();
   for ( size_t i = 0; i < n; ++i )
   {
      if ( text[i] == '<' )
      {
         size_t j = i + 1;
         while ( j < n && space( text[j] ) )
            ++j;
         if ( j < n && text[j] == '/' )
         {
            ++j;
            while ( j < n && space( text[j] ) )
               ++j;
            if ( j + 3 <= n && lower( text[j] ) == 'r' && lower( text[j+1] ) == 'a' && lower( text[j+2] ) == 'w' )
            {
               out += "</raw>&lt;<raw>";
               continue;
            }
         }
      }
      out += text[i];
   }
   return out;
}

// The message of an exception caught with catch ( ... ): the reader's own
// reason for a SerError, otherwise what kind of failure it was.
inline std::string DescribeException( std::exception_ptr p )
{
   try
   {
      std::rethrow_exception( p );
   }
   catch ( const ser::SerError& e )
   {
      return e.what();
   }
   catch ( const std::bad_alloc& )
   {
      return "out of memory";
   }
   catch ( const std::exception& e )
   {
      return std::string( "unexpected error: " ) + e.what();
   }
   catch ( ... )
   {
      return "unknown error";
   }
}

} // namespace planetary::preview

#endif // __SerInfoText_h
