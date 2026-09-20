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
   return "PI Copilot v" PICOPILOT_STR(PICOPILOT_MODULE_VERSION_MAJOR) " — In-app AI assistant for PixInsight. "
          "Native PCL module skeleton (empty dockable panel; chat UI lands in a later increment).";
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
   month = 9;
   day   = 20;
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
