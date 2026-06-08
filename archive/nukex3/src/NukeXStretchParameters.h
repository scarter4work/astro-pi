//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStretch Parameters

#ifndef __NukeXStretchParameters_h
#define __NukeXStretchParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// ----------------------------------------------------------------------------
// Stretch Algorithm Enumeration
// ----------------------------------------------------------------------------

class NXSStretchAlgorithm : public MetaEnumeration
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

   NXSStretchAlgorithm( MetaProcess* );

   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

extern NXSStretchAlgorithm* TheNXSStretchAlgorithmParameter;

// ----------------------------------------------------------------------------
// Boolean Parameters
// ----------------------------------------------------------------------------

class NXSAutoBlackPoint : public MetaBoolean
{
public:
   NXSAutoBlackPoint( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

extern NXSAutoBlackPoint* TheNXSAutoBlackPointParameter;

// ----------------------------------------------------------------------------
// Floating Point Parameters
// ----------------------------------------------------------------------------

class NXSContrast : public MetaFloat
{
public:
   NXSContrast( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSContrast* TheNXSContrastParameter;

// ----------------------------------------------------------------------------

class NXSSaturation : public MetaFloat
{
public:
   NXSSaturation( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSSaturation* TheNXSSaturationParameter;

// ----------------------------------------------------------------------------

class NXSBlackPoint : public MetaFloat
{
public:
   NXSBlackPoint( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSBlackPoint* TheNXSBlackPointParameter;

// ----------------------------------------------------------------------------

class NXSWhitePoint : public MetaFloat
{
public:
   NXSWhitePoint( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSWhitePoint* TheNXSWhitePointParameter;

// ----------------------------------------------------------------------------

class NXSGamma : public MetaFloat
{
public:
   NXSGamma( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSGamma* TheNXSGammaParameter;

// ----------------------------------------------------------------------------

class NXSStretchStrength : public MetaFloat
{
public:
   NXSStretchStrength( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   double DefaultValue() const override;
};

extern NXSStretchStrength* TheNXSStretchStrengthParameter;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStretchParameters_h
