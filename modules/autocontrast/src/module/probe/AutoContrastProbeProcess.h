// AutoContrast -- test-only probe process (compiled only with -DAC_BUILD_PROBES=ON; never shipped).
// Copyright (c) 2026 Scott Carter. MIT License.
//
// Reached from a PixInsight command-line script (.scp) run with -r= under --automation-mode:
//    AutoContrastProbe --case=<id> --out=<json file>
// Each case writes its result as JSON to the --out file; the test harness reads it. No PJSR.

#ifndef __AutoContrastProbeProcess_h
#define __AutoContrastProbeProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class AutoContrastProbeProcess : public MetaProcess
{
public:
   AutoContrastProbeProcess();

   IsoString Id() const override;
   IsoString Categories() const override;
   uint32 Version() const override;
   String Description() const override;
   ProcessInterface* DefaultInterface() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;

   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
   bool CanProcessCommandLines() const override;
   int ProcessCommandLine( const StringList& argv ) const override;
};

} // namespace pcl

#endif
