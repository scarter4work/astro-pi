// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __SERViewerInterface_h
#define __SERViewerInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>

#include "SERViewerInstance.h"

namespace pcl
{

class SERViewerInterface : public ProcessInterface
{
public:
   SERViewerInterface();
   virtual ~SERViewerInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   InterfaceFeatures Features() const override;

   void ApplyInstance() const override;
   void ResetInstance() override;

   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;

   ProcessImplementation* NewProcess() const override;

   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;

   bool ImportProcess( const ProcessImplementation& ) override;

private:
   SERViewerInstance instance;

   struct GUIData
   {
      GUIData( SERViewerInterface& );

      VerticalSizer Global_Sizer;
   };

   GUIData* GUI = nullptr;
};

extern SERViewerInterface* TheSERViewerInterface;

} // namespace pcl

#endif // __SERViewerInterface_h
