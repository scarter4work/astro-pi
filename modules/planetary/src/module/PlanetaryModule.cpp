// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryVersion.h"

// PCL's MODULE_INITIALIZE macros require these specific names; forward
// them to the versioned values in PlanetaryVersion.h.
#define MODULE_VERSION_MAJOR     PLANETARY_MODULE_VERSION_MAJOR
#define MODULE_VERSION_MINOR     PLANETARY_MODULE_VERSION_MINOR
#define MODULE_VERSION_REVISION  PLANETARY_MODULE_VERSION_REVISION
#define MODULE_VERSION_BUILD     PLANETARY_MODULE_VERSION_BUILD
#define MODULE_VERSION_LANGUAGE  eng

#include "PlanetaryModule.h"
#include "SERViewerInterface.h"
#include "SERViewerProcess.h"

namespace pcl
{

PlanetaryModule* ThePlanetaryModule = nullptr;

PlanetaryModule::PlanetaryModule()
{
   ThePlanetaryModule = this;
}

const char* PlanetaryModule::Version() const
{
   return PCL_MODULE_VERSION( MODULE_VERSION_MAJOR,
                              MODULE_VERSION_MINOR,
                              MODULE_VERSION_REVISION,
                              MODULE_VERSION_BUILD,
                              MODULE_VERSION_LANGUAGE );
}

IsoString PlanetaryModule::Name() const
{
   return "Planetary";
}

String PlanetaryModule::Description() const
{
   // Decoded as UTF-8: String( const char* ) would read any non-ASCII text as ISO-8859-1.
   return String::UTF8ToUTF16( "Planetary imaging for PixInsight. This version provides SERViewer, which opens SER planetary "
          "video files and uncompressed 8-bit greyscale or raw Bayer AVI files (such as ASIAIR captures), previews frames, opens a frame as an image "
          "and can save an AVI as a SER file. "
          "It reads only the video file you choose (and an AVI's .txt sidecar), writes only the SER file you choose, "
          "uses no network and runs no external programs. "
          "This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/)." );
}

String PlanetaryModule::Company() const
{
   return "Scott Carter";
}

String PlanetaryModule::Author() const
{
   return "Scott Carter";
}

String PlanetaryModule::Copyright() const
{
   return "Copyright (c) 2026 Scott Carter";
}

String PlanetaryModule::TradeMarks() const
{
   return "Planetary";
}

String PlanetaryModule::OriginalFileName() const
{
#ifdef __PCL_LINUX
   return "Planetary-pxm.so";
#endif
#ifdef __PCL_MACOSX
   return "Planetary-pxm.dylib";
#endif
#ifdef __PCL_WINDOWS
   return "Planetary-pxm.dll";
#endif
}

void PlanetaryModule::GetReleaseDate( int& year, int& month, int& day ) const
{
   year  = 2026;
   month = 10;
   day   = 21;
}

} // namespace pcl

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::PlanetaryModule;
   if ( mode == pcl::InstallMode::FullInstall )
   {
      new pcl::SERViewerProcess;
      new pcl::SERViewerInterface;
   }
   return 0;
}
