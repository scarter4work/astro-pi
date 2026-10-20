// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// SERViewer info-block text. PCL-free and header-only, so it is unit tested
// without PixInsight (test/unit/test_preview.cpp).

#ifndef __SerInfoText_h
#define __SerInfoText_h

#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <string>

#include "SerFile.h"
#include "SerTime.h"

namespace planetary::preview
{

// One "Label: value" line per field, UTF-8, no trailing newline. The first
// frame's time comes from the timestamp trailer (nullopt: the file has none);
// the header's DateTime_UTC is shown on its own line, since an opened frame
// falls back to it for DATE-OBS. Every reader warning gets its own line.
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

   std::string t = "File: " + fileNameUtf8 + "\n";
   t += "Size: " + std::to_string( info.header.width ) + " \xc3\x97 " + std::to_string( info.header.height ) + "\n";
   t += "Frames: " + std::to_string( info.frameCount ) + "\n";
   t += std::string( "Colour: " ) + ser::ColorIdName( info.colorId ) + "\n";
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
   t += "First frame UTC: " + time( firstFrameTicks ) + "\n";
   t += "Header UTC: " + time( info.header.dateTimeUtc ) + "\n";
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
