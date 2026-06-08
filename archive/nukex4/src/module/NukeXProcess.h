// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeXProcess_h
#define __NukeXProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class NukeXProcess : public MetaProcess
{
public:
   NukeXProcess();

   IsoString Id() const override;
   IsoString Categories() const override;
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

   // Phase 8 — PCL Settings-backed opt-out under key
   // "NukeX/Phase8/RatingPopupSuppressed" (see NukeXProcess.cpp).
   // "Suppressed" means "don't auto-open the rating popup after Execute".
   // The "Rate last run" button on NukeXInterface still works regardless.
   // Methods are const because persistence lives in PCL Settings, not on
   // member state -- the Process singleton itself is stateless.
   bool rating_popup_suppressed() const;
   void set_rating_popup_suppressed(bool suppressed) const;
};

extern NukeXProcess* TheNukeXProcess;

} // namespace pcl

#endif // __NukeXProcess_h
