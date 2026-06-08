//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// This file is part of NukeX.
//
// NukeX is free software: you can redistribute it and/or modify it under
// the terms of the MIT License.

#ifndef __NukeXParameters_h
#define __NukeXParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// Preview Mode Enumeration
// ----------------------------------------------------------------------------

class NXPreviewMode : public MetaEnumeration
{
public:
   enum { BeforeAfter,
          RegionMap,
          IndividualRegions,
          StretchedResult,
          NumberOfItems,
          Default = StretchedResult };

   NXPreviewMode( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern NXPreviewMode* TheNXPreviewModeParameter;

// ----------------------------------------------------------------------------
// Stretch Algorithm Enumeration
// ----------------------------------------------------------------------------

class NXStretchAlgorithm : public MetaEnumeration
{
public:
   enum { MTF,
          Histogram,
          GHS,
          ArcSinh,
          Log,
          Lumpton,
          RNC,
          Photometric,
          OTS,
          SAS,
          Veralux,
          Auto,
          NumberOfItems,
          Default = Auto };

   NXStretchAlgorithm( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern NXStretchAlgorithm* TheNXStretchAlgorithmParameter;

// ----------------------------------------------------------------------------
// Boolean Parameters
// ----------------------------------------------------------------------------

class NXAutoSegment : public MetaBoolean
{
public:
   NXAutoSegment( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXAutoSegment* TheNXAutoSegmentParameter;

class NXAutoSelect : public MetaBoolean
{
public:
   NXAutoSelect( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXAutoSelect* TheNXAutoSelectParameter;

class NXEnableLRGB : public MetaBoolean
{
public:
   NXEnableLRGB( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableLRGB* TheNXEnableLRGBParameter;

class NXEnableToneMapping : public MetaBoolean
{
public:
   NXEnableToneMapping( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableToneMapping* TheNXEnableToneMappingParameter;

class NXAutoBlackPoint : public MetaBoolean
{
public:
   NXAutoBlackPoint( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXAutoBlackPoint* TheNXAutoBlackPointParameter;

// ----------------------------------------------------------------------------
// Floating Point Parameters
// ----------------------------------------------------------------------------

class NXGlobalContrast : public MetaFloat
{
public:
   NXGlobalContrast( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXGlobalContrast* TheNXGlobalContrastParameter;

class NXSaturationBoost : public MetaFloat
{
public:
   NXSaturationBoost( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSaturationBoost* TheNXSaturationBoostParameter;

class NXBlendRadius : public MetaFloat
{
public:
   NXBlendRadius( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXBlendRadius* TheNXBlendRadiusParameter;

class NXStretchStrength : public MetaFloat
{
public:
   NXStretchStrength( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXStretchStrength* TheNXStretchStrengthParameter;

class NXBlackPoint : public MetaFloat
{
public:
   NXBlackPoint( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXBlackPoint* TheNXBlackPointParameter;

class NXWhitePoint : public MetaFloat
{
public:
   NXWhitePoint( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXWhitePoint* TheNXWhitePointParameter;

class NXGamma : public MetaFloat
{
public:
   NXGamma( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXGamma* TheNXGammaParameter;

// ----------------------------------------------------------------------------
// Region Enable Parameters (one per region class)
// ----------------------------------------------------------------------------

class NXEnableStarCores : public MetaBoolean
{
public:
   NXEnableStarCores( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableStarCores* TheNXEnableStarCoresParameter;

class NXEnableStarHalos : public MetaBoolean
{
public:
   NXEnableStarHalos( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableStarHalos* TheNXEnableStarHalosParameter;

class NXEnableNebulaBright : public MetaBoolean
{
public:
   NXEnableNebulaBright( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableNebulaBright* TheNXEnableNebulaBrightParameter;

class NXEnableNebulaFaint : public MetaBoolean
{
public:
   NXEnableNebulaFaint( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableNebulaFaint* TheNXEnableNebulaFaintParameter;

class NXEnableDustLanes : public MetaBoolean
{
public:
   NXEnableDustLanes( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableDustLanes* TheNXEnableDustLanesParameter;

class NXEnableGalaxyCore : public MetaBoolean
{
public:
   NXEnableGalaxyCore( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableGalaxyCore* TheNXEnableGalaxyCoreParameter;

class NXEnableGalaxyHalo : public MetaBoolean
{
public:
   NXEnableGalaxyHalo( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableGalaxyHalo* TheNXEnableGalaxyHaloParameter;

class NXEnableGalaxyArms : public MetaBoolean
{
public:
   NXEnableGalaxyArms( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableGalaxyArms* TheNXEnableGalaxyArmsParameter;

class NXEnableBackground : public MetaBoolean
{
public:
   NXEnableBackground( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXEnableBackground* TheNXEnableBackgroundParameter;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXParameters_h
