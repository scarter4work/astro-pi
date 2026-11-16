// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2025-2026, Brian Scott Carter. BSD 3-Clause License.

#ifndef __SERViewerProcess_h
#define __SERViewerProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class SERViewerProcess : public MetaProcess
{
public:
   SERViewerProcess();

   IsoString Id() const override;
   IsoString Categories() const override;
   uint32 Version() const override;
   String Description() const override;
   ProcessInterface* DefaultInterface() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;

   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
};

extern SERViewerProcess* TheSERViewerProcess;

} // namespace pcl

#endif // __SERViewerProcess_h
