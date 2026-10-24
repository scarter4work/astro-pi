// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// PlanetaryStack's keyword plan and result text: which FITS cards and HISTORY
// lines a stack carries, the summary lines the console shows, and the
// PlanetaryStack window's info, result and progress text. PCL-free and
// header-only, so it is unit tested without PixInsight
// (test/unit/test_stack_cards.cpp); PlanetaryStackInstance turns the cards into
// PCL keywords.

#ifndef __StackCards_h
#define __StackCards_h

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "Engine.h"
#include "FitsText.h"
#include "PlanetaryVersion.h"
#include "SerInfoText.h"

namespace planetary::stackcards
{

// The capture's time span for DATE-OBS / DATE-END (PlanetaryStackInstance's
// CaptureTimes fills it from a SER).
struct StackTimes
{
   std::optional<std::string> start, end;
   bool fromFrames = false; // start/end are frame times (trailer), not the header's start
};

// One keyword card. value: printable ASCII, unquoted; text: written as a FITS
// string (quoted, quotes doubled), else as is (a number).
struct Card
{
   std::string name, value, comment;
   bool text = true;
};

inline bool IsAvi( const ser::FileInfo& info )
{
   return info.container != ser::Container::Ser;
}

// Where summary warnings are shown: the PlanetaryStack window (its Bayer
// pattern combo and Save pattern... are right there), or the process's console
// and FITS HISTORY (scripts set the bayerPattern parameter).
enum class WarningsFor { Window, Process };

inline bool EndsWith( const std::string& s, const std::string& tail )
{
   return s.size() >= tail.size() && s.compare( s.size() - tail.size(), tail.size(), tail ) == 0;
}

// The summary warnings as shown, one line per condition. A stacked undeclared
// mosaic (Ruling P9) carries two warnings about one condition, the reader's
// (its reason, ending in ser::kRawCfaAsMonoTail) and the engine's
// kUndeclaredMosaicWarning; they become one line, first: the reader's reason,
// what was stacked and why it cannot be undone, and this context's remedy.
// Every other warning passes through, an AVI's reader warnings reworded for the
// context (StackWindowWarning / StackWarning).
inline std::vector<std::string> ShownWarnings( const stack::StackOutcome& outcome, WarningsFor where )
{
   const bool isAvi = IsAvi( outcome.info );
   const bool mosaic = outcome.image && stack::UndeclaredMosaic( outcome.info );
   const std::string tail = ser::kRawCfaAsMonoTail;
   std::vector<std::string> shown;
   std::string reason;
   for ( const std::string& w : outcome.summary.warnings )
   {
      if ( mosaic && w == stack::kUndeclaredMosaicWarning )
         continue;
      if ( mosaic && EndsWith( w, tail ) )
      {
         if ( reason.empty() )
            reason = w.substr( 0, w.size() - tail.size() );
         continue;
      }
      shown.push_back( !isAvi ? w : where == WarningsFor::Window ? preview::StackWindowWarning( w )
                                                                 : preview::StackWarning( w ) );
   }
   if ( mosaic )
   {
      if ( reason.empty() )
         reason = "No Bayer pattern is known for this AVI";
      reason[0] = char( std::toupper( static_cast<unsigned char>( reason[0] ) ) );
      shown.insert( shown.begin(), reason +
         ", so the raw colour mosaic was stacked as mono: the result is a mono image of the mosaic and cannot be "
         "debayered afterwards (every frame was resampled onto the stack, which mixes the colour sites). For colour, " +
         std::string( where == WarningsFor::Window
                         ? "choose the pattern under Bayer pattern and Stack! again; Save pattern... then remembers it."
                         : "set bayerPattern (or save the pattern with Save pattern... in PlanetaryStack or SERViewer) "
                           "and stack again." ) );
   }
   return shown;
}

// A stack made from capture `a` still stands for `b` (the file read again, or
// with another Bayer choice): the same container, size, depth and frame count,
// the same effective colour layout, and the same undeclared-mosaic standing
// (its warning). The pattern's source may differ: RGGB chosen and RGGB from the
// sidecar Save pattern... then wrote stack alike.
inline bool SameCapture( const ser::FileInfo& a, const ser::FileInfo& b )
{
   return a.container == b.container && a.colorId == b.colorId && a.frameCount == b.frameCount &&
          a.header.width == b.header.width && a.header.height == b.header.height && a.header.depth == b.header.depth &&
          stack::UndeclaredMosaic( a ) == stack::UndeclaredMosaic( b );
}

inline std::string Fixed( double v, int decimals )
{
   char b[64];
   std::snprintf( b, sizeof b, "%.*f", decimals, v );
   return b;
}

// "4088 frames measured, 4085 stacked in 5.09 s (Hot pixels 0.16 s, ...); read 2135 MB at 420 MB/s".
// Without an image (a diagnosis), "stacked" is left out: nothing was.
inline std::string CountsLine( const stack::StackSummary& s, bool stacked )
{
   std::string t = std::to_string( s.measured ) + " frames measured";
   if ( stacked )
      t += ", " + std::to_string( s.used ) + " stacked";
   t += " in " + Fixed( s.seconds, 2 ) + " s";
   if ( !s.phaseSeconds.empty() )
   {
      t += " (";
      for ( size_t i = 0; i < s.phaseSeconds.size(); ++i )
         t += (i > 0 ? ", " : "") + s.phaseSeconds[i].first + " " + Fixed( s.phaseSeconds[i].second, 2 ) + " s";
      t += ")";
   }
   t += "; read " + Fixed( s.bytesRead / 1e6, 0 ) + " MB at " + Fixed( s.readMBps, 0 ) + " MB/s";
   return t;
}

// The diagnosis message with its numbers: "<message> (frames 40, frames with a target 0)" (%.4g).
inline std::string DiagnosisText( const stack::Diagnosis& d )
{
   std::string t = d.message;
   if ( !d.numbers.empty() )
   {
      t += " (";
      for ( size_t i = 0; i < d.numbers.size(); ++i )
      {
         char v[64];
         std::snprintf( v, sizeof v, "%.4g", d.numbers[i].second );
         t += (i > 0 ? ", " : "") + d.numbers[i].first + " " + v;
      }
      t += ")";
   }
   return t;
}

// The stack's cards, in order: SRCVIDEO (the file name, cut to fit its card
// with the extension kept -- the full name is HISTORY's first line), NFRAMES
// (measured), NCOMBINE (stacked), DATE-OBS / DATE-END (`times`; never for an
// AVI, which has no trustworthy clock, whatever `times` holds), INSTRUME
// (the SER header, or the ASIAIR sidecar's camera), TELESCOP, OBSERVER. No
// BAYERPAT: the stack is debayered RGB, or the raw mosaic as mono.
inline std::vector<Card> StackCards( const std::string& fileNameUtf8, const stack::StackOutcome& outcome,
                                     const StackTimes& times )
{
   const ser::FileInfo& info = outcome.info;
   const bool isAvi = IsAvi( info );
   std::vector<Card> cards;
   cards.push_back( { "SRCVIDEO", fits::FitStringValue( fits::AsciiFromUtf8( fileNameUtf8 ) ),
                      "Video stacked (full name in HISTORY)" } );
   cards.push_back( { "NFRAMES", std::to_string( outcome.summary.measured ), "Frames in the video, every one measured", false } );
   cards.push_back( { "NCOMBINE", std::to_string( outcome.summary.used ), "Frames stacked", false } );
   if ( !isAvi && times.start )
      cards.push_back( { "DATE-OBS", fits::AsciiFromBytes( *times.start ),
                         times.fromFrames ? "UTC time of the first frame stacked (SER timestamps)"
                                          : "UTC start of the capture (SER header)" } );
   if ( !isAvi && times.end )
      cards.push_back( { "DATE-END", fits::AsciiFromBytes( *times.end ), "UTC time of the last frame stacked (SER timestamps)" } );
   // SER header text has no defined encoding; FitStringValue keeps a long one within its card.
   if ( !info.header.instrument.empty() )
      cards.push_back( { "INSTRUME", fits::FitStringValue( fits::AsciiFromBytes( info.header.instrument ) ),
                         isAvi ? "ASIAIR sidecar camera" : "SER header Instrument" } );
   if ( !info.header.telescope.empty() )
      cards.push_back( { "TELESCOP", fits::FitStringValue( fits::AsciiFromBytes( info.header.telescope ) ), "SER header Telescope" } );
   if ( !info.header.observer.empty() )
      cards.push_back( { "OBSERVER", fits::FitStringValue( fits::AsciiFromBytes( info.header.observer ) ), "SER header Observer" } );
   return cards;
}

// The stack's HISTORY texts, unsplit (one FITS card holds 72 characters; the
// caller splits at spaces): the version and the full file name, an AVI's Bayer
// pattern and its source, the selection and alignment lines, the frame counts
// and time, and every summary warning (reader warnings reworded for a stack).
inline std::vector<std::string> StackHistory( const std::string& fileNameUtf8, const stack::StackOutcome& outcome )
{
   const ser::FileInfo& info = outcome.info;
   const stack::StackSummary& s = outcome.summary;
   const bool isAvi = IsAvi( info );
   std::vector<std::string> h;
   h.push_back( "PlanetaryStack " PLANETARY_VERSION_STRING ": stacked " + fits::AsciiFromUtf8( fileNameUtf8 ) );
   if ( isAvi )
      h.push_back( "PlanetaryStack: Bayer " + fits::FitsAscii( preview::BayerText( info ) ) );
   h.push_back( "PlanetaryStack: " + fits::FitsAscii( s.selection ) );
   h.push_back( "PlanetaryStack: " + fits::FitsAscii( s.alignment ) );
   h.push_back( "PlanetaryStack: " + CountsLine( s, true/*stacked*/ ) );
   // SER header text has no defined encoding; AVI warnings and the engine's are
   // built from UTF-8 file names and sidecar text, or are ASCII.
   for ( const std::string& w : ShownWarnings( outcome, WarningsFor::Process ) )
      h.push_back( "PlanetaryStack warning: " + (isAvi ? fits::FitsAscii( w ) : fits::AsciiFromBytes( w )) );
   return h;
}

// ---- The PlanetaryStack window's text (PlanetaryStackInterface) ------------
// UTF-8: file names are shown as they are; the engine's text is ASCII.

// "Jupiter.avi: 1500 frames, 1024 x 768, 8-bit, Bayer RGGB (sidecar RG)": an
// AVI's pattern with its source (BayerText); a SER's colour from its header.
inline std::string StackInfoLine( const std::string& fileNameUtf8, const ser::FileInfo& info )
{
   std::string colour;
   if ( stack::UndeclaredMosaic( info ) )
      colour = "no Bayer pattern known (raw CFA)";   // the fact; the warning gives the remedy
   else if ( IsAvi( info ) )
      colour = "Bayer " + preview::BayerText( info );
   else if ( ser::IsBayer( info.colorId ) )
      colour = std::string( "Bayer " ) + ser::BayerPatternKeyword( info.colorId );
   else
      colour = info.colorId == ser::ColorId::Mono ? "mono" : "RGB";
   return fileNameUtf8 + ": " + std::to_string( info.frameCount ) + " frames, " + std::to_string( info.header.width ) +
          " x " + std::to_string( info.header.height ) + ", " + std::to_string( info.header.depth ) + "-bit, " + colour;
}

// The result of a run, one fact per line. A stack: the frames used of those
// measured and the time, every summary warning (an AVI's reader warnings
// reworded for the window, StackWindowWarning), then the selection and
// alignment lines. A diagnosis: "No image: <message>", its numbers one per
// line (%.4g), the frames measured and the time, and the warnings.
inline std::string ResultText( const stack::StackOutcome& outcome )
{
   const stack::StackSummary& s = outcome.summary;
   std::string warnings;
   for ( const std::string& w : ShownWarnings( outcome, WarningsFor::Window ) )
      warnings += "\nWarning: " + w;
   if ( outcome.image )
      return "Stacked: " + std::to_string( s.used ) + " of " + std::to_string( s.measured ) + " frames used, in " +
             Fixed( s.seconds, 2 ) + " s." + warnings + "\nFrames: " + s.selection + "\nAlignment: " + s.alignment;
   std::string t = "No image: " + outcome.diagnosis.message;
   if ( !outcome.diagnosis.numbers.empty() )
   {
      t += "\nThe numbers behind it:";
      for ( const auto& [name, value] : outcome.diagnosis.numbers )
      {
         char v[64];
         std::snprintf( v, sizeof v, "%.4g", value );
         t += "\n  " + name + ": " + v;
      }
   }
   return t + "\n" + std::to_string( s.measured ) + " frames measured in " + Fixed( s.seconds, 2 ) + " s." + warnings;
}

// The fraction of a phase done, 0..1 (0 for no total).
inline double ProgressFraction( int64_t done, int64_t total )
{
   if ( total <= 0 )
      return 0;
   return done >= total ? 1.0 : done <= 0 ? 0.0 : double( done ) / double( total );
}

// The progress bar's text for the engine's three phases (Engine.h, in order):
// "Step 1 of 3: Measuring frames 1234/4088" -- the engine's total there is the
// hot-pixel frames plus every frame, so the count is of the capture's
// `frames`, after the hot-pixel frames; "Step 2 of 3: Aligning 50%" (its total
// is not a frame count); "Step 3 of 3: Stacking 312/1400" (the frames the plan
// uses). Another name as it is.
inline std::string ProgressText( const std::string& phase, int64_t done, int64_t total, int64_t frames )
{
   const auto step = []( int i ) { return "Step " + std::to_string( i ) + " of 3: "; };
   if ( phase == stack::kPhaseMeasuring )
   {
      const int64_t hot = std::max<int64_t>( 0, total - frames );
      return step( 1 ) + phase + " " + std::to_string( std::clamp<int64_t>( done - hot, 0, frames ) ) + "/" +
             std::to_string( frames );
   }
   if ( phase == stack::kPhaseAligning )
      return step( 2 ) + phase + " " + std::to_string( int( 100 * ProgressFraction( done, total ) ) ) + "%";
   if ( phase == stack::kPhaseStacking )
      return step( 3 ) + phase + " " + std::to_string( std::clamp<int64_t>( done, 0, total ) ) + "/" + std::to_string( total );
   return phase;
}

} // namespace planetary::stackcards

#endif // __StackCards_h
