//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStretch Interface - Stub for compilation

#ifndef __NukeXStretchInterface_h
#define __NukeXStretchInterface_h

#include <pcl/ProcessInterface.h>

#include "NukeXStretchInstance.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStretchInterface : public ProcessInterface
{
public:

   NukeXStretchInterface();
   virtual ~NukeXStretchInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   String IconImageSVGFile() const override;
   InterfaceFeatures Features() const override;
   void ApplyInstance() const override;
   void ResetInstance() override;
   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& /*flags*/ ) override;
   ProcessImplementation* NewProcess() const override;
   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;
   bool ImportProcess( const ProcessImplementation& ) override;

private:

   NukeXStretchInstance m_instance;

   // GUI will be built out when stretch is ported (Task 4.x)

   friend class NukeXStretchProcess;
};

// ----------------------------------------------------------------------------

extern NukeXStretchInterface* TheNukeXStretchInterface;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStretchInterface_h
