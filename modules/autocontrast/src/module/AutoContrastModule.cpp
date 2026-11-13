// AutoContrast -- Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AutoContrastVersion.h"

// PCL's MODULE_INITIALIZE macros require these specific names; forward them
// to the versioned values in AutoContrastVersion.h.
#define MODULE_VERSION_MAJOR     AC_MODULE_VERSION_MAJOR
#define MODULE_VERSION_MINOR     AC_MODULE_VERSION_MINOR
#define MODULE_VERSION_REVISION  AC_MODULE_VERSION_RELEASE
#define MODULE_VERSION_BUILD     AC_MODULE_VERSION_BUILD
#define MODULE_VERSION_LANGUAGE  eng

#include "AutoContrastModule.h"
#include "AutoContrastProcess.h"
#include "AutoContrastReferencesProcess.h"
#ifdef AC_BUILD_PROBES
#include "probe/AutoContrastProbeProcess.h"
#endif

namespace pcl
{

AutoContrastModule* TheAutoContrastModule = nullptr;

AutoContrastModule::AutoContrastModule()
{
   TheAutoContrastModule = this;
}

const char* AutoContrastModule::Version() const
{
   return PCL_MODULE_VERSION( MODULE_VERSION_MAJOR,
                              MODULE_VERSION_MINOR,
                              MODULE_VERSION_REVISION,
                              MODULE_VERSION_BUILD,
                              MODULE_VERSION_LANGUAGE );
}

// The module id. STABLE once shipped: installs are updated by it.
IsoString AutoContrastModule::Name() const
{
   return "AutoContrast";
}

String AutoContrastModule::Description() const
{
   // Decoded as UTF-8: String( const char* ) would read non-ASCII text as ISO-8859-1.
   return String::UTF8ToUTF16( "AutoContrast for PixInsight: automatic, reference-guided contrast and colour refinement of astronomical images. "
          "This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/)." );
}

String AutoContrastModule::Company() const
{
   return "Scott Carter";
}

String AutoContrastModule::Author() const
{
   return "Scott Carter";
}

String AutoContrastModule::Copyright() const
{
   return "Copyright (c) 2026 Scott Carter";
}

String AutoContrastModule::TradeMarks() const
{
   return "AutoContrast";
}

String AutoContrastModule::OriginalFileName() const
{
#ifdef __PCL_LINUX
   return "AutoContrast-pxm.so";
#endif
#ifdef __PCL_MACOSX
   return "AutoContrast-pxm.dylib";
#endif
#ifdef __PCL_WINDOWS
   return "AutoContrast-pxm.dll";
#endif
}

void AutoContrastModule::GetReleaseDate( int& year, int& month, int& day ) const
{
   year  = 2026;
   month = 11;
   day   = 12;
}

} // namespace pcl

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::AutoContrastModule;
   // Registration order is the order PixInsight lists them in.
   new pcl::AutoContrastProcess;
   new pcl::AutoContrastReferencesProcess;
#ifdef AC_BUILD_PROBES
   new pcl::AutoContrastProbeProcess;   // test-only; compiled in only with -DAC_BUILD_PROBES=ON
#endif
   return 0;
}
