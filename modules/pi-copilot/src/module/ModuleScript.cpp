// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ModuleScript.h"
#include "EvalGuard.h"
#include "PICopilotModule.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Variant.h>

#include <nlohmann/json.hpp>

namespace pcl
{

namespace
{

// JavaScript helpers prepended to every wrapper (all ASCII, all fixed text):
//  pcWell(s):  s with every unpaired surrogate replaced by U+FFFD, so the
//              final JSON.stringify never emits a "\udXXX" escape that no
//              strict JSON parser accepts.
//  pcAscii(s): s with every non-ASCII code unit \u-escaped. The result
//              crosses EvaluateScript as pure ASCII, immune to the engine's
//              narrow-string re-encoding (inc-5 Task 1, finding 1).
//  pcCut(s,n): String( s ) cut to n characters.
const char* const kHelpersJs =
   "function pcCut( s, n ) { s = String( s ); return s.length > n ? s.slice( 0, n ) : s; }"
   " "
   "function pcWell( s ) { s = String( s ); var r = \"\";"
   " for ( var i = 0; i < s.length; ++i ) { var c = s.charCodeAt( i );"
   "  if ( c >= 0xD800 && c <= 0xDBFF && i+1 < s.length && s.charCodeAt( i+1 ) >= 0xDC00 && s.charCodeAt( i+1 ) <= 0xDFFF )"
   "   { r += s.charAt( i ) + s.charAt( i+1 ); ++i; }"
   "  else if ( c >= 0xD800 && c <= 0xDFFF ) r += \"\\uFFFD\";"
   "  else r += s.charAt( i ); }"
   " return r; }"
   " function pcAscii( s ) { var r = \"\";"
   " for ( var i = 0; i < s.length; ++i ) { var c = s.charCodeAt( i );"
   "  r += c < 0x80 ? s.charAt( i ) : \"\\\\u\" + (\"0000\" + c.toString( 16 )).slice( -4 ); }"
   " return r; }";

} // namespace

std::string ScriptLiteral( const String& text )
{
   // U8() turns an unpaired surrogate into U+FFFD, so this never throws on
   // text from a String; dump( ensure_ascii ) escapes ", \, every control
   // character and every non-ASCII character (a non-BMP one as a \u pair).
   return nlohmann::json( U8( text ) ).dump( -1, ' ', true/*ensure_ascii*/ );
}

nlohmann::json EvaluateAsciiJson( const std::string& body )
{
   const std::string src = std::string( "(function(){ " ) + kHelpersJs + " " + body + " })()";
   String r;
   {
      // Held for the call so timer-driven readers (HistoryReader, the journey
      // tracker) defer instead of nesting an EvaluateScript inside this one.
      EvalDepthGuard guard;
      r = ThePICopilotModule->EvaluateScript( String( src.c_str() ), "JavaScript" ).ToString();
   }
   return nlohmann::json::parse( U8( r ) );
}

} // namespace pcl
