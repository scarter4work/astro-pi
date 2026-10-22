// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryStackParameters.h"

namespace pcl
{

PSFilePath*     ThePSFilePathParameter = nullptr;
PSBayerPattern* ThePSBayerPatternParameter = nullptr;

PSFilePath::PSFilePath( MetaProcess* p ) : MetaString( p )
{
   ThePSFilePathParameter = this;
}

IsoString PSFilePath::Id() const { return "filePath"; }
String PSFilePath::DefaultValue() const { return String(); }

PSBayerPattern::PSBayerPattern( MetaProcess* p ) : BayerPatternEnumeration( p )
{
   ThePSBayerPatternParameter = this;
}

} // namespace pcl
