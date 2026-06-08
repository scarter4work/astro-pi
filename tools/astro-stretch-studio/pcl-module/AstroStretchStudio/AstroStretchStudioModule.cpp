// ----------------------------------------------------------------------------
// AstroStretchStudio Module
// PixInsight Class Library (PCL) Module
//
// Advanced image stretching for astrophotography using:
// - Optimal Transport Stretch (OTS)
// - Starlet Arctan Stretch (SAS)
// ----------------------------------------------------------------------------

#include "AstroStretchStudioModule.h"
#include "AstroStretchStudioProcess.h"
#include "AstroStretchStudioInterface.h"

#include <pcl/Console.h>
#include <pcl/MetaModule.h>

// Module version information
#define MODULE_VERSION_MAJOR     01
#define MODULE_VERSION_MINOR     00
#define MODULE_VERSION_REVISION  00
#define MODULE_VERSION_BUILD     0001
#define MODULE_VERSION_LANGUAGE  eng

namespace pcl
{

// ----------------------------------------------------------------------------

AstroStretchStudioModule::AstroStretchStudioModule()
{
}

// ----------------------------------------------------------------------------

const char* AstroStretchStudioModule::Version() const
{
   return PCL_MODULE_VERSION( MODULE_VERSION_MAJOR,
                              MODULE_VERSION_MINOR,
                              MODULE_VERSION_REVISION,
                              MODULE_VERSION_BUILD,
                              MODULE_VERSION_LANGUAGE );
}

// ----------------------------------------------------------------------------

IsoString AstroStretchStudioModule::Name() const
{
   return "AstroStretchStudio";
}

// ----------------------------------------------------------------------------

String AstroStretchStudioModule::Description() const
{
   return "AstroStretch Studio - Advanced image stretching for astrophotography "
          "using Optimal Transport and Starlet Wavelet transforms.";
}

// ----------------------------------------------------------------------------

String AstroStretchStudioModule::Company() const
{
   return "EZ Suite";
}

// ----------------------------------------------------------------------------

String AstroStretchStudioModule::Author() const
{
   return "EZ Suite Development Team";
}

// ----------------------------------------------------------------------------

String AstroStretchStudioModule::Copyright() const
{
   return "Copyright (c) 2024 EZ Suite";
}

// ----------------------------------------------------------------------------

String AstroStretchStudioModule::TradeMarks() const
{
   return "";
}

// ----------------------------------------------------------------------------

String AstroStretchStudioModule::OriginalFileName() const
{
#ifdef __PCL_LINUX
   return "AstroStretchStudio-pxm.so";
#endif
#ifdef __PCL_FREEBSD
   return "AstroStretchStudio-pxm.so";
#endif
#ifdef __PCL_MACOSX
   return "AstroStretchStudio-pxm.dylib";
#endif
#ifdef __PCL_WINDOWS
   return "AstroStretchStudio-pxm.dll";
#endif
}

// ----------------------------------------------------------------------------

void AstroStretchStudioModule::GetReleaseDate( int& year, int& month, int& day ) const
{
   year  = 2024;
   month = 1;
   day   = 10;
}

// ----------------------------------------------------------------------------

} // namespace pcl

// ----------------------------------------------------------------------------
// Module instantiation
// ----------------------------------------------------------------------------

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::AstroStretchStudioModule;

   if ( mode == pcl::InstallMode::FullInstall )
   {
      new pcl::AstroStretchStudioProcess;
      new pcl::AstroStretchStudioInterface;
   }

   return 0;
}

// ----------------------------------------------------------------------------
