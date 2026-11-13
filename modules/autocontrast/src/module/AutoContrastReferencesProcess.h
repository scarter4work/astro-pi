// AutoContrast -- Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __AutoContrastReferencesProcess_h
#define __AutoContrastReferencesProcess_h

#include <pcl/MetaProcess.h>
#include <pcl/ProcessImplementation.h>

namespace pcl
{

// The reference-library global process. Id is STABLE. No parameters and no execution yet (Task 17).
class AutoContrastReferencesProcess : public MetaProcess
{
public:
   AutoContrastReferencesProcess();

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

class AutoContrastReferencesInstance : public ProcessImplementation
{
public:
   AutoContrastReferencesInstance( const MetaProcess* );
   AutoContrastReferencesInstance( const AutoContrastReferencesInstance& ) = default;

   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
};

extern AutoContrastReferencesProcess* TheAutoContrastReferencesProcess;

} // namespace pcl

#endif // __AutoContrastReferencesProcess_h
