// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __SERViewerInstance_h
#define __SERViewerInstance_h

#include <pcl/MetaParameter.h> // pcl_bool
#include <pcl/ProcessImplementation.h>

#include <cstdint>

#include "Debayer.h"
#include "SerFile.h"

namespace pcl
{

// Frame `index` of `file` as normalised float samples, the way SERViewer shows
// it: a Bayer file debayered to RGB when `debayer` is true, else its raw CFA as
// one channel; every other colour layout as stored (BGR already reordered to
// RGB by the reader). Throws planetary::ser::SerError with the real reason.
planetary::ser::FloatImage LoadFrameForDisplay( const planetary::ser::SerFile& file, int64_t index, bool debayer );

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
};

} // namespace pcl

#endif // __SERViewerInstance_h
