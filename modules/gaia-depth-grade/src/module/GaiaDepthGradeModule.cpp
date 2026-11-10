// GaiaDepthGrade -- Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "GaiaDepthGradeVersion.h"

// PCL's MODULE_INITIALIZE macros require these specific names; forward them
// to the versioned values in GaiaDepthGradeVersion.h.
#define MODULE_VERSION_MAJOR     GDG_MODULE_VERSION_MAJOR
#define MODULE_VERSION_MINOR     GDG_MODULE_VERSION_MINOR
#define MODULE_VERSION_REVISION  GDG_MODULE_VERSION_REVISION
#define MODULE_VERSION_BUILD     GDG_MODULE_VERSION_BUILD
#define MODULE_VERSION_LANGUAGE  eng

#include "GaiaDepthGradeModule.h"

#ifdef GDG_PROBES
#include "probe/ProbeProcess.h"
#endif

namespace pcl
{

GaiaDepthGradeModule* TheGaiaDepthGradeModule = nullptr;

GaiaDepthGradeModule::GaiaDepthGradeModule()
{
   TheGaiaDepthGradeModule = this;
}

const char* GaiaDepthGradeModule::Version() const
{
   return PCL_MODULE_VERSION( MODULE_VERSION_MAJOR,
                              MODULE_VERSION_MINOR,
                              MODULE_VERSION_REVISION,
                              MODULE_VERSION_BUILD,
                              MODULE_VERSION_LANGUAGE );
}

// The module id. STABLE once shipped: installs are updated by it.
IsoString GaiaDepthGradeModule::Name() const
{
   return "GaiaDepthGrade";
}

String GaiaDepthGradeModule::Description() const
{
   // Decoded as UTF-8: String( const char* ) would read non-ASCII text as ISO-8859-1.
   return String::UTF8ToUTF16( "Gaia Depth Grade for PixInsight: grades the stars of an image by their measured Gaia "
          "distances, and the nebula by a structure-driven, Gaia-calibrated depth. "
          "This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/)." );
}

String GaiaDepthGradeModule::Company() const
{
   return "Scott Carter";
}

String GaiaDepthGradeModule::Author() const
{
   return "Scott Carter";
}

String GaiaDepthGradeModule::Copyright() const
{
   return "Copyright (c) 2026 Scott Carter";
}

String GaiaDepthGradeModule::TradeMarks() const
{
   return "GaiaDepthGrade";
}

String GaiaDepthGradeModule::OriginalFileName() const
{
#ifdef __PCL_LINUX
   return "GaiaDepthGrade-pxm.so";
#endif
#ifdef __PCL_MACOSX
   return "GaiaDepthGrade-pxm.dylib";
#endif
#ifdef __PCL_WINDOWS
   return "GaiaDepthGrade-pxm.dll";
#endif
}

void GaiaDepthGradeModule::GetReleaseDate( int& year, int& month, int& day ) const
{
   year  = 2026;
   month = 11;
   day   = 10;
}

} // namespace pcl

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::GaiaDepthGradeModule;
   // No processes yet (plan Task 2): GaiaDepthGrade arrives in Task 16.
#ifdef GDG_PROBES
   // Test-only probe process; compiled only into host test builds (GDG_BUILD_PROBES).
   if ( mode == pcl::InstallMode::FullInstall )
      new pcl::GDGProbeProcess;
#endif
   return 0;
}
