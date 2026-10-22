// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// FITS keyword cards and view ids for the windows Planetary processes open
// (SERViewer's frame, PlanetaryStack's stack). The text rules themselves are
// PCL-free and unit tested in FitsText.h.

#ifndef __FitsCards_h
#define __FitsCards_h

#include <pcl/FITSHeaderKeyword.h>
#include <pcl/File.h>
#include <pcl/String.h>

#include <string>

#include "FitsText.h"

namespace pcl
{
namespace planetary_fits
{

// A FITS string value: printable ASCII only (header bytes are not guaranteed
// to be), embedded quotes doubled, enclosed in quotes.
inline IsoString FitsString( const std::string& s )
{
   IsoString v = "'";
   for ( char c : planetary::fits::AsciiFromBytes( s ) )
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
inline void AddHistory( FITSKeywordArray& keywords, const std::string& text )
{
   for ( const std::string& piece : planetary::fits::SplitCardText( text ) )
      keywords << FITSHeaderKeyword( "HISTORY", IsoString(), IsoString( piece.c_str() ) );
}

// UTF-8 text bound for a FITS card: the reader's arrow becomes "->", any
// other code point outside printable ASCII one '?'.
inline std::string FitsAscii( std::string s )
{
   const std::string arrow = "\xE2\x86\x92";
   for ( size_t at = s.find( arrow ); at != std::string::npos; at = s.find( arrow, at + 2 ) )
      s.replace( at, arrow.size(), "->" );
   return planetary::fits::AsciiFromUtf8( s );
}

// A view id from the file stem: [A-Za-z0-9_] only, never starting with a digit;
// "SER" for a stem with nothing usable. Callers append their own suffix.
inline IsoString ViewIdStem( const String& path )
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
   return id;
}

} // namespace planetary_fits
} // namespace pcl

#endif // __FitsCards_h
