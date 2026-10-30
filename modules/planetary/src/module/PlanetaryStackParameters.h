// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryStackParameters_h
#define __PlanetaryStackParameters_h

#include <pcl/MetaParameter.h>

#include <optional>

#include "BayerPatternParameter.h"
#include "CaptureName.h"

namespace pcl
{

// Absolute path of the .ser or .avi capture to stack (the format is chosen by content).
class PSFilePath : public MetaString
{
public:
   PSFilePath( MetaProcess* );
   IsoString Id() const override;
   String DefaultValue() const override;
};

// The shared bayerPattern parameter (BayerPatternParameter.h), as in SERViewer:
// a pattern set here wins over the AVI's sidecar.
class PSBayerPattern : public BayerPatternEnumeration
{
public:
   PSBayerPattern( MetaProcess* );
};

// What the capture is of: Auto reads it from the ASIAIR capture name
// (<date>-<time>-<Target>-Bin<n> <Filter> <temp>F); any other value is the
// user's. It names the stack (OBJECT) and decides whether Mineral moon may run.
class PSTarget : public MetaEnumeration
{
public:
   enum { Auto, Other, Moon, Mercury, Venus, Mars, Jupiter, Saturn, Uranus, Neptune, NumberOfItems, Default = Auto };

   PSTarget( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

// The target a target parameter value names: nullopt for Auto (the file name
// decides, stackcards::ResolveTarget). Throws pcl::Error, prefixed with
// `process`, for a value outside the enumeration.
std::optional<planetary::ser::Target> TargetFor( pcl_enum target, const char* process );

// Also make the mineral moon (Mineral.h) of a Moon stack, as a second image.
class PSMineralMoon : public MetaBoolean
{
public:
   PSMineralMoon( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

// The mineral moon's colour strength k (MineralParams::strength): 0 grey, 8 the default.
class PSMineralStrength : public MetaDouble
{
public:
   PSMineralStrength( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};

// The mineral moon's chroma blur, px at frames up to 2000 px wide
// (MineralParams::chromaBlurSigma): the colour's noise reduction.
class PSChromaBlur : public MetaDouble
{
public:
   PSChromaBlur( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};

// The mineral moon's Sharpen amount (MineralParams::sharpen, Mineral.h SharpenLuminanceImage): 0 none, 1 the
// engine's nominal gains, 1.5 the default, up to 4.
class PSMineralSharpen : public MetaDouble
{
public:
   PSMineralSharpen( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};

// Also make the planet processing (Planet.h) of a planet stack, as a second image.
class PSPlanetProcess : public MetaBoolean
{
public:
   PSPlanetProcess( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

// Planet processing's Saturation (PlanetParams::saturation): a multiple of the
// natural colour, 0 grey, 1 the default; range and default from Planet.h.
class PSPlanetSaturation : public MetaDouble
{
public:
   PSPlanetSaturation( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};

// Planet processing's chroma blur, px (PlanetParams::chromaBlurSigma); range
// and default from Planet.h.
class PSPlanetChromaBlur : public MetaDouble
{
public:
   PSPlanetChromaBlur( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};

// Planet processing's Sharpen amount (PlanetParams::sharpen, with
// kPlanetSharpen): 0 none; range and default from Planet.h.
class PSPlanetSharpen : public MetaDouble
{
public:
   PSPlanetSharpen( MetaProcess* );
   IsoString Id() const override;
   int Precision() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};

extern PSFilePath*        ThePSFilePathParameter;
extern PSBayerPattern*    ThePSBayerPatternParameter;
extern PSTarget*          ThePSTargetParameter;
extern PSMineralMoon*     ThePSMineralMoonParameter;
extern PSMineralStrength* ThePSMineralStrengthParameter;
extern PSChromaBlur*      ThePSChromaBlurParameter;
extern PSMineralSharpen*  ThePSMineralSharpenParameter;
extern PSPlanetProcess*    ThePSPlanetProcessParameter;
extern PSPlanetSaturation* ThePSPlanetSaturationParameter;
extern PSPlanetChromaBlur* ThePSPlanetChromaBlurParameter;
extern PSPlanetSharpen*    ThePSPlanetSharpenParameter;

} // namespace pcl

#endif // __PlanetaryStackParameters_h
