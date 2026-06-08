//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// This file is part of NukeX.
//
// NukeX is free software: you can redistribute it and/or modify it under
// the terms of the MIT License.

#ifndef __NukeXProcess_h
#define __NukeXProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXProcess : public MetaProcess
{
public:

   NukeXProcess();

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

extern NukeXProcess* TheNukeXProcess;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXProcess_h
