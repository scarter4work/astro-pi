// EZTools -- Native PCL Module for PixInsight
// Copyright and licence: see src/EZToolsLicense.h.

#include "EZToolsVersion.h"

// PCL's MODULE_INITIALIZE macros require these specific names; forward them
// to the versioned values in EZToolsVersion.h.
#define MODULE_VERSION_MAJOR     EZTOOLS_MODULE_VERSION_MAJOR
#define MODULE_VERSION_MINOR     EZTOOLS_MODULE_VERSION_MINOR
#define MODULE_VERSION_REVISION  EZTOOLS_MODULE_VERSION_REVISION
#define MODULE_VERSION_BUILD     EZTOOLS_MODULE_VERSION_BUILD
#define MODULE_VERSION_LANGUAGE  eng

#include "EZToolsModule.h"
#include "EZHazeKillInterface.h"
#include "EZHazeKillProcess.h"
#include "EZToolsLicense.h"

namespace pcl
{

EZToolsModule* TheEZToolsModule = nullptr;

EZToolsModule::EZToolsModule()
{
   TheEZToolsModule = this;
}

const char* EZToolsModule::Version() const
{
   return PCL_MODULE_VERSION( MODULE_VERSION_MAJOR,
                              MODULE_VERSION_MINOR,
                              MODULE_VERSION_REVISION,
                              MODULE_VERSION_BUILD,
                              MODULE_VERSION_LANGUAGE );
}

// The module id. STABLE once shipped: installs are updated by it.
IsoString EZToolsModule::Name() const
{
   return "EZTools";
}

String EZToolsModule::Description() const
{
   // Decoded as UTF-8: String( const char* ) would read non-ASCII text as ISO-8859-1.
   return String::UTF8ToUTF16( "EZ Tools for PixInsight. This version provides one process, EZHazeKill: it removes the flat "
          "background haze (a residual sky pedestal and colour cast) left after gradient extraction, measured on the "
          "darkest sky of the image, with a Real-Time Preview. It changes only the target image: no network, no "
          "external programs, no files written. Licensed under " EZTOOLS_LICENSE " (" EZTOOLS_LICENSE_URL "). "
          "This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its "
          "contributors (https://pixinsight.com/)." );
}

String EZToolsModule::Company() const
{
   return "Scott Carter";
}

String EZToolsModule::Author() const
{
   return "Scott Carter";
}

String EZToolsModule::Copyright() const
{
   return EZTOOLS_COPYRIGHT ". Licensed under " EZTOOLS_LICENSE ".";
}

String EZToolsModule::TradeMarks() const
{
   return "EZTools";
}

String EZToolsModule::OriginalFileName() const
{
#ifdef __PCL_LINUX
   return "EZTools-pxm.so";
#endif
#ifdef __PCL_MACOSX
   return "EZTools-pxm.dylib";
#endif
#ifdef __PCL_WINDOWS
   return "EZTools-pxm.dll";
#endif
}

void EZToolsModule::GetReleaseDate( int& year, int& month, int& day ) const
{
   year  = 2026;
   month = 11;
   day   = 10;
}

} // namespace pcl

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::EZToolsModule;
   if ( mode == pcl::InstallMode::FullInstall )
   {
      new pcl::EZHazeKillProcess;
      new pcl::EZHazeKillInterface;
   }
   return 0;
}
