// EZTools -- EZHazeKill process parameters.
// Copyright and licence: see src/EZToolsLicense.h.
//
// Parameter ids are the EZHazeKill.js 1.1.0 Parameters keys and are STABLE: never rename them.

#ifndef __EZHazeKillParameters_h
#define __EZHazeKillParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

PCL_BEGIN_LOCAL

class EZHKAmount : public MetaDouble
{
public:
   EZHKAmount( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern EZHKAmount* TheEZHKAmountParameter;

class EZHKNeutralize : public MetaDouble
{
public:
   EZHKNeutralize( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern EZHKNeutralize* TheEZHKNeutralizeParameter;

class EZHKTargetLevel : public MetaDouble
{
public:
   EZHKTargetLevel( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};
extern EZHKTargetLevel* TheEZHKTargetLevelParameter;

PCL_END_LOCAL

} // namespace pcl

#endif
