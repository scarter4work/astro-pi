//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#define MODULE_VERSION_MAJOR     1
#define MODULE_VERSION_MINOR     1
#define MODULE_VERSION_REVISION  0
#define MODULE_VERSION_BUILD     3
#define MODULE_VERSION_LANGUAGE  eng

#define MODULE_RELEASE_YEAR      2026
#define MODULE_RELEASE_MONTH     1
#define MODULE_RELEASE_DAY       28

#include "NukeXModule.h"
#include "NukeXProcess.h"
#include "NukeXInterface.h"
#include "NukeXStackProcess.h"
#include "NukeXStackInterface.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXModule::NukeXModule()
{
   TheNukeXModule = this;
}

// ----------------------------------------------------------------------------

const char* NukeXModule::Version() const
{
   return PCL_MODULE_VERSION( MODULE_VERSION_MAJOR,
                              MODULE_VERSION_MINOR,
                              MODULE_VERSION_REVISION,
                              MODULE_VERSION_BUILD,
                              MODULE_VERSION_LANGUAGE );
}

// ----------------------------------------------------------------------------

IsoString NukeXModule::Name() const
{
   return "NukeX";
}

// ----------------------------------------------------------------------------

String NukeXModule::Description() const
{
   return "NukeX - Intelligent Region-Aware Processing Module for PixInsight. "
          "Includes NukeX (region-aware stretching using AI segmentation) and "
          "NukeXStack (intelligent pixel selection for subframe integration "
          "using distribution fitting and ML-guided selection strategies).";
}

// ----------------------------------------------------------------------------

String NukeXModule::Company() const
{
   return "Scott Carter";
}

// ----------------------------------------------------------------------------

String NukeXModule::Author() const
{
   return "Scott Carter";
}

// ----------------------------------------------------------------------------

String NukeXModule::Copyright() const
{
   return "Copyright (c) 2026 Scott Carter";
}

// ----------------------------------------------------------------------------

String NukeXModule::TradeMarks() const
{
   return "NukeX";
}

// ----------------------------------------------------------------------------

String NukeXModule::OriginalFileName() const
{
#ifdef __PCL_FREEBSD
   return "NukeX-pxm.so";
#endif
#ifdef __PCL_LINUX
   return "NukeX-pxm.so";
#endif
#ifdef __PCL_MACOSX
   return "NukeX-pxm.dylib";
#endif
#ifdef __PCL_WINDOWS
   return "NukeX-pxm.dll";
#endif
}

// ----------------------------------------------------------------------------

void NukeXModule::GetReleaseDate( int& year, int& month, int& day ) const
{
   year  = MODULE_RELEASE_YEAR;
   month = MODULE_RELEASE_MONTH;
   day   = MODULE_RELEASE_DAY;
}

// ----------------------------------------------------------------------------

NukeXModule* TheNukeXModule = nullptr;

// Module singleton accessor - creates module on first call
NukeXModule* GetNukeXModuleInstance()
{
   static NukeXModule* s_instance = new NukeXModule;
   return s_instance;
}

// Static initializer to ensure Module global is set before IdentifyPixInsightModule
namespace {
   struct ModuleInitializer {
      ModuleInitializer() {
         GetNukeXModuleInstance();
      }
   };
   static ModuleInitializer s_moduleInit;
}

} // namespace pcl

// ----------------------------------------------------------------------------
// PCL Module Installation Routine
// ----------------------------------------------------------------------------

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   // Module instance is created via GetNukeXModuleInstance() static initializer

   if ( mode == pcl::InstallMode::FullInstall )
   {
      // Original stretch process
      new pcl::NukeXProcess;
      new pcl::NukeXInterface;

      // Stack integration process
      new pcl::NukeXStackProcess;
      new pcl::NukeXStackInterface;
   }

   return 0;
}
