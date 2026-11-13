// AutoContrast -- test-only probe process (compiled only with -DAC_BUILD_PROBES=ON; never shipped).
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AutoContrastProbeInstance.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

AutoContrastProbeInstance::AutoContrastProbeInstance( const MetaProcess* m ) : ProcessImplementation( m )
{
}

bool AutoContrastProbeInstance::CanExecuteGlobal( String& whyNot ) const
{
   whyNot = "AutoContrastProbe runs from a command line only.";
   return false;
}

bool AutoContrastProbeInstance::ExecuteGlobal()
{
   throw Error( "AutoContrastProbe: runs from a command line only" );
}

} // namespace pcl
