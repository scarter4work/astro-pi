// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryStackProcess_h
#define __PlanetaryStackProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class PlanetaryStackProcess : public MetaProcess
{
public:
   PlanetaryStackProcess();

   IsoString Id() const override;
   IsoString Categories() const override;
   uint32 Version() const override;
   String Description() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;

   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
};

extern PlanetaryStackProcess* ThePlanetaryStackProcess;

} // namespace pcl

#endif // __PlanetaryStackProcess_h
