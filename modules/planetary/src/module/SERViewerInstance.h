// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __SERViewerInstance_h
#define __SERViewerInstance_h

#include <pcl/MetaParameter.h> // pcl_bool
#include <pcl/ProcessImplementation.h>

#include <cstdint>
#include <string>

#include "Debayer.h"
#include "FrameSource.h"
#include "SerExport.h"

namespace pcl
{

struct DisplayFrame
{
   planetary::ser::FloatImage image;
   std::string rangeWarning; // planetary::ser::FrameRangeWarning of the frame; empty when it fits
};

// Frame `index` of `file` as float samples normalised by the file's normDepth,
// the way SERViewer shows it: a Bayer file debayered to RGB when `debayer` is
// true, else its raw CFA as one channel; every other colour layout as stored
// (BGR already reordered to RGB by the reader). Throws
// planetary::ser::SerError with the real reason.
DisplayFrame LoadFrameForDisplay( const planetary::ser::FrameSource& file, int64_t index, bool debayer );

// The reader's Bayer choice for a bayerPattern parameter value (SVBayerPattern
// element index). Throws pcl::Error for a value outside the enumeration.
planetary::ser::BayerChoice BayerChoiceFor( pcl_enum bayerPattern );

// Where ConvertToSer shows its progress and takes a cancel from.
enum class SaveProgress
{
   // StandardStatus on the Process Console, whose Abort button cancels. Only
   // inside process execution (ExecuteGlobal): PixInsight refuses
   // Console::EnableAbort() anywhere else ("EnableAbort(): API function error",
   // measured from an interface button handler, PI 1.9.5).
   Console,
   // ProgressBarStatus: a modal progress dialog whose Cancel button cancels,
   // for the interface (the PCL pattern for interface-side work, as in Blink).
   Dialog
};

// Save as SER, shared by saveSerPath (ExecuteGlobal) and the interface's Save as
// SER... button: writes the AVI `source` (opened from `path`) as a SER at
// `savePath` with planetary::ser::ExportSer. The conversion context goes on the
// Process Console before the export, then progress as `progressIn` says (a
// cancel removes the partial file and throws ProcessAborted), then a console
// line saying what was written. Opens no image window. Throws pcl::Error with
// the reason (SER input, or any ExportSer failure).
planetary::ser::ExportResult ConvertToSer( const planetary::ser::FrameSource& source, const String& path,
                                           const String& savePath, bool allowOverwrite, SaveProgress progressIn );

class SERViewerInstance : public ProcessImplementation
{
public:
   SERViewerInstance( const MetaProcess* );
   SERViewerInstance( const SERViewerInstance& );

   void Assign( const ProcessImplementation& ) override;

   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;

   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

   String   p_filePath;
   int32    p_frameIndex;
   pcl_bool p_debayer;
   pcl_enum p_bayerPattern;
   String   p_saveSerPath;
};

} // namespace pcl

#endif // __SERViewerInstance_h
