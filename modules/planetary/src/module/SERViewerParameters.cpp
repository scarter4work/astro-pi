// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SERViewerParameters.h"

namespace pcl
{

SVFilePath*     TheSVFilePathParameter = nullptr;
SVFrameIndex*   TheSVFrameIndexParameter = nullptr;
SVDebayer*      TheSVDebayerParameter = nullptr;
SVBayerPattern* TheSVBayerPatternParameter = nullptr;
SVSaveSerPath*  TheSVSaveSerPathParameter = nullptr;

SVFilePath::SVFilePath( MetaProcess* p ) : MetaString( p )
{
   TheSVFilePathParameter = this;
}

IsoString SVFilePath::Id() const { return "filePath"; }
String SVFilePath::DefaultValue() const { return String(); }

SVFrameIndex::SVFrameIndex( MetaProcess* p ) : MetaInt32( p )
{
   TheSVFrameIndexParameter = this;
}

IsoString SVFrameIndex::Id() const { return "frameIndex"; }
double SVFrameIndex::DefaultValue() const { return 0; }
double SVFrameIndex::MinimumValue() const { return 0; }
double SVFrameIndex::MaximumValue() const { return int32_max; }

SVDebayer::SVDebayer( MetaProcess* p ) : MetaBoolean( p )
{
   TheSVDebayerParameter = this;
}

IsoString SVDebayer::Id() const { return "debayer"; }
bool SVDebayer::DefaultValue() const { return true; }

SVBayerPattern::SVBayerPattern( MetaProcess* p ) : BayerPatternEnumeration( p )
{
   TheSVBayerPatternParameter = this;
}

SVSaveSerPath::SVSaveSerPath( MetaProcess* p ) : MetaString( p )
{
   TheSVSaveSerPathParameter = this;
}

IsoString SVSaveSerPath::Id() const { return "saveSerPath"; }
String SVSaveSerPath::DefaultValue() const { return String(); }

} // namespace pcl
