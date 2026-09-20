// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PICopilotProcess_h
#define __PICopilotProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class PICopilotProcess : public MetaProcess
{
public:
   PICopilotProcess();

   IsoString Id() const override;
   IsoString Categories() const override;
   uint32 Version() const override;
   String Description() const override;
   ProcessInterface* DefaultInterface() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;

   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
   bool NeedsValidation() const override;
};

extern PICopilotProcess* ThePICopilotProcess;

} // namespace pcl

#endif // __PICopilotProcess_h
