// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// Guards for the two console-output defects these builders exist to fix.
//
//   1. StandardStatus (the progress monitor) writes its percentage IN PLACE
//      on the current console line.  A per-frame detail line written without
//      a leading break got overwritten mid-word.  Straight out of
//      /tmp/nukex_e2e_console.log:
//
//         frame 4: 200 stars, FWHM 3.  6%    frame 5: 200 stars, FWHM 3.32 px
//
//      "FWHM 3.86 px" lost its last four characters and frame 5 continued on
//      the same line.  console_detail_line() must therefore always open with
//      <end><cbr>, exactly as begin_phase() already does.
//
//   2. The banner is UTF-8 block art.  PCL decodes a plain const char* as
//      ISO-8859-1, so the banner has to reach the console through
//      pcl::String::UTF8ToUTF16 -- which is only correct if what we hand it
//      really is well-formed UTF-8.  The decoder below proves that.

#include "catch_amalgamated.hpp"

#include "NukeXConsoleText.hpp"
#include "NukeXVersion.h"

#include <string>
#include <vector>

using namespace nukex;

namespace {

std::vector<std::string> split_lines( const std::string& s )
{
   std::vector<std::string> lines;
   std::string current;
   for ( char c : s ) {
      if ( c == '\n' ) { lines.push_back( current ); current.clear(); }
      else             { current += c; }
   }
   lines.push_back( current );
   return lines;
}

// Strict UTF-8 decoder: rejects stray continuation bytes, truncated
// sequences, overlong encodings, surrogates and out-of-range code points.
// An ISO-8859-1 mangling of block art trips the first two checks.
bool is_valid_utf8( const std::string& s )
{
   std::size_t i = 0;
   const std::size_t n = s.size();
   while ( i < n ) {
      const unsigned char c = static_cast<unsigned char>( s[i] );
      std::size_t extra;
      unsigned int cp;
      if ( c < 0x80 )                { ++i; continue; }
      else if ( (c & 0xE0) == 0xC0 ) { extra = 1; cp = c & 0x1Fu; }
      else if ( (c & 0xF0) == 0xE0 ) { extra = 2; cp = c & 0x0Fu; }
      else if ( (c & 0xF8) == 0xF0 ) { extra = 3; cp = c & 0x07u; }
      else return false;                                  // stray 0x80-0xBF, or 0xF8+

      if ( i + extra >= n ) return false;                 // truncated
      for ( std::size_t k = 1; k <= extra; ++k ) {
         const unsigned char cc = static_cast<unsigned char>( s[i + k] );
         if ( (cc & 0xC0) != 0x80 ) return false;
         cp = (cp << 6) | (cc & 0x3Fu);
      }
      if ( extra == 1 && cp < 0x80 )      return false;   // overlong
      if ( extra == 2 && cp < 0x800 )     return false;   // overlong
      if ( extra == 3 && cp < 0x10000 )   return false;   // overlong
      if ( cp > 0x10FFFF )                return false;
      if ( cp >= 0xD800 && cp <= 0xDFFF ) return false;   // surrogate half
      i += extra + 1;
   }
   return true;
}

const char* const kSetForeground = "\x1b[38;2;";
const char* const kReset         = "\x1b[0m";

} // anonymous namespace

// ---------------------------------------------------------------------------
// console_detail_line
// ---------------------------------------------------------------------------

TEST_CASE( "console_detail_line opens with <end><cbr>", "[module][console_text]" )
{
   // The regression guard for the overwritten "FWHM 3.86 px" line.
   const std::string line = console_detail_line( 1, "frame 4: 200 stars, FWHM 3.86 px" );
   REQUIRE( line.rfind( "<end><cbr>", 0 ) == 0 );
   REQUIRE( line.find( "frame 4: 200 stars, FWHM 3.86 px" ) != std::string::npos );
}

TEST_CASE( "console_detail_line indents two spaces per depth", "[module][console_text]" )
{
   REQUIRE( console_detail_line( 0, "x" ) == "<end><cbr>x" );
   REQUIRE( console_detail_line( 1, "x" ) == "<end><cbr>  x" );
   REQUIRE( console_detail_line( 2, "x" ) == "<end><cbr>    x" );
   REQUIRE( console_detail_line( 3, "x" ) == "<end><cbr>      x" );
}

TEST_CASE( "console_detail_line treats a negative depth as no indent", "[module][console_text]" )
{
   REQUIRE( console_detail_line( -1, "x" ) == "<end><cbr>x" );
}

// ---------------------------------------------------------------------------
// nukex_banner
// ---------------------------------------------------------------------------

TEST_CASE( "banner carries this build's own version", "[module][console_text]" )
{
   // Hardcoding the version would let the banner drift on the next bump.
   REQUIRE( nukex_banner().find( NUKEX_VERSION_STRING ) != std::string::npos );
}

TEST_CASE( "banner names the author", "[module][console_text]" )
{
   REQUIRE( nukex_banner().find( "Scott Carter" ) != std::string::npos );
}

TEST_CASE( "banner opens with <end><cbr>", "[module][console_text]" )
{
   REQUIRE( nukex_banner().rfind( "<end><cbr>", 0 ) == 0 );
}

TEST_CASE( "banner keeps the block-art wordmark", "[module][console_text]" )
{
   const std::string banner = nukex_banner();
   REQUIRE( banner.find( "\xE2\x96\x88" ) != std::string::npos );  // U+2588 FULL BLOCK
   // Six wordmark rows plus the version/author line.
   REQUIRE( split_lines( banner ).size() == 7 );
}

TEST_CASE( "every coloured banner line resets colour before it ends", "[module][console_text]" )
{
   const std::vector<std::string> lines = split_lines( nukex_banner() );
   int coloured = 0;
   for ( const std::string& line : lines ) {
      if ( line.find( kSetForeground ) == std::string::npos )
         continue;
      ++coloured;
      // The last escape sequence on the line must be the reset, and it must
      // be the very end of the line -- otherwise colour bleeds into whatever
      // PixInsight writes next.
      const std::size_t last = line.rfind( "\x1b[" );
      REQUIRE( last != std::string::npos );
      REQUIRE( line.compare( last, 4, kReset ) == 0 );
      REQUIRE( last + 4 == line.size() );
   }
   REQUIRE( coloured == 7 );  // every line of the banner is coloured
}

TEST_CASE( "banner is well-formed UTF-8", "[module][console_text]" )
{
   // PCL's String(const char*) would decode this as ISO-8859-1; the caller
   // must route it through UTF8ToUTF16, and that is only meaningful if the
   // bytes really are UTF-8.
   REQUIRE( is_valid_utf8( nukex_banner() ) );
}

TEST_CASE( "the UTF-8 validator actually rejects bad input", "[module][console_text]" )
{
   // Guards the guard: a validator that returns true unconditionally would
   // make the test above worthless.
   REQUIRE_FALSE( is_valid_utf8( std::string( "\x88" ) ) );          // stray continuation
   REQUIRE_FALSE( is_valid_utf8( std::string( "\xE2\x96" ) ) );      // truncated
   REQUIRE_FALSE( is_valid_utf8( std::string( "\xC0\xAF" ) ) );      // overlong '/'
   REQUIRE( is_valid_utf8( std::string( "\xE2\x96\x88" ) ) );        // U+2588
}
