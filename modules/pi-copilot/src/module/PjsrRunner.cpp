// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PjsrRunner.h"
#include "PICopilotModule.h"
#include "Utf8.h"

#include <pcl/ByteArray.h>
#include <pcl/Exception.h>
#include <pcl/StringList.h>
#include <pcl/Variant.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <climits>
#include <exception>

namespace pcl
{

namespace
{

String S16( const std::string& s )
{
   return FromU8( s );
}

// JavaScript helpers prepended to every wrapper (all ASCII, all fixed text):
//  pcWell(s):  s with every unpaired surrogate replaced by U+FFFD, so the
//              final JSON.stringify never emits a "\udXXX" escape that no
//              strict JSON parser accepts.
//  pcAscii(s): s with every non-ASCII code unit \u-escaped. The result
//              crosses EvaluateScript as pure ASCII, immune to the engine's
//              narrow-string re-encoding (inc-5 Task 1, finding 1).
//  pcLine(e):  the <anonymous>:LINE of the innermost frame in the compiled
//              function's source, or 0 (a SyntaxError has none: Task 1, 2).
//              Limitation: code the script itself compiles (eval, its own
//              new Function) also appears as "<anonymous>" frames, so an
//              error raised inside such code reports that code's line, not
//              the model's -- inexact in that case, never fabricated.
//  pcCut(s,n): String( s ) cut to n characters, so a huge thrown value or
//              message never crosses the boundary whole.
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
   " return r; }"
   " function pcLine( e ) { try { var m = /<anonymous>:(\\d+):(\\d+)/.exec( String( e && e.stack ) );"
   " return m ? parseInt( m[1], 10 ) : 0; } catch ( x ) { return 0; } }";

// Evaluates an all-ASCII wrapper (fixed text + JSON literals) that returns
// pcAscii( JSON.stringify( ... ) ), and parses that.
nlohmann::json EvalJson( const std::string& body )
{
   const std::string src = std::string( "(function(){ " ) + kHelpersJs + " " + body + " })()";
   const Variant v = ThePICopilotModule->EvaluateScript( String( src.c_str() ), "JavaScript" );
   return nlohmann::json::parse( U8( v.ToString() ) );
}

// A line reported by pcLine() as a 1-based line of the model's code; 0 when unknown.
int BodyLine( int reported )
{
   const int offset = PjsrLineOffset();
   if ( reported <= 0 || offset < 0 )
      return 0;
   const int line = reported - offset;
   return line >= 1 ? line : 0;
}

bool IsDigit( char16_type c )
{
   return c >= '0' && c <= '9';
}

// "[YYYY-MM-DD hh:mm:ss] " at the start of a line (the core's log format).
bool HasLogTimestamp( const String& line )
{
   if ( line.Length() < 22 || line[0] != '[' || line[20] != ']' || line[21] != ' ' )
      return false;
   const char* const shape = "dddd-dd-dd dd:dd:dd";
   for ( int i = 0; i < 19; ++i )
      if ( shape[i] == 'd' ? !IsDigit( line[1+i] ) : line[1+i] != char16_type( shape[i] ) )
         return false;
   return true;
}

bool IsStarRule( const String& line )
{
   if ( line.Length() < 10 )
      return false;
   for ( size_type i = 0; i < line.Length(); ++i )
      if ( line[i] != '*' )
         return false;
   return true;
}

// console.endLog() returns the core's LOG format: a three-line banner
// ("****", "PixInsight Core <version>", "****") and a timestamp on every
// line. Only what the script wrote is useful to the model, so both are
// removed -- exactly those shapes, nothing else.
String ScriptConsoleText( const String& log )
{
   StringList lines;
   log.Break( lines, char16_type( '\n' ) );
   for ( String& line : lines )
      if ( HasLogTimestamp( line ) )
         line.DeleteLeft( 22 );
   size_type first = 0;
   if ( lines.Length() >= 3 && IsStarRule( lines[0] ) && lines[1].StartsWith( "PixInsight Core " ) && IsStarRule( lines[2] ) )
      first = 3;
   String out;
   for ( size_type i = first; i < lines.Length(); ++i )
   {
      out += lines[i];
      if ( i+1 < lines.Length() )
         out += '\n';
   }
   return out;
}

// Unicode general category Cf (format characters), Unicode 15.1.
struct CpRange { uint32 first, last; };
const CpRange kFormatChars[] =
{
   { 0x00AD, 0x00AD }, { 0x0600, 0x0605 }, { 0x061C, 0x061C }, { 0x06DD, 0x06DD }, { 0x070F, 0x070F },
   { 0x0890, 0x0891 }, { 0x08E2, 0x08E2 }, { 0x180E, 0x180E }, { 0x200B, 0x200F }, { 0x202A, 0x202E },
   { 0x2060, 0x2064 }, { 0x2066, 0x206F }, { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB }, { 0x110BD, 0x110BD },
   { 0x110CD, 0x110CD }, { 0x13430, 0x1343F }, { 0x1BCA0, 0x1BCA3 }, { 0x1D173, 0x1D17A }, { 0xE0001, 0xE0001 },
   { 0xE0020, 0xE007F }
};

bool IsFormatChar( uint32 c )
{
   for ( const CpRange& r : kFormatChars )
      if ( c >= r.first && c <= r.last )
         return true;
   return false;
}

// What kind of refused character c is; nullptr when it is allowed.
const char* RefusedKind( uint32 c )
{
   if ( c == 0x09 || c == 0x0A )
      return nullptr;
   if ( c == 0x0D )
      return "CARRIAGE RETURN not followed by a line feed";
   if ( c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F) )
      return "a control character";
   if ( c == 0x2028 )
      return "LINE SEPARATOR, which JavaScript treats as a line break";
   if ( c == 0x2029 )
      return "PARAGRAPH SEPARATOR, which JavaScript treats as a line break";
   if ( (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F || c == 0x061C )
      return "a bidirectional text control, which can reorder how the code is displayed";
   if ( c >= 0xD800 && c <= 0xDFFF )
      return "an unpaired surrogate, which is not valid text";
   if ( IsFormatChar( c ) )
      return "an invisible format character";
   // Not Cf, but invisible AND legal inside JavaScript identifiers, so two
   // names that look identical can be different variables.
   if ( c == 0x115F || c == 0x1160 || c == 0x3164 || c == 0xFFA0 )
      return "a Hangul filler, an invisible character JavaScript accepts inside names";
   if ( (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) )
      return "a variation selector, an invisible character JavaScript accepts inside names";
   if ( c == 0x034F || c == 0x17B4 || c == 0x17B5 )
      return "an invisible combining character JavaScript accepts inside names";
   return nullptr;
}

} // namespace

String NormalizeScriptNewlines( const String& code )
{
   String out;
   out.Reserve( code.Length() );
   for ( size_type i = 0; i < code.Length(); ++i )
      if ( !(code[i] == '\r' && i+1 < code.Length() && code[i+1] == '\n') )
         out += code[i];
   return out;
}

String ScriptCharProblem( const String& code )
{
   int line = 1, column = 1;
   const size_type n = code.Length();
   for ( size_type i = 0; i < n; ++i, ++column )
   {
      uint32 c = code[i];
      if ( c >= 0xD800 && c <= 0xDBFF && i+1 < n && code[i+1] >= 0xDC00 && code[i+1] <= 0xDFFF )
         c = 0x10000 + ((c - 0xD800) << 10) + (uint32( code[++i] ) - 0xDC00);
      if ( c == '\n' )
      {
         ++line;
         column = 0;
         continue;
      }
      if ( const char* kind = RefusedKind( c ) )
         return String().Format( "the script contains U+%04X (", unsigned( c ) ) + String( kind )
              + String().Format( ") at line %d, column %d. Characters that are invisible, reorder text, or that "
                                 "JavaScript treats as line breaks are refused, so that the user reads exactly the code "
                                 "that runs. Remove it, or write such characters as \\uXXXX escapes inside string "
                                 "literals (e.g. \"\\u200B\"), then call run_pjsr again. Nothing was shown to the user "
                                 "and nothing ran.", line, column );
   }
   return String();
}

std::string ScriptLiteral( const String& code )
{
   // U8() turns an unpaired surrogate into U+FFFD, so this never throws on
   // text from a String; dump( ensure_ascii ) escapes ", \, every control
   // character and every non-ASCII character (a non-BMP one as a \u pair).
   return nlohmann::json( U8( code ) ).dump( -1, ' ', true/*ensure_ascii*/ );
}

int PjsrLineOffset()
{
   static int offset = INT_MIN;   // root thread only
   if ( offset == INT_MIN )
   {
      try
      {
         // Body line 3 throws at run time; its frame line minus 3 is the header.
         const nlohmann::json j = EvalJson(
            "try { new Function( \"targetViewId\", \"\\n\\nthrow new Error( 'pc-calibrate' );\" )( \"\" );"
            " return JSON.stringify( { line: 0 } ); }"
            " catch ( e ) { return JSON.stringify( { line: pcLine( e ) } ); }" );
         const int line = j.value( "line", 0 );
         if ( line >= 3 )
            offset = line - 3;
      }
      catch ( ... )
      {
      }
   }
   return offset == INT_MIN ? -1 : offset;
}

PjsrCheck CheckPjsrSyntax( const String& code )
{
   PjsrCheck c;
   try
   {
      // Only constructs the function: parsing never executes the body.
      const nlohmann::json j = EvalJson(
         "try { new Function( \"targetViewId\", " + ScriptLiteral( code ) + " );"
         " return JSON.stringify( { ok: true } ); }"
         " catch ( e ) { return pcAscii( JSON.stringify( { ok: false, name: pcWell( pcCut( e && e.name, 200 ) ),"
         " message: pcWell( pcCut( e && e.message, " + std::to_string( PICopilotMaxScriptErrorChars + 16 ) + " ) ) } ) ); }" );
      if ( j.value( "ok", false ) )
      {
         c.ok = true;
         return c;
      }
      c.error = S16( j.value( "name", std::string( "Error" ) ) + ": " + j.value( "message", std::string() ) );
   }
   catch ( const pcl::Exception& x )
   {
      c.error = "the syntax check could not run: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      c.error = String( "the script text could not be prepared: " ) + String( x.what() );
   }
   if ( c.error.Length() > PICopilotMaxScriptErrorChars )
   {
      c.error = c.error.Left( PICopilotMaxScriptErrorChars );
      c.errorTruncated = true;
   }
   return c;
}

PjsrRun RunPjsr( const String& code, const IsoString& targetViewId )
{
   PjsrRun r;
   try
   {
      const std::string target = nlohmann::json( std::string( targetViewId.c_str() ) ).dump( -1, ' ', true );
      // beginLog() before the try, endLog() in its finally: balanced whatever
      // the script does. A value is cut in JS a little past the limit (so a
      // huge return does not cross the boundary), then exactly in C++.
      const std::string kErrCut = std::to_string( PICopilotMaxScriptErrorChars + 16 );
      const std::string body =
         "var out = { ok: true }; console.beginLog();"
         " try { var f = new Function( \"targetViewId\", " + ScriptLiteral( code ) + " ); var v = f( " + target + " );"
         "  if ( v !== undefined ) { var s; try { s = JSON.stringify( v ); } catch ( x ) { s = undefined; }"
         "   if ( s === undefined ) s = String( v );"
         "   out.valueLength = s.length; out.value = pcWell( s.length > " + std::to_string( PICopilotMaxScriptValueChars + 16 )
         + " ? s.slice( 0, " + std::to_string( PICopilotMaxScriptValueChars + 16 ) + " ) : s ); } }"
         " catch ( e ) { out.ok = false; var es;"
         "  try { es = String( e ); } catch ( x ) { es = \"(the thrown value cannot be converted to text)\"; }"
         "  out.errorLength = es.length; out.error = pcWell( pcCut( es, " + kErrCut + " ) ); out.line = pcLine( e ); }"
         " finally { try { out.consoleB64 = console.endLog().toBase64(); }"
         "  catch ( x ) { out.consoleB64 = \"\"; var xs; try { xs = String( x ); } catch ( y ) { xs = \"(no text)\"; }"
         "   out.consoleError = pcWell( pcCut( xs, " + kErrCut + " ) ); } }"
         " return pcAscii( JSON.stringify( out ) );";
      const auto t0 = std::chrono::steady_clock::now();
      const nlohmann::json j = EvalJson( body );
      r.elapsedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
      r.ok = j.value( "ok", false );
      r.value = S16( j.value( "value", std::string() ) );
      if ( j.value( "valueLength", 0.0 ) > double( PICopilotMaxScriptValueChars ) )
         r.valueTruncated = true;
      const ByteArray raw = IsoString( j.value( "consoleB64", std::string() ).c_str() ).FromBase64();
      r.console = ScriptConsoleText( S16( std::string( reinterpret_cast<const char*>( raw.Begin() ), raw.Length() ) ) );
      if ( j.contains( "consoleError" ) )
      {
         String ce = S16( j.value( "consoleError", std::string() ) );
         if ( ce.Length() > PICopilotMaxScriptErrorChars )
            ce = ce.Left( PICopilotMaxScriptErrorChars ) + "...";
         r.console += "\n[the console output could not be read: " + ce + "]";
      }
      if ( !r.ok )
      {
         r.error = S16( j.value( "error", std::string( "(no error text)" ) ) );
         r.line = BodyLine( j.value( "line", 0 ) );
         if ( j.value( "errorLength", 0.0 ) > double( PICopilotMaxScriptErrorChars ) )
            r.errorTruncated = true;
      }
   }
   catch ( const pcl::Exception& x )
   {
      r.ok = false;
      r.error = "the script could not run: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      r.ok = false;
      r.error = String( "the script result could not be read: " ) + String( x.what() );
   }
   if ( r.error.Length() > PICopilotMaxScriptErrorChars )
   {
      r.error = r.error.Left( PICopilotMaxScriptErrorChars );
      r.errorTruncated = true;
   }
   if ( r.value.Length() > PICopilotMaxScriptValueChars )
   {
      r.value = r.value.Left( PICopilotMaxScriptValueChars );
      r.valueTruncated = true;
   }
   if ( r.console.Length() > PICopilotMaxScriptConsoleChars )
   {
      const size_type cut = r.console.Length() - PICopilotMaxScriptConsoleChars;
      r.console = String().Format( "[... %u earlier characters omitted]\n", unsigned( cut ) )
                + r.console.Right( PICopilotMaxScriptConsoleChars );
      r.consoleTruncated = true;
   }
   return r;
}

} // namespace pcl
