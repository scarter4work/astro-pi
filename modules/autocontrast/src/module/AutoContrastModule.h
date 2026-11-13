// AutoContrast -- Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __AutoContrastModule_h
#define __AutoContrastModule_h

#include <pcl/MetaModule.h>

namespace pcl
{

class AutoContrastModule : public MetaModule
{
public:
   AutoContrastModule();

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

extern AutoContrastModule* TheAutoContrastModule;

} // namespace pcl

#endif // __AutoContrastModule_h
