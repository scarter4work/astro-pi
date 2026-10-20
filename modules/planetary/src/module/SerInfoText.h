// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// The SERViewer info block. PCL-free and header-only, so it is unit tested
// without PixInsight (test/unit/test_preview.cpp).

#ifndef __SerInfoText_h
#define __SerInfoText_h

#include <cstdint>
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
   t += "Bit depth: " + std::to_string( info.header.depth ) + "\n";
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

} // namespace planetary::preview

#endif // __SerInfoText_h
