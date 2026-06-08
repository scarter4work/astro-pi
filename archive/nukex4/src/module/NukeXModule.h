// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeXModule_h
#define __NukeXModule_h

#include <pcl/MetaModule.h>

namespace pcl
{

class NukeXModule : public MetaModule
{
public:
   NukeXModule();

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

extern NukeXModule* TheNukeXModule;

} // namespace pcl

#endif // __NukeXModule_h
