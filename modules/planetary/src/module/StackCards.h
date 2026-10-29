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

#include "CaptureName.h"
#include "Engine.h"
#include "FitsText.h"
#include "Mineral.h"
#include "Planet.h"
#include "PlanetaryVersion.h"
#include "SerInfoText.h"

namespace planetary::stackcards
{

// The target a stack is of, and where it came from: the target parameter, or
// (Auto) the ASIAIR capture name (ser::ParseCaptureName).
struct TargetChoice
{
   ser::Target target = ser::Target::Other;
   bool setByUser = false;  // the target parameter named it
   bool inFileName = false; // Auto, and the file name follows the ASIAIR pattern
};

// `userTarget`: the target parameter's choice, nullopt for Auto (then the file
// name decides; a name that is no capture name, or names no planet, is Other).
// `pathUtf8`: a base name or a full path.
inline TargetChoice ResolveTarget( std::optional<ser::Target> userTarget, const std::string& pathUtf8 )
{
   TargetChoice t;
   if ( userTarget )
   {
      t.target = *userTarget;
      t.setByUser = true;
      return t;
   }
   const ser::CaptureName c = ser::ParseCaptureName( pathUtf8 );
   t.target = c.target;
   t.inFileName = c.parsed;
   return t;
}

// Why Mineral moon cannot run for this capture (ser::MineralMoonRefusal, the
// filter read from the capture name), or nullopt. `info` must be the reader's
// after OpenVideo has resolved an AVI's Bayer pattern (AviFile leaves an AVI
// Mono until then; B1 review m2).
inline std::optional<std::string> MineralRefusalFor( const std::string& pathUtf8, const ser::FileInfo& info,
                                                     const TargetChoice& target )
{
   const ser::CaptureName c = ser::ParseCaptureName( pathUtf8 );
   return ser::MineralMoonRefusal( target.target, c.filter, info, c.filterToken );
}

// "Moon (from file name)", "Jupiter (set by user)", "Other (not in file name)".
inline std::string TargetText( const TargetChoice& t )
{
   return std::string( ser::TargetName( t.target ) ) +
          (t.setByUser ? " (set by user)" : t.inFileName ? " (from file name)" : " (not in file name)");
}

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

// The stack's cards, in order: OBJECT (the target's name, unless it is
// Other), SRCVIDEO (the file name, cut to fit its card
// with the extension kept -- the full name is HISTORY's first line), NFRAMES
// (measured), NCOMBINE (stacked), DATE-OBS / DATE-END (`times`; never for an
// AVI, which has no trustworthy clock, whatever `times` holds), INSTRUME
// (the SER header, or the ASIAIR sidecar's camera), TELESCOP, OBSERVER. No
// BAYERPAT: the stack is debayered RGB, or the raw mosaic as mono.
inline std::vector<Card> StackCards( const std::string& fileNameUtf8, const stack::StackOutcome& outcome,
                                     const StackTimes& times, const TargetChoice& target )
{
   const ser::FileInfo& info = outcome.info;
   const bool isAvi = IsAvi( info );
   std::vector<Card> cards;
   if ( target.target != ser::Target::Other )
      cards.push_back( { "OBJECT", ser::TargetName( target.target ),
                         target.setByUser ? "Target, set by the user" : "Target, from the file name" } );
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
// caller splits at spaces): the version and the full file name, the target and
// where it came from ("PlanetaryStack target: Moon (from file name)"), an AVI's Bayer
// pattern and its source, the selection and alignment lines, the frame counts
// and time, and every summary warning (reader warnings reworded for a stack).
inline std::vector<std::string> StackHistory( const std::string& fileNameUtf8, const stack::StackOutcome& outcome,
                                              const TargetChoice& target )
{
   const ser::FileInfo& info = outcome.info;
   const stack::StackSummary& s = outcome.summary;
   const bool isAvi = IsAvi( info );
   std::vector<std::string> h;
   h.push_back( "PlanetaryStack " PLANETARY_VERSION_STRING ": stacked " + fits::AsciiFromUtf8( fileNameUtf8 ) );
   h.push_back( "PlanetaryStack target: " + TargetText( target ) );
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

// ---- Post-processing: the mineral moon (Mineral.h) or planet processing (Planet.h) ---------
// A stack can carry one display-ready product made from it: the mineral moon of
// a Moon, or the planet processing of a planet. Both are the engine's prepare /
// finish pair on one cache (stack::MineralCache), so the window's sliders, its
// worker and its second window serve either through these.

enum class PostKind { Mineral, Planet };

// The product a target's row offers: the mineral moon for the Moon, planet
// processing for anything else (a planet; Other, which planet processing
// refuses with "pick the planet in the Target list").
inline PostKind PostKindFor( ser::Target t )
{
   return t == ser::Target::Moon ? PostKind::Mineral : PostKind::Planet;
}

// "Mineral moon" / "Planet processing": the option's name, as its check box reads.
inline std::string PostTitle( PostKind k )
{
   return k == PostKind::Mineral ? "Mineral moon" : "Planet processing";
}

// "mineral" / "planet": its HISTORY prefix ("PlanetaryStack mineral: "), its
// window's id suffix ("_mineral") and its image ("the mineral image").
inline std::string PostNoun( PostKind k )
{
   return k == PostKind::Mineral ? "mineral" : "planet";
}

// "mineral moon" / "planet processing": the product in running text (the
// console, errors).
inline std::string PostName( PostKind k )
{
   return k == PostKind::Mineral ? "mineral moon" : "planet processing";
}

// One product's parameters: the kind's own and, for a planet, the body whose
// catalogue colour calibrates it (Planet.h PreparePlanet).
struct PostParams
{
   PostKind kind = PostKind::Mineral;
   ser::Target target = ser::Target::Moon;
   stack::MineralParams mineral;
   stack::PlanetParams planet;
};

// The same product at the same slider values.
inline bool SamePostParams( const PostParams& a, const PostParams& b )
{
   if ( a.kind != b.kind )
      return false;
   if ( a.kind == PostKind::Mineral )
      return a.mineral.strength == b.mineral.strength && a.mineral.chromaBlurSigma == b.mineral.chromaBlurSigma &&
             a.mineral.sharpen == b.mineral.sharpen;
   return a.target == b.target && a.planet.saturation == b.planet.saturation &&
          a.planet.chromaBlurSigma == b.planet.chromaBlurSigma && a.planet.sharpen == b.planet.sharpen;
}

// A cache prepared for `a` serves `b` (only the finish runs again): the same
// kind and, for a planet, the same body.
inline bool SamePostData( const PostParams& a, const PostParams& b )
{
   return a.kind == b.kind && (a.kind == PostKind::Mineral || a.target == b.target);
}

// Why planet processing cannot run for this capture (ser::PlanetRefusal, the
// filter read from the capture name), or nullopt; `info` as for MineralRefusalFor.
inline std::optional<std::string> PlanetRefusalFor( const std::string& pathUtf8, const ser::FileInfo& info,
                                                    const TargetChoice& target )
{
   const ser::CaptureName c = ser::ParseCaptureName( pathUtf8 );
   return ser::PlanetRefusal( target.target, c.filter, info, c.filterToken );
}

// The kind's gate: MineralRefusalFor or PlanetRefusalFor.
inline std::optional<std::string> PostRefusalFor( PostKind k, const std::string& pathUtf8, const ser::FileInfo& info,
                                                  const TargetChoice& target )
{
   return k == PostKind::Mineral ? MineralRefusalFor( pathUtf8, info, target ) : PlanetRefusalFor( pathUtf8, info, target );
}

// The expensive half, made once per stack (and per planet): PrepareMineral or
// PreparePlanet. Exceptions as theirs.
inline stack::MineralCache PreparePost( const PostParams& p, const ser::FloatImage& stacked, int threads,
                                        const stack::Progress& progress, const stack::StackNoise* noise )
{
   return p.kind == PostKind::Mineral ? stack::PrepareMineral( stacked, threads, progress, noise )
                                      : stack::PreparePlanet( stacked, p.target, threads, progress, noise );
}

// The cheap half on its cache at the sliders' values: FinishMineral or
// FinishPlanet. Exceptions as theirs.
inline stack::MineralOutcome FinishPost( const PostParams& p, const stack::MineralCache& cache, int threads,
                                         const stack::Progress& progress = {} )
{
   return p.kind == PostKind::Mineral ? stack::FinishMineral( cache, p.mineral, threads, progress )
                                      : stack::FinishPlanet( cache, p.planet, threads, progress );
}

// The product's own HISTORY texts, after the stack's: one per step, in the
// engine's order (seven: the mineral moon's Lunar disk .. Mineral colour, the
// planet's Planet disc .. Saturation), each "PlanetaryStack <noun>: <the step's line>".
inline std::vector<std::string> PostHistory( PostKind k, const stack::MineralOutcome& post )
{
   std::vector<std::string> h;
   for ( const std::string& line : post.history )
      h.push_back( "PlanetaryStack " + PostNoun( k ) + ": " + fits::FitsAscii( line ) );
   return h;
}

// Why the product's image is only the stack clipped (MineralOutcome::notice),
// for the console, the result line and the window; empty without a notice.
inline std::string PostNoticeText( PostKind k, const std::string& notice )
{
   if ( notice.empty() )
      return std::string();
   return PostTitle( k ) + ": " + notice + ", so the " + PostNoun( k ) +
          " image is the stack clipped to [0, 1], with no colour or sharpening.";
}

// The window's product lines, after ResultText: the colour step's line (the
// last of its history: Mineral colour's strength and chroma blur, or
// Saturation's), the Sharpen line up to its noise note (the slider's amount
// and the gains it gave), then the notice as a warning.
inline std::string PostResultText( PostKind k, const stack::MineralOutcome& post )
{
   std::string t = PostTitle( k ) + " shown: " + (post.history.empty() ? std::string() : post.history.back());
   for ( const std::string& h : post.history )
      if ( h.rfind( "Sharpen", 0 ) == 0 )
      {
         t += "\n" + h.substr( 0, h.find( "; noise:" ) );
         break;
      }
   if ( !post.notice.empty() )
      t += "\nWarning: " + PostNoticeText( k, post.notice );
   return t;
}

// The mineral moon's (PostKind::Mineral).
inline std::vector<std::string> MineralHistory( const stack::MineralOutcome& mineral )
{
   return PostHistory( PostKind::Mineral, mineral );
}
inline std::string MineralNoticeText( const std::string& notice )
{
   return PostNoticeText( PostKind::Mineral, notice );
}
inline std::string MineralResultText( const stack::MineralOutcome& mineral )
{
   return PostResultText( PostKind::Mineral, mineral );
}

// The kind's slider values: "strength 8.00, chroma blur 2.50 px, sharpen 1.50"
// or "saturation 1.00, chroma blur 1.00 px, sharpen 1.50".
inline std::string PostParamsText( const PostParams& p )
{
   if ( p.kind == PostKind::Mineral )
      return "strength " + Fixed( p.mineral.strength, 2 ) + ", chroma blur " + Fixed( p.mineral.chromaBlurSigma, 2 ) +
             " px, sharpen " + Fixed( p.mineral.sharpen, 2 );
   return "saturation " + Fixed( p.planet.saturation, 2 ) + ", chroma blur " + Fixed( p.planet.chromaBlurSigma, 2 ) +
          " px, sharpen " + Fixed( p.planet.sharpen, 2 );
}

// The console's context after the target: the product that follows the stack
// and its parameters ("; then the mineral moon: strength 8.00, chroma blur 2.50
// px, sharpen 1.50", "; then planet processing: saturation 1.00, ..."), or,
// with none (`post` null), "; no mineral moon" for the Moon and "; no planet
// processing" for anything else.
inline std::string PostContextText( ser::Target target, const PostParams* post )
{
   if ( post == nullptr )
      return PostKindFor( target ) == PostKind::Mineral ? "; no mineral moon" : "; no planet processing";
   return post->kind == PostKind::Mineral ? "; then the mineral moon: " + PostParamsText( *post )
                                          : "; then planet processing: " + PostParamsText( *post );
}

// ---- The PlanetaryStack window's text (PlanetaryStackInterface) ------------
// UTF-8: file names are shown as they are; the engine's text is ASCII.

// "Jupiter.avi: 1500 frames, 1024 x 768, 8-bit, Bayer RGGB (sidecar RG), target
// not in file name": an AVI's pattern with its source (BayerText); a SER's
// colour from its header; then the target an ASIAIR capture name gives
// ("target from file name: Moon", an unknown one "Other (Betelgeuse)"), read
// from `pathUtf8`, the full path ResolveTarget reads, so the two always agree
// (B8 review M5).
inline std::string StackInfoLine( const std::string& fileNameUtf8, const ser::FileInfo& info, const std::string& pathUtf8 )
{
   const ser::CaptureName name = ser::ParseCaptureName( pathUtf8 );
   std::string target = "target not in file name";
   if ( name.parsed )
      target = std::string( "target from file name: " ) + ser::TargetName( name.target ) +
               (name.target == ser::Target::Other ? " (" + name.targetToken + ")" : std::string());
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
          " x " + std::to_string( info.header.height ) + ", " + std::to_string( info.header.depth ) + "-bit, " + colour +
          ", " + target;
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

// The progress bar's text for the engine's three phases (Engine.h, in order)
// and the mineral moon's or planet processing's (kPhasePlanet, shown as
// "Planet processing"), of `steps` in the run (3: a stack; 4: a stack and its
// product; 1: the product of a stack already made, unnumbered):
// "Step 1 of 3: Measuring frames 1234/4088" -- the engine's total there is the
// hot-pixel frames plus every frame, so the count is of the capture's
// `frames`, after the hot-pixel frames; "Step 2 of 3: Aligning 50%" (its total
// is not a frame count); "Step 3 of 3: Stacking 312/1400" (the frames the plan
// uses); "Step 4 of 4: Mineral moon 3/7" (its steps). Another name as it is.
inline std::string ProgressText( const std::string& phase, int64_t done, int64_t total, int64_t frames, int steps )
{
   const auto step = [steps]( int i )
   {
      return steps <= 1 ? std::string() : "Step " + std::to_string( i ) + " of " + std::to_string( steps ) + ": ";
   };
   if ( phase == stack::kPhaseMineral || phase == stack::kPhasePlanet )
      return step( steps ) + (phase == stack::kPhasePlanet ? PostTitle( PostKind::Planet ) : phase) + " " + std::to_string( std::clamp<int64_t>( done, 0, total ) ) + "/" +
             std::to_string( total );
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
