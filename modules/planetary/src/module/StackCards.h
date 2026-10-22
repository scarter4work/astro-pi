// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
//
// PlanetaryStack's keyword plan and result text: which FITS cards and HISTORY
// lines a stack carries, and the summary lines the console shows. PCL-free and
// header-only, so it is unit tested without PixInsight
// (test/unit/test_stack_cards.cpp); PlanetaryStackInstance turns the cards into
// PCL keywords.

#ifndef __StackCards_h
#define __StackCards_h

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
   for ( const std::string& w : s.warnings )
      h.push_back( "PlanetaryStack warning: " +
                   (isAvi ? fits::FitsAscii( preview::StackWarning( w ) ) : fits::AsciiFromBytes( w )) );
   return h;
}

} // namespace planetary::stackcards

#endif // __StackCards_h
