// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// FITS keyword text helpers. PCL-free and header-only, so they are unit
// tested without PixInsight (test/unit/test_fits_text.cpp).

#ifndef __FitsText_h
#define __FitsText_h

#include <string>
#include <vector>

namespace planetary::fits
{

// Text room on one HISTORY/COMMENT card: 80 columns minus the 8-column name.
constexpr size_t kCardTextWidth = 72;

inline bool IsPrintableAscii( unsigned char c )
{
   return c >= 0x20 && c <= 0x7e;
}

// Header text of unknown encoding: every byte outside printable ASCII -> '?'.
inline std::string AsciiFromBytes( const std::string& s )
{
   std::string out;
   for ( unsigned char c : s )
      out += IsPrintableAscii( c ) ? char( c ) : '?';
   return out;
}

// UTF-8 text (a file name): every code point outside printable ASCII -> one '?'.
inline std::string AsciiFromUtf8( const std::string& s )
{
   std::string out;
   for ( unsigned char c : s )
   {
      if ( (c & 0xC0) == 0x80 )
         continue; // continuation byte of a code point already replaced
      out += IsPrintableAscii( c ) ? char( c ) : '?';
   }
   return out;
}

// Splits text into card-sized pieces, breaking at spaces (the break space is
// dropped, so joining the pieces with single spaces restores the text). A word
// longer than a card is cut at the card width.
inline std::vector<std::string> SplitCardText( const std::string& text, size_t width = kCardTextWidth )
{
   std::vector<std::string> parts;
   size_t pos = 0;
   while ( pos < text.size() )
   {
      while ( pos < text.size() && text[pos] == ' ' )
         ++pos;
      if ( pos >= text.size() )
         break;
      if ( text.size() - pos <= width )
      {
         parts.push_back( text.substr( pos ) );
         break;
      }
      size_t cut = text.rfind( ' ', pos + width );
      if ( cut == std::string::npos || cut <= pos )
         cut = pos + width;
      size_t end = cut;
      while ( end > pos && text[end - 1] == ' ' )
         --end;
      parts.push_back( text.substr( pos, end - pos ) );
      pos = cut;
   }
   return parts;
}

} // namespace planetary::fits

#endif // __FitsText_h
