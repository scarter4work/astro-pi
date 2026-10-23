// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PICopilotModule_h
#define __PICopilotModule_h

#include <pcl/MetaModule.h>

namespace pcl
{

class PICopilotModule : public MetaModule
{
public:
   PICopilotModule();

   const char* Version() const override;
   IsoString   Name() const override;
   String      Description() const override;
   String      Company() const override;
   String      Author() const override;
   String      Copyright() const override;
   String      TradeMarks() const override;
   String      OriginalFileName() const override;
   void        GetReleaseDate( int& year, int& month, int& day ) const override;
   void        OnLoad() override;
   void        OnUnload() override;
};

extern PICopilotModule* ThePICopilotModule;

// True from the first line of OnUnload on: PixInsight is quitting. OnUnload can
// run INSIDE our own code (any event pump dispatches File > Quit, see
// JourneyService), so code that resumes afterwards checks this and starts
// nothing new: no model request, no journey write-up, no further tool.
// Root thread.
bool PICopilotUnloadStarted();

} // namespace pcl

#endif // __PICopilotModule_h
