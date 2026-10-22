// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryStackParameters_h
#define __PlanetaryStackParameters_h

#include <pcl/MetaParameter.h>

#include "BayerPatternParameter.h"

namespace pcl
{

// Absolute path of the .ser or .avi capture to stack (the format is chosen by content).
class PSFilePath : public MetaString
{
public:
   PSFilePath( MetaProcess* );
   IsoString Id() const override;
   String DefaultValue() const override;
};

// The shared bayerPattern parameter (BayerPatternParameter.h), as in SERViewer:
// a pattern set here wins over the AVI's sidecar.
class PSBayerPattern : public BayerPatternEnumeration
{
public:
   PSBayerPattern( MetaProcess* );
};

extern PSFilePath*     ThePSFilePathParameter;
extern PSBayerPattern* ThePSBayerPatternParameter;

} // namespace pcl

#endif // __PlanetaryStackParameters_h
