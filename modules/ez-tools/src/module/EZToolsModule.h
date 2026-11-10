// EZTools -- Native PCL Module for PixInsight
// Copyright and licence: see src/EZToolsLicense.h.

#ifndef __EZToolsModule_h
#define __EZToolsModule_h

#include <pcl/MetaModule.h>

namespace pcl
{

class EZToolsModule : public MetaModule
{
public:
   EZToolsModule();

   const char* Version() const override;
   IsoString   Name() const override;
   String      Description() const override;
   String      Company() const override;
   String      Author() const override;
   String      Copyright() const override;
   String      TradeMarks() const override;
   String      OriginalFileName() const override;
   void        GetReleaseDate( int& year, int& month, int& day ) const override;
};

extern EZToolsModule* TheEZToolsModule;

} // namespace pcl

#endif // __EZToolsModule_h
