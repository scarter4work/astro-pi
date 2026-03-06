//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack Interface - Stub for compilation

#ifndef __NukeXStackInterface_h
#define __NukeXStackInterface_h

#include <pcl/ProcessInterface.h>

#include "NukeXStackInstance.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStackInterface : public ProcessInterface
{
public:

   NukeXStackInterface();
   virtual ~NukeXStackInterface();

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

   NukeXStackInstance m_instance;

   // GUI will be built out in later tasks

   friend class NukeXStackProcess;
};

// ----------------------------------------------------------------------------

extern NukeXStackInterface* TheNukeXStackInterface;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackInterface_h
