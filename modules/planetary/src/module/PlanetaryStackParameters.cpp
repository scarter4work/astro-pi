// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryStackParameters.h"

#include <pcl/ErrorHandler.h>

#include "Mineral.h"

namespace ser = planetary::ser;

namespace pcl
{

PSFilePath*        ThePSFilePathParameter = nullptr;
PSBayerPattern*    ThePSBayerPatternParameter = nullptr;
PSTarget*          ThePSTargetParameter = nullptr;
PSMineralMoon*     ThePSMineralMoonParameter = nullptr;
PSMineralStrength* ThePSMineralStrengthParameter = nullptr;
PSChromaBlur*      ThePSChromaBlurParameter = nullptr;
PSMineralSharpen*  ThePSMineralSharpenParameter = nullptr;

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

// ----------------------------------------------------------------------------

PSTarget::PSTarget( MetaProcess* p ) : MetaEnumeration( p )
{
   ThePSTargetParameter = this;
}

IsoString PSTarget::Id() const { return "target"; }
size_type PSTarget::NumberOfElements() const { return NumberOfItems; }

IsoString PSTarget::ElementId( size_type i ) const
{
   switch ( i )
   {
   default:
   case Auto:    return "TargetAuto";
   case Other:   return "TargetOther";
   case Moon:    return "TargetMoon";
   case Mercury: return "TargetMercury";
   case Venus:   return "TargetVenus";
   case Mars:    return "TargetMars";
   case Jupiter: return "TargetJupiter";
   case Saturn:  return "TargetSaturn";
   case Uranus:  return "TargetUranus";
   case Neptune: return "TargetNeptune";
   }
}

int PSTarget::ElementValue( size_type i ) const { return int( i ); }
size_type PSTarget::DefaultValueIndex() const { return size_type( Default ); }

std::optional<ser::Target> TargetFor( pcl_enum target, const char* process )
{
   switch ( target )
   {
   case PSTarget::Auto:    return std::nullopt;
   case PSTarget::Other:   return ser::Target::Other;
   case PSTarget::Moon:    return ser::Target::Moon;
   case PSTarget::Mercury: return ser::Target::Mercury;
   case PSTarget::Venus:   return ser::Target::Venus;
   case PSTarget::Mars:    return ser::Target::Mars;
   case PSTarget::Jupiter: return ser::Target::Jupiter;
   case PSTarget::Saturn:  return ser::Target::Saturn;
   case PSTarget::Uranus:  return ser::Target::Uranus;
   case PSTarget::Neptune: return ser::Target::Neptune;
   default:
      throw Error( String( process ) + ": invalid target value " + String( int( target ) ) );
   }
}

// ----------------------------------------------------------------------------

PSMineralMoon::PSMineralMoon( MetaProcess* p ) : MetaBoolean( p )
{
   ThePSMineralMoonParameter = this;
}

IsoString PSMineralMoon::Id() const { return "mineralMoon"; }
bool PSMineralMoon::DefaultValue() const { return false; }

PSMineralStrength::PSMineralStrength( MetaProcess* p ) : MetaDouble( p )
{
   ThePSMineralStrengthParameter = this;
}

IsoString PSMineralStrength::Id() const { return "mineralStrength"; }
int PSMineralStrength::Precision() const { return 2; }
double PSMineralStrength::DefaultValue() const { return 8.0; }
double PSMineralStrength::MinimumValue() const { return 0.0; }
double PSMineralStrength::MaximumValue() const { return 15.0; }

PSChromaBlur::PSChromaBlur( MetaProcess* p ) : MetaDouble( p )
{
   ThePSChromaBlurParameter = this;
}

IsoString PSChromaBlur::Id() const { return "chromaBlur"; }
int PSChromaBlur::Precision() const { return 2; }
double PSChromaBlur::DefaultValue() const { return 2.5; }
double PSChromaBlur::MinimumValue() const { return 0.5; }
double PSChromaBlur::MaximumValue() const { return 6.0; }

PSMineralSharpen::PSMineralSharpen( MetaProcess* p ) : MetaDouble( p )
{
   ThePSMineralSharpenParameter = this;
}

IsoString PSMineralSharpen::Id() const { return "mineralSharpen"; }
int PSMineralSharpen::Precision() const { return 2; }
double PSMineralSharpen::DefaultValue() const { return planetary::stack::kSharpenDefault; }
double PSMineralSharpen::MinimumValue() const { return 0.0; }
double PSMineralSharpen::MaximumValue() const { return planetary::stack::kSharpenMax; }

} // namespace pcl
