// Minimal JSON text helpers for the TEST-ONLY probe process.
#ifndef __GDG_ProbeJson_h
#define __GDG_ProbeJson_h

#include <pcl/String.h>

#include <cmath>
#include <cstdio>

namespace pcl
{
namespace gdgprobe
{

inline IsoString Str( const String& s )
{
   IsoString u = s.ToUTF8(), o = "\"";
   for ( char c : u )
   {
      if ( c == '"' || c == '\\' ) { o += '\\'; o += c; }
      else if ( c == '\n' ) o += "\\n";
      else if ( c == '\t' ) o += "\\t";
      else if ( static_cast<unsigned char>( c ) < 0x20 ) o += ' ';
      else o += c;
   }
   return o + "\"";
}

inline IsoString Num( double v )
{
   if ( !std::isfinite( v ) )
      return "null";
   char b[ 64 ];
   std::snprintf( b, sizeof( b ), "%.17g", v );
   return IsoString( b );
}

inline IsoString Field( const char* name, const IsoString& value )
{
   return "\"" + IsoString( name ) + "\":" + value;
}

} // namespace gdgprobe
} // namespace pcl

#endif // __GDG_ProbeJson_h
