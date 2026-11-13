// AutoContrast -- Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __AutoContrastProcess_h
#define __AutoContrastProcess_h

#include <pcl/MetaProcess.h>
#include <pcl/ProcessImplementation.h>

namespace pcl
{

// The AutoContrast view process. Id is STABLE. No parameters and no execution yet (Task 16).
class AutoContrastProcess : public MetaProcess
{
public:
   AutoContrastProcess();

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

class AutoContrastInstance : public ProcessImplementation
{
public:
   AutoContrastInstance( const MetaProcess* );
   AutoContrastInstance( const AutoContrastInstance& ) = default;

   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
};

extern AutoContrastProcess* TheAutoContrastProcess;

} // namespace pcl

#endif // __AutoContrastProcess_h
