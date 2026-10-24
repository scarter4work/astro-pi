// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryModule_h
#define __PlanetaryModule_h

#include <pcl/MetaModule.h>

namespace pcl
{

class PlanetaryModule : public MetaModule
{
public:
   PlanetaryModule();

   const char* Version() const override;
   IsoString   Name() const override;
   String      Description() const override;
   String      Company() const override;
   String      Author() const override;
   String      Copyright() const override;
   String      TradeMarks() const override;
   String      OriginalFileName() const override;
   void        GetReleaseDate( int& year, int& month, int& day ) const override;
   // Before PixInsight exits (or the module is uninstalled): a stack running in
   // the PlanetaryStack window is cancelled and joined.
   void        OnUnload() override;
};

extern PlanetaryModule* ThePlanetaryModule;

} // namespace pcl

#endif // __PlanetaryModule_h
