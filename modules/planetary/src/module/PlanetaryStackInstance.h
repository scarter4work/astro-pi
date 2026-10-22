// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryStackInstance_h
#define __PlanetaryStackInstance_h

#include <pcl/FITSHeaderKeyword.h>
#include <pcl/MetaParameter.h> // pcl_enum
#include <pcl/ProcessImplementation.h>

#include <optional>
#include <string>

#include "Engine.h"
#include "FrameSource.h"

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
struct StackTimes
{
   std::optional<std::string> start, end;
   bool fromFrames = false; // start/end are frame times (trailer), not the header's start
};
StackTimes CaptureTimes( const planetary::ser::FrameSource& source, const planetary::stack::StackOutcome& outcome );

// The stack window's FITS keywords: SRCVIDEO, NFRAMES (measured), NCOMBINE
// (stacked), DATE-OBS / DATE-END (StackTimes), INSTRUME / TELESCOP / OBSERVER
// (the SER header, or the ASIAIR sidecar's camera as INSTRUME), and HISTORY:
// the version and file, an AVI's Bayer pattern and its source, the selection
// and alignment lines, the frame counts and time, and every summary warning.
// No BAYERPAT: the stack is debayered RGB, or the raw mosaic as mono.
FITSKeywordArray StackKeywords( const String& path, const planetary::stack::StackOutcome& outcome, const StackTimes& times );

// Writes the outcome to the Process Console: the summary warnings first (an
// undeclared mosaic's among them, so it is not missed), then the selection and
// alignment lines and the frame counts, time per step and read rate; for a
// diagnosis, its message and numbers instead of the selection.
void WriteStackResult( const String& path, const planetary::stack::StackOutcome& outcome );

// Opens outcome's image (which must be present) as a new, shown 32-bit float
// image window "<file stem>_stack" with StackKeywords, and says so on the
// console. Returns the new view's id. Throws pcl::Error if no window can be
// made; a half-made window is closed, never shown.
IsoString OpenStackWindow( const String& path, const planetary::stack::StackOutcome& outcome, const StackTimes& times );

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
   // reader's reason when the file cannot be read.
   bool ExecuteGlobal() override;

   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   String   p_filePath;
   pcl_enum p_bayerPattern;
};

} // namespace pcl

#endif // __PlanetaryStackInstance_h
