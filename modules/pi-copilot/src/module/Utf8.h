// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_Utf8_h
#define PICopilot_Utf8_h

#include <pcl/String.h>

#include <string>

namespace pcl
{

// UTF-16 pcl::String -> UTF-8 std::string: the ONE conversion for every piece
// of text that goes into a request body (message content, system prompt,
// view context, process catalog), so it is defined exactly once.
//
// Deliberately NOT String::ToUTF8(): PCL's String::UTF16ToUTF8()
// (src/pcl/String.cpp:314 in PCL 2.9.x) builds the last byte of a 4-byte
// sequence from the HIGH surrogate instead of the combined code point, so
// every non-BMP character is silently re-encoded as a different one
// (U+1F4F7 -> U+1F4FD). It also encodes an unpaired surrogate as ED A0..BF,
// which nlohmann::json::dump() then rejects. Here a well-formed pair becomes
// its 4-byte sequence and an unpaired surrogate becomes U+FFFD, so the result
// is always strict UTF-8.
inline std::string U8( const String& s )
{
   std::string r;
   r.reserve( s.Length() );
   const size_type n = s.Length();
   const char16_type* p = s.c_str();
   for ( size_type i = 0; i < n; ++i )
   {
      uint32 c = p[i];
      if ( c >= 0xD800 && c <= 0xDBFF && i+1 < n && p[i+1] >= 0xDC00 && p[i+1] <= 0xDFFF )
         c = 0x10000 + ((c - 0xD800) << 10) + (uint32( p[++i] ) - 0xDC00);
      else if ( c >= 0xD800 && c <= 0xDFFF )
         c = 0xFFFD;                                   // unpaired surrogate
      if ( c < 0x80 )
         r += char( c );
      else if ( c < 0x800 )
      {
         r += char( 0xC0 | (c >> 6) );
         r += char( 0x80 | (c & 0x3F) );
      }
      else if ( c < 0x10000 )
      {
         r += char( 0xE0 | (c >> 12) );
         r += char( 0x80 | ((c >> 6) & 0x3F) );
         r += char( 0x80 | (c & 0x3F) );
      }
      else
      {
         r += char( 0xF0 | (c >> 18) );
         r += char( 0x80 | ((c >> 12) & 0x3F) );
         r += char( 0x80 | ((c >> 6) & 0x3F) );
         r += char( 0x80 | (c & 0x3F) );
      }
   }
   return r;
}

} // namespace pcl

#endif // PICopilot_Utf8_h
