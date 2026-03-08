//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack - Per-pixel statistical inference stacking

#ifndef __NukeXStackParameters_h
#define __NukeXStackParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// Input Frame List (table parameter)
// ----------------------------------------------------------------------------

class NXSInputFrames : public MetaTable
{
public:
   NXSInputFrames( MetaProcess* );
   IsoString Id() const override;
   size_type MinLength() const override;
};

extern NXSInputFrames* TheNXSInputFramesParameter;

// Frame path within the table
class NXSInputFramePath : public MetaString
{
public:
   NXSInputFramePath( MetaTable* );
   IsoString Id() const override;
};

extern NXSInputFramePath* TheNXSInputFramePathParameter;

// Frame enabled flag within the table
class NXSInputFrameEnabled : public MetaBoolean
{
public:
   NXSInputFrameEnabled( MetaTable* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSInputFrameEnabled* TheNXSInputFrameEnabledParameter;

// ----------------------------------------------------------------------------
// Quality Weight Mode Enumeration
// ----------------------------------------------------------------------------

class NXSQualityWeightMode : public MetaEnumeration
{
public:
   enum { None = 0,      // No quality weighting
          FWHMOnly = 1,  // Weight by FWHM only
          Full = 2,      // Full multi-attribute weighting (default)
          NumberOfItems,
          Default = Full };

   NXSQualityWeightMode( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern NXSQualityWeightMode* TheNXSQualityWeightModeParameter;

// ----------------------------------------------------------------------------
// Boolean Parameters
// ----------------------------------------------------------------------------

class NXSGenerateProvenance : public MetaBoolean
{
public:
   NXSGenerateProvenance( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSGenerateProvenance* TheNXSGenerateProvenanceParameter;

class NXSGenerateDistMetadata : public MetaBoolean
{
public:
   NXSGenerateDistMetadata( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSGenerateDistMetadata* TheNXSGenerateDistMetadataParameter;

class NXSEnableQualityWeighting : public MetaBoolean
{
public:
   NXSEnableQualityWeighting( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableQualityWeighting* TheNXSEnableQualityWeightingParameter;

class NXSEnableAutoStretch : public MetaBoolean
{
public:
   NXSEnableAutoStretch( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableAutoStretch* TheNXSEnableAutoStretchParameter;

// ----------------------------------------------------------------------------
// Floating Point Parameters
// ----------------------------------------------------------------------------

class NXSOutlierSigmaThreshold : public MetaFloat
{
public:
   NXSOutlierSigmaThreshold( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSOutlierSigmaThreshold* TheNXSOutlierSigmaThresholdParameter;

class NXSFWHMWeight : public MetaFloat
{
public:
   NXSFWHMWeight( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSFWHMWeight* TheNXSFWHMWeightParameter;

class NXSEccentricityWeight : public MetaFloat
{
public:
   NXSEccentricityWeight( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSEccentricityWeight* TheNXSEccentricityWeightParameter;

class NXSSkyBackgroundWeight : public MetaFloat
{
public:
   NXSSkyBackgroundWeight( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSSkyBackgroundWeight* TheNXSSkyBackgroundWeightParameter;

class NXSHFRWeight : public MetaFloat
{
public:
   NXSHFRWeight( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSHFRWeight* TheNXSHFRWeightParameter;

class NXSAltitudeWeight : public MetaFloat
{
public:
   NXSAltitudeWeight( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSAltitudeWeight* TheNXSAltitudeWeightParameter;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackParameters_h
