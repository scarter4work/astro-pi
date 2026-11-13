// AutoContrast -- test-only probe process (compiled only with -DAC_BUILD_PROBES=ON; never shipped).
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __AutoContrastProbeInstance_h
#define __AutoContrastProbeInstance_h

#include <pcl/ProcessImplementation.h>

namespace pcl
{

// The probe has no parameters and no execution: it exists to be called from a command line.
class AutoContrastProbeInstance : public ProcessImplementation
{
public:
   AutoContrastProbeInstance( const MetaProcess* );
   AutoContrastProbeInstance( const AutoContrastProbeInstance& ) = default;

   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
};

} // namespace pcl

#endif
