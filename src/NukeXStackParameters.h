//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack - Intelligent pixel selection for subframe integration

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
// Selection Strategy Enumeration
// ----------------------------------------------------------------------------

class NXSSelectionStrategy : public MetaEnumeration
{
public:
   enum { Distribution,    // Use distribution fitting
          WeightedMedian,  // Weighted median by confidence
          MLGuided,        // ML-guided selection
          Hybrid,          // Combined approach (default)
          NumberOfItems,
          Default = Hybrid };

   NXSSelectionStrategy( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern NXSSelectionStrategy* TheNXSSelectionStrategyParameter;

// ----------------------------------------------------------------------------
// Boolean Parameters
// ----------------------------------------------------------------------------

class NXSEnableMLSegmentation : public MetaBoolean
{
public:
   NXSEnableMLSegmentation( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableMLSegmentation* TheNXSEnableMLSegmentationParameter;

class NXSEnableTransitionSmoothing : public MetaBoolean
{
public:
   NXSEnableTransitionSmoothing( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSEnableTransitionSmoothing* TheNXSEnableTransitionSmoothingParameter;

class NXSUseSpatialContext : public MetaBoolean
{
public:
   NXSUseSpatialContext( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSUseSpatialContext* TheNXSUseSpatialContextParameter;

class NXSUseTargetContext : public MetaBoolean
{
public:
   NXSUseTargetContext( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSUseTargetContext* TheNXSUseTargetContextParameter;

class NXSGenerateMetadata : public MetaBoolean
{
public:
   NXSGenerateMetadata( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSGenerateMetadata* TheNXSGenerateMetadataParameter;

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

class NXSMinClassConfidence : public MetaFloat
{
public:
   NXSMinClassConfidence( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSMinClassConfidence* TheNXSMinClassConfidenceParameter;

class NXSSmoothingStrength : public MetaFloat
{
public:
   NXSSmoothingStrength( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSSmoothingStrength* TheNXSSmoothingStrengthParameter;

class NXSTransitionThreshold : public MetaFloat
{
public:
   NXSTransitionThreshold( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSTransitionThreshold* TheNXSTransitionThresholdParameter;

// ----------------------------------------------------------------------------
// Integer Parameters
// ----------------------------------------------------------------------------

class NXSTileSize : public MetaInt32
{
public:
   NXSTileSize( MetaProcess* );
   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSTileSize* TheNXSTileSizeParameter;

class NXSSmoothingRadius : public MetaInt32
{
public:
   NXSSmoothingRadius( MetaProcess* );
   IsoString Id() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSSmoothingRadius* TheNXSSmoothingRadiusParameter;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackParameters_h
