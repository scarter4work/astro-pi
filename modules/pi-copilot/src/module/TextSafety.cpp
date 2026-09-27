// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "TextSafety.h"

namespace pcl
{

namespace
{

struct CpRange { uint32 first, last; };
const CpRange kFormatChars[] =
{
   { 0x00AD, 0x00AD }, { 0x0600, 0x0605 }, { 0x061C, 0x061C }, { 0x06DD, 0x06DD }, { 0x070F, 0x070F },
   { 0x0890, 0x0891 }, { 0x08E2, 0x08E2 }, { 0x180E, 0x180E }, { 0x200B, 0x200F }, { 0x202A, 0x202E },
   { 0x2060, 0x2064 }, { 0x2066, 0x206F }, { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB }, { 0x110BD, 0x110BD },
   { 0x110CD, 0x110CD }, { 0x13430, 0x1343F }, { 0x1BCA0, 0x1BCA3 }, { 0x1D173, 0x1D17A }, { 0xE0001, 0xE0001 },
   { 0xE0020, 0xE007F }
};

struct CpName { uint32 c; const char* name; };
const CpName kNames[] =
{
   { 0x0009, "CHARACTER TABULATION" }, { 0x000A, "LINE FEED" }, { 0x000D, "CARRIAGE RETURN" },
   { 0x001B, "ESCAPE" }, { 0x007F, "DELETE" }, { 0x00A0, "NO-BREAK SPACE" }, { 0x00AD, "SOFT HYPHEN" },
   { 0x061C, "ARABIC LETTER MARK" }, { 0x200B, "ZERO WIDTH SPACE" }, { 0x200C, "ZERO WIDTH NON-JOINER" },
   { 0x200D, "ZERO WIDTH JOINER" }, { 0x200E, "LEFT-TO-RIGHT MARK" }, { 0x200F, "RIGHT-TO-LEFT MARK" },
   { 0x2028, "LINE SEPARATOR" }, { 0x2029, "PARAGRAPH SEPARATOR" }, { 0x202A, "LEFT-TO-RIGHT EMBEDDING" },
   { 0x202B, "RIGHT-TO-LEFT EMBEDDING" }, { 0x202C, "POP DIRECTIONAL FORMATTING" },
   { 0x202D, "LEFT-TO-RIGHT OVERRIDE" }, { 0x202E, "RIGHT-TO-LEFT OVERRIDE" }, { 0x2060, "WORD JOINER" },
   { 0x2066, "LEFT-TO-RIGHT ISOLATE" }, { 0x2067, "RIGHT-TO-LEFT ISOLATE" }, { 0x2068, "FIRST STRONG ISOLATE" },
   { 0x2069, "POP DIRECTIONAL ISOLATE" }, { 0x3000, "IDEOGRAPHIC SPACE" }, { 0xFEFF, "ZERO WIDTH NO-BREAK SPACE" },
};

bool IsNonAsciiSpace( uint32 c )
{
   return c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}

} // namespace

bool IsFormatChar( uint32 c )
{
   for ( const CpRange& r : kFormatChars )
      if ( c >= r.first && c <= r.last )
         return true;
   return false;
}

const char* UnsafeDisplayKind( uint32 c )
{
   if ( c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) )
      return "a control character";
   if ( c == 0x2028 || c == 0x2029 )
      return "a line break character";
   if ( (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F || c == 0x061C )
      return "a bidirectional text control";
   if ( c >= 0xD800 && c <= 0xDFFF )
      return "an unpaired surrogate";
   if ( IsFormatChar( c ) )
      return "an invisible format character";
   if ( IsNonAsciiSpace( c ) )
      return "a non-ASCII space";
   if ( c == 0x115F || c == 0x1160 || c == 0x3164 || c == 0xFFA0 )
      return "an invisible Hangul filler";
   if ( (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) )
      return "an invisible variation selector";
   if ( c == 0x034F || c == 0x17B4 || c == 0x17B5 )
      return "an invisible combining character";
   return nullptr;
}

const char* UnsafeDisplayName( uint32 c )
{
   for ( const CpName& n : kNames )
      if ( n.c == c )
         return n.name;
   return nullptr;
}

uint32 CodePointAt( const String& s, size_type i, size_type& units )
{
   units = 1;
   uint32 c = s[i];
   if ( c >= 0xD800 && c <= 0xDBFF && i+1 < s.Length() && s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF )
   {
      c = 0x10000 + ((c - 0xD800) << 10) + (uint32( s[i+1] ) - 0xDC00);
      units = 2;
   }
   return c;
}

String DisplayCharacter( uint32 c )
{
   if ( const char* kind = UnsafeDisplayKind( c ) )
   {
      String out = String().Format( "U+%04X", unsigned( c ) );
      if ( const char* name = UnsafeDisplayName( c ) )
         out += ' ' + String( name );
      return out + " (" + String( kind ) + ')';
   }
   String ch;
   if ( c >= 0x10000 )
   {
      ch += char16_type( 0xD800 + ((c - 0x10000) >> 10) );
      ch += char16_type( 0xDC00 + ((c - 0x10000) & 0x3FF) );
   }
   else
      ch += char16_type( c );
   return '\'' + ch + '\'';
}

String DisplayText( const String& s, size_type maxChars )
{
   String out = "\"";
   size_type shown = 0, total = 0;
   for ( size_type i = 0; i < s.Length(); ++total )
   {
      size_type units = 1;
      const uint32 c = CodePointAt( s, i, units );
      if ( shown < maxChars )
      {
         if ( UnsafeDisplayKind( c ) != nullptr )
            out += String().Format( "<U+%04X>", unsigned( c ) );
         else
            out += s.Substring( i, units );
         ++shown;
      }
      i += units;
   }
   out += '"';
   if ( total > shown )
      out += String::UTF8ToUTF16( "\xE2\x80\xA6" ) + String().Format( " (%u characters)", unsigned( total ) );
   return out;
}

} // namespace pcl
