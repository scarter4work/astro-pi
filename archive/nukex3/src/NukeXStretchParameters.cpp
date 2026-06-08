//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStretchParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Stretch Algorithm
// ----------------------------------------------------------------------------

NXSStretchAlgorithm* TheNXSStretchAlgorithmParameter = nullptr;

NXSStretchAlgorithm::NXSStretchAlgorithm( MetaProcess* P ) : MetaEnumeration( P )
{
   TheNXSStretchAlgorithmParameter = this;
}

IsoString NXSStretchAlgorithm::Id() const
{
   return "stretchAlgorithm";
}

size_type NXSStretchAlgorithm::NumberOfElements() const
{
   return NumberOfItems;
}

IsoString NXSStretchAlgorithm::ElementId( size_type i ) const
{
   switch ( i )
   {
   default:
   case MTF:         return "Algorithm_MTF";
   case Histogram:   return "Algorithm_Histogram";
   case GHS:         return "Algorithm_GHS";
   case ArcSinh:     return "Algorithm_ArcSinh";
   case Log:         return "Algorithm_Log";
   case Lumpton:     return "Algorithm_Lumpton";
   case RNC:         return "Algorithm_RNC";
   case Photometric: return "Algorithm_Photometric";
   case OTS:         return "Algorithm_OTS";
   case SAS:         return "Algorithm_SAS";
   case Veralux:     return "Algorithm_Veralux";
   case Auto:        return "Algorithm_Auto";
   }
}

int NXSStretchAlgorithm::ElementValue( size_type i ) const
{
   return int( i );
}

size_type NXSStretchAlgorithm::DefaultValueIndex() const
{
   return Default;
}

// ----------------------------------------------------------------------------
// Auto Black Point
// ----------------------------------------------------------------------------

NXSAutoBlackPoint* TheNXSAutoBlackPointParameter = nullptr;

NXSAutoBlackPoint::NXSAutoBlackPoint( MetaProcess* P ) : MetaBoolean( P )
{
   TheNXSAutoBlackPointParameter = this;
}

IsoString NXSAutoBlackPoint::Id() const
{
   return "autoBlackPoint";
}

bool NXSAutoBlackPoint::DefaultValue() const
{
   return true;
}

// ----------------------------------------------------------------------------
// Contrast
// ----------------------------------------------------------------------------

NXSContrast* TheNXSContrastParameter = nullptr;

NXSContrast::NXSContrast( MetaProcess* P ) : MetaFloat( P )
{
   TheNXSContrastParameter = this;
}

IsoString NXSContrast::Id() const
{
   return "contrast";
}

int NXSContrast::Precision() const
{
   return 3;
}

double NXSContrast::MinimumValue() const
{
   return 0.0;
}

double NXSContrast::MaximumValue() const
{
   return 2.0;
}

double NXSContrast::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------
// Saturation
// ----------------------------------------------------------------------------

NXSSaturation* TheNXSSaturationParameter = nullptr;

NXSSaturation::NXSSaturation( MetaProcess* P ) : MetaFloat( P )
{
   TheNXSSaturationParameter = this;
}

IsoString NXSSaturation::Id() const
{
   return "saturation";
}

int NXSSaturation::Precision() const
{
   return 3;
}

double NXSSaturation::MinimumValue() const
{
   return 0.0;
}

double NXSSaturation::MaximumValue() const
{
   return 2.0;
}

double NXSSaturation::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------
// Black Point
// ----------------------------------------------------------------------------

NXSBlackPoint* TheNXSBlackPointParameter = nullptr;

NXSBlackPoint::NXSBlackPoint( MetaProcess* P ) : MetaFloat( P )
{
   TheNXSBlackPointParameter = this;
}

IsoString NXSBlackPoint::Id() const
{
   return "blackPoint";
}

int NXSBlackPoint::Precision() const
{
   return 6;
}

double NXSBlackPoint::MinimumValue() const
{
   return 0.0;
}

double NXSBlackPoint::MaximumValue() const
{
   return 0.5;
}

double NXSBlackPoint::DefaultValue() const
{
   return 0.0;
}

// ----------------------------------------------------------------------------
// White Point
// ----------------------------------------------------------------------------

NXSWhitePoint* TheNXSWhitePointParameter = nullptr;

NXSWhitePoint::NXSWhitePoint( MetaProcess* P ) : MetaFloat( P )
{
   TheNXSWhitePointParameter = this;
}

IsoString NXSWhitePoint::Id() const
{
   return "whitePoint";
}

int NXSWhitePoint::Precision() const
{
   return 6;
}

double NXSWhitePoint::MinimumValue() const
{
   return 0.5;
}

double NXSWhitePoint::MaximumValue() const
{
   return 1.0;
}

double NXSWhitePoint::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------
// Gamma
// ----------------------------------------------------------------------------

NXSGamma* TheNXSGammaParameter = nullptr;

NXSGamma::NXSGamma( MetaProcess* P ) : MetaFloat( P )
{
   TheNXSGammaParameter = this;
}

IsoString NXSGamma::Id() const
{
   return "gamma";
}

int NXSGamma::Precision() const
{
   return 3;
}

double NXSGamma::MinimumValue() const
{
   return 0.1;
}

double NXSGamma::MaximumValue() const
{
   return 5.0;
}

double NXSGamma::DefaultValue() const
{
   return 1.0;
}

// ----------------------------------------------------------------------------
// Stretch Strength
// ----------------------------------------------------------------------------

NXSStretchStrength* TheNXSStretchStrengthParameter = nullptr;

NXSStretchStrength::NXSStretchStrength( MetaProcess* P ) : MetaFloat( P )
{
   TheNXSStretchStrengthParameter = this;
}

IsoString NXSStretchStrength::Id() const
{
   return "stretchStrength";
}

int NXSStretchStrength::Precision() const
{
   return 3;
}

double NXSStretchStrength::MinimumValue() const
{
   return 0.0;
}

double NXSStretchStrength::MaximumValue() const
{
   return 2.0;
}

double NXSStretchStrength::DefaultValue() const
{
   return 0.5;
}

// ----------------------------------------------------------------------------

} // namespace pcl
