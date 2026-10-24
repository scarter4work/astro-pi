// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotVersion.h"

// PCL's MODULE_INITIALIZE macros require these specific names; forward
// them to the versioned values in PICopilotVersion.h.
#define MODULE_VERSION_MAJOR     PICOPILOT_MODULE_VERSION_MAJOR
#define MODULE_VERSION_MINOR     PICOPILOT_MODULE_VERSION_MINOR
#define MODULE_VERSION_REVISION  PICOPILOT_MODULE_VERSION_REVISION
#define MODULE_VERSION_BUILD     PICOPILOT_MODULE_VERSION_BUILD
#define MODULE_VERSION_LANGUAGE  eng

#include "PICopilotModule.h"
#include "PICopilotProcess.h"
#include "PICopilotInterface.h"
#include "JourneyConstants.h"
#include "JourneySpikeProbe.h"
#include "JourneyTracker.h"
#include "CopilotSettings.h"

namespace pcl
{

PICopilotModule::PICopilotModule()
{
   ThePICopilotModule = this;
}

const char* PICopilotModule::Version() const
{
   return PCL_MODULE_VERSION( MODULE_VERSION_MAJOR,
                              MODULE_VERSION_MINOR,
                              MODULE_VERSION_REVISION,
                              MODULE_VERSION_BUILD,
                              MODULE_VERSION_LANGUAGE );
}

IsoString PICopilotModule::Name() const
{
   return "PICopilot";
}

String PICopilotModule::Description() const
{
   // UTF-8 source text (the em dash): String( const char* ) would decode it as ISO-8859-1.
   return String::UTF8ToUTF16( "PI Copilot " PICOPILOT_VERSION_STRING " \xE2\x80\x94 In-app AI assistant for PixInsight. "
          "Chat that sees the active view, streams its replies and works on your images: Copilot (acts, undoable), Guided (asks first), Advisor (read-only). "
          "Can start installed, signed PixInsight scripts (you confirm every run); never runs model-written code. "
          "Records each image's processing journey from the stacked master; keep your best as a process icon set, a recipe and a write-up, and replay it on new data. "
          "Sends data to Anthropic (api.anthropic.com) only after you agree. "
          "This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/)." );
}

String PICopilotModule::Company() const
{
   return "Scott Carter";
}

String PICopilotModule::Author() const
{
   return "Scott Carter";
}

String PICopilotModule::Copyright() const
{
   return "Copyright (c) 2026 Scott Carter";
}

String PICopilotModule::TradeMarks() const
{
   return "PICopilot";
}

String PICopilotModule::OriginalFileName() const
{
#ifdef __PCL_LINUX
   return "PICopilot-pxm.so";
#endif
#ifdef __PCL_MACOSX
   return "PICopilot-pxm.dylib";
#endif
#ifdef __PCL_WINDOWS
   return "PICopilot-pxm.dll";
#endif
}

void PICopilotModule::GetReleaseDate( int& year, int& month, int& day ) const
{
   year  = 2026;
   month = 10;
   day   = 24;
}

void PICopilotModule::OnLoad()
{
   // Test-only (no-op unless run by test/run-selftest.sh): the plan's Task 1
   // spike records which image notifications and timer ticks reach the module
   // while the PI Copilot panel has never been opened.
   ArmJourneySpikeProbeIfSelfTest();
   // 0.2.2.0: settings of removed features (PICopilot/RunPjsrEnabled) never linger.
   CopilotSettings::RemoveRetiredSettings();
   // Image journey (0.2.0.0): always recording, panel open or not (Ruling 22).
   if ( PICopilotJourneyServiceStartsOnLoad )
      JourneyService::Instance().Start();
}

namespace
{
bool g_unloadStarted = false;
} // namespace

bool PICopilotUnloadStarted()
{
   return g_unloadStarted;
}

void PICopilotModule::OnUnload()
{
   g_unloadStarted = true;   // first: whatever resumes after this call (see the header) starts nothing new
   JourneyService::Instance().Stop();
   DisarmJourneySpikeProbe();
}

} // namespace pcl

namespace pcl {
PICopilotModule* ThePICopilotModule = nullptr;
} // namespace pcl

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::PICopilotModule;
   if ( mode == pcl::InstallMode::FullInstall )
   {
      new pcl::PICopilotProcess;
      new pcl::PICopilotInterface;
   }
   return 0;
}
