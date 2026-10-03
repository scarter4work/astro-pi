// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NukeXConsoleText.hpp"
#include "NukeXVersion.h"

#include <cstddef>
#include <string>
#include <vector>

namespace nukex {

namespace {

// The wordmark, one entry per row.  These are UTF-8 box-drawing and block
// characters (U+2588 FULL BLOCK and friends), which is exactly why the
// banner must reach PCL through String::UTF8ToUTF16 -- PCL decodes a plain
// const char* as ISO-8859-1 and would ship these as garbage.
const char* const kWordmark[] = {
   "███╗   ██╗██╗   ██╗██╗  ██╗███████╗██╗  ██╗",
   "████╗  ██║██║   ██║██║ ██╔╝██╔════╝╚██╗██╔╝",
   "██╔██╗ ██║██║   ██║█████╔╝ █████╗   ╚███╔╝",
   "██║╚██╗██║██║   ██║██╔═██╗ ██╔══╝   ██╔██╗",
   "██║ ╚████║╚██████╔╝██║  ██╗███████╗██╔╝ ██╗",
   "╚═╝  ╚═══╝ ╚═════╝ ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝"
};

// Width of the widest wordmark row, in characters.  Fixing the gradient
// denominator here rather than per row keeps the colour bands vertically
// aligned down the whole wordmark.
constexpr int kWordmarkColumns = 43;

// Colour steps across the wordmark.  Banding rather than interpolating per
// character keeps the escape sequences down to eight per row.
constexpr int kBands = 8;

// Horizontal ramp: cyan on the left, violet on the right.
constexpr int kRampFrom[3] = {   0, 200, 255 };
constexpr int kRampTo  [3] = { 170,  90, 255 };

// The strapline under the wordmark: version dim, author in a bright accent.
constexpr int kDim   [3] = { 120, 128, 140 };
constexpr int kAccent[3] = { 120, 225, 255 };

const char* const kReset = "\x1b[0m";

// 24-bit foreground select, the form Console.h documents for KDE Konsole.
std::string foreground( const int rgb[3] )
{
   return std::string( "\x1b[38;2;" ) + std::to_string( rgb[0] ) + ';'
                                      + std::to_string( rgb[1] ) + ';'
                                      + std::to_string( rgb[2] ) + 'm';
}

// One step along the cyan->violet ramp.
std::string band_colour( int band )
{
   const double t = ( kBands > 1 ) ? double( band ) / ( kBands - 1 ) : 0.0;
   int rgb[3];
   for ( int c = 0; c < 3; ++c )
      rgb[c] = int( kRampFrom[c] + t * ( kRampTo[c] - kRampFrom[c] ) + 0.5 );
   return foreground( rgb );
}

// Split UTF-8 into code points, so a column index counts characters and not
// bytes -- every glyph in the wordmark is three bytes wide.
std::vector<std::string> utf8_chars( const char* s )
{
   std::vector<std::string> out;
   for ( const unsigned char* p = reinterpret_cast<const unsigned char*>( s ); *p != 0; )
   {
      std::size_t want = 1;
      if      ( (*p & 0xE0) == 0xC0 ) want = 2;
      else if ( (*p & 0xF0) == 0xE0 ) want = 3;
      else if ( (*p & 0xF8) == 0xF0 ) want = 4;

      // Never walk past the terminator, however malformed the input is.
      std::size_t len = 1;
      while ( len < want && p[len] != 0 ) ++len;

      out.emplace_back( reinterpret_cast<const char*>( p ), len );
      p += len;
   }
   return out;
}

} // anonymous namespace

std::string console_detail_line( int depth, const std::string& detail )
{
   // <end> parks the cursor after the last character in the console and
   // <cbr> breaks the line unless it is already empty.  Without this the
   // progress monitor's in-place percentage eats the start of the detail.
   std::string out = "<end><cbr>";
   for ( int i = 0; i < depth; ++i )
      out += "  ";
   out += detail;
   return out;
}

std::string nukex_banner()
{
   std::string out = "<end><cbr>";

   bool first_row = true;
   for ( const char* row : kWordmark )
   {
      if ( !first_row )
         out += '\n';
      first_row = false;

      const std::vector<std::string> chars = utf8_chars( row );
      int current_band = -1;
      for ( std::size_t c = 0; c < chars.size(); ++c )
      {
         int band = int( c ) * kBands / kWordmarkColumns;
         if ( band >= kBands )
            band = kBands - 1;
         if ( band != current_band )
         {
            current_band = band;
            out += band_colour( band );
         }
         out += chars[c];
      }
      // Reset on every row: colour must not bleed into whatever PixInsight
      // writes to the console next.
      out += kReset;
   }

   // NUKEX_VERSION_STRING, never a literal -- a version bump must not be
   // able to leave a stale banner behind.
   out += '\n';
   out += foreground( kDim );
   out += "v" NUKEX_VERSION_STRING "   ·   ";
   out += foreground( kAccent );
   out += "Scott Carter";
   out += kReset;

   return out;
}

} // namespace nukex
