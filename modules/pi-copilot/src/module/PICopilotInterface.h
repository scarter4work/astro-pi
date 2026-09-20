// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PICopilotInterface_h
#define __PICopilotInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>

namespace pcl
{

class PICopilotInterface : public ProcessInterface
{
public:
   PICopilotInterface();
   virtual ~PICopilotInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   InterfaceFeatures Features() const override;

   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;

   // This interface doesn't reimplement NewProcess() (increment 1 has no
   // process state to generate an instance from), so per the ProcessInterface
   // contract it must declare itself a non-generator.
   bool IsInstanceGenerator() const override;

private:

   // ── GUI Controls ──────────────────────────────────────────────
   // Increment 1: empty placeholder panel — proves the docking primitive.
   // The real chat UI lands in a later increment.

   struct GUIData
   {
      GUIData( PICopilotInterface& );

      VerticalSizer Global_Sizer;
      Label         Placeholder_Label;
   };

   GUIData* GUI = nullptr;
};

extern PICopilotInterface* ThePICopilotInterface;

} // namespace pcl

#endif // __PICopilotInterface_h
