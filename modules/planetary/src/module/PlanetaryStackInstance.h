// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryStackInstance_h
#define __PlanetaryStackInstance_h

#include <pcl/FITSHeaderKeyword.h>
#include <pcl/Image.h>
#include <pcl/MetaParameter.h> // pcl_enum
#include <pcl/ProcessImplementation.h>

#include <optional>
#include <string>

#include "Engine.h"
#include "FrameSource.h"
#include "StackCards.h"

namespace pcl
{

// The worker threads the engine runs with: PixInsight's own allowance for a
// module (Thread::NumberOfThreads: the logical processors less the threads
// already running, capped by Preferences > Parallel Processing), so the stack
// honours the user's setting like every other process. The engine caps its
// workers further by memory (stack::PlanMemory); its result does not depend on
// the count.
int StackThreads();

// The capture's time span for DATE-OBS / DATE-END, from a SER only (an AVI has
// no trustworthy clock): the UTC times of the first and last frame stacked
// when the file has a timestamp trailer; else the header's DateTime_UTC as the
// start (no end). Empty for an AVI, a diagnosed outcome, or a SER without times.
// PCL-free in body, so the interface's worker thread calls it too.
using StackTimes = planetary::stackcards::StackTimes;
StackTimes CaptureTimes( const planetary::ser::FrameSource& source, const planetary::stack::StackOutcome& outcome );

using TargetChoice = planetary::stackcards::TargetChoice;

// The stack window's FITS keywords: stackcards::StackCards (OBJECT unless the
// target is Other, SRCVIDEO, NFRAMES,
// NCOMBINE, DATE-OBS / DATE-END never for an AVI, INSTRUME / TELESCOP /
// OBSERVER) and stackcards::StackHistory as HISTORY cards.
FITSKeywordArray StackKeywords( const String& path, const planetary::stack::StackOutcome& outcome, const StackTimes& times,
                                const TargetChoice& target );

// The mineral moon's parameters from the mineralStrength, chromaBlur and
// mineralSharpen values. Throws pcl::Error, prefixed with `process`, for a
// value outside its parameter's range (a script can set any number).
planetary::stack::MineralParams MineralParamsFor( double strength, double chromaBlur, double sharpen, const char* process );

// Writes the context lines before a run to the Process Console: version, file,
// frames, size, colour, depth and threads; for an AVI, its Bayer pattern and
// where it came from; the target and where it came from, and the mineral moon's
// parameters when `mineral` is given (nullptr: no mineral moon).
void WriteStackContext( const String& path, const planetary::ser::FileInfo& info, int threads, const TargetChoice& target,
                        const planetary::stack::MineralParams* mineral );

// Writes the outcome to the Process Console: the summary warnings first, one
// line per condition as stackcards::ShownWarnings gives them for `where` (an
// undeclared mosaic's among them, so it is not missed), then the selection and
// alignment lines and the frame counts, time per step and read rate; for a
// diagnosis, its message and numbers instead of the selection.
void WriteStackResult( const String& path, const planetary::stack::StackOutcome& outcome,
                       planetary::stackcards::WarningsFor where = planetary::stackcards::WarningsFor::Process );

// Opens outcome's image (which must be present) as a new, shown 32-bit float
// image window "<file stem>_stack" with StackKeywords (ShowNewImageWindow: a
// half-made window is closed, never shown; the image and keywords are built
// first, so the window exists only for PCL's own calls), and says so on the console.
// Returns the new view's id. Throws pcl::Error if no window can be made.
IsoString OpenStackWindow( const String& path, const planetary::stack::StackOutcome& outcome, const StackTimes& times,
                           const TargetChoice& target );

// The mineral moon's window, made in two steps so that a caller opening it
// beside the stack window can build everything that can fail before either
// window is shown (both or none; B8 review M1, M2):
// MakeMineralWindow: the 32-bit float RGB image and its keywords -- the stack's
// (StackKeywords), then stackcards::MineralHistory's seven lines as HISTORY.
// Throws pcl::Error for an image that is not RGB.
// ShowMineralWindow: shows it as "<file stem>_mineral" (ShowNewImageWindow) and
// says so on the console; a notice (no disc: the image is the stack clipped) is
// written as a warning first, so the result line is never read alone. Returns
// the new view's id.
struct MineralWindow
{
   Image            image;
   FITSKeywordArray keywords;
};
MineralWindow MakeMineralWindow( const String& path, const planetary::stack::StackOutcome& outcome, const StackTimes& times,
                                 const TargetChoice& target, const planetary::stack::MineralOutcome& mineral );
IsoString ShowMineralWindow( const String& path, const MineralWindow& window, const planetary::stack::MineralOutcome& mineral );

class PlanetaryStackInstance : public ProcessImplementation
{
public:
   PlanetaryStackInstance( const MetaProcess* );
   PlanetaryStackInstance( const PlanetaryStackInstance& );

   void Assign( const ProcessImplementation& ) override;

   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   // Stacks filePath (stack::RunStack) with StandardStatus progress on the
   // Process Console, whose Abort button cancels (ProcessAborted; no window,
   // every engine thread joined). Opens the stack (OpenStackWindow), or throws
   // pcl::Error with the diagnosis when the capture gives no image, or with the
   // reader's reason when the file cannot be read. The target is p_target, or
   // for Auto the capture name's (stackcards::ResolveTarget). With
   // p_mineralMoon: refused before stacking (pcl::Error with
   // stackcards::MineralRefusalFor's reason, from the reader's info after the
   // Bayer pattern is resolved) unless the capture is a colour Moon not taken
   // through a narrowband or IR-pass filter;
   // else the mineral moon (stack::MakeMineralMoon, p_mineralStrength,
   // p_chromaBlur, p_mineralSharpen; the stack's own noise, StackOutcome::noise)
   // is made after the stack, with the same progress and Abort,
   // and both windows open only once both images exist (an Abort during the
   // mineral moon leaves no window).
   bool ExecuteGlobal() override;

   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   String   p_filePath;
   pcl_enum p_bayerPattern;
   pcl_enum p_target;
   pcl_bool p_mineralMoon;
   double   p_mineralStrength;
   double   p_chromaBlur;
   double   p_mineralSharpen;
};

} // namespace pcl

#endif // __PlanetaryStackInstance_h
