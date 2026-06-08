//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack - Intelligent pixel selection for subframe integration

#ifndef __NukeXStackProcess_h
#define __NukeXStackProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStackProcess : public MetaProcess
{
public:

   NukeXStackProcess();

   IsoString Id() const override;
   IsoString Category() const override;
   uint32 Version() const override;
   String Description() const override;
   String IconImageSVGFile() const override;
   ProcessInterface* DefaultInterface() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;

   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
   bool IsAssignable() const override;
   bool NeedsInitialization() const override;
   bool NeedsValidation() const override;
   bool PrefersGlobalExecution() const override;
};

// ----------------------------------------------------------------------------

extern NukeXStackProcess* TheNukeXStackProcess;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackProcess_h
