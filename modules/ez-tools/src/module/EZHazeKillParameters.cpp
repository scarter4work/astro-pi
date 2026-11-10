// EZTools -- EZHazeKill process parameters.
// Copyright and licence: see src/EZToolsLicense.h.

#include "EZHazeKillParameters.h"

#include "hazekill/HazeKill.h"

namespace pcl
{

EZHKAmount*      TheEZHKAmountParameter = nullptr;
EZHKNeutralize*  TheEZHKNeutralizeParameter = nullptr;
EZHKTargetLevel* TheEZHKTargetLevelParameter = nullptr;

EZHKAmount::EZHKAmount( MetaProcess* p ) : MetaDouble( p )
{
   TheEZHKAmountParameter = this;
}

IsoString EZHKAmount::Id() const { return "amount"; }
int EZHKAmount::Precision() const { return 2; }
double EZHKAmount::DefaultValue() const { return eztools::hazekill::Settings().amount; }
double EZHKAmount::MinimumValue() const { return 0.0; }
double EZHKAmount::MaximumValue() const { return 1.0; }

EZHKNeutralize::EZHKNeutralize( MetaProcess* p ) : MetaDouble( p )
{
   TheEZHKNeutralizeParameter = this;
}

IsoString EZHKNeutralize::Id() const { return "neutralize"; }
int EZHKNeutralize::Precision() const { return 2; }
double EZHKNeutralize::DefaultValue() const { return eztools::hazekill::Settings().neutralize; }
double EZHKNeutralize::MinimumValue() const { return 0.0; }
double EZHKNeutralize::MaximumValue() const { return 1.0; }

EZHKTargetLevel::EZHKTargetLevel( MetaProcess* p ) : MetaDouble( p )
{
   TheEZHKTargetLevelParameter = this;
}

IsoString EZHKTargetLevel::Id() const { return "targetLevel"; }
int EZHKTargetLevel::Precision() const { return 3; }
double EZHKTargetLevel::DefaultValue() const { return eztools::hazekill::Settings().target; }
double EZHKTargetLevel::MinimumValue() const { return 0.0; }
double EZHKTargetLevel::MaximumValue() const { return 0.5; }

} // namespace pcl
