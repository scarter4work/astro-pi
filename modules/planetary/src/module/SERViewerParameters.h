// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __SERViewerParameters_h
#define __SERViewerParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// Absolute path of the .ser or .avi file to read (the format is chosen by content).
class SVFilePath : public MetaString
{
public:
   SVFilePath( MetaProcess* );
   IsoString Id() const override;
   String DefaultValue() const override;
};

// Zero-based frame to show / open.
class SVFrameIndex : public MetaInt32
{
public:
   SVFrameIndex( MetaProcess* );
   IsoString Id() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
};

// Bayer files: open as debayered RGB (true) or as the raw 1-channel CFA (false).
class SVDebayer : public MetaBoolean
{
public:
   SVDebayer( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

// AVI only (a SER carries its own ColorID): the CFA pattern. Auto = the ASIAIR
// sidecar's Bayer value as it stands; with no sidecar, raw mono and a warning.
class SVBayerPattern : public MetaEnumeration
{
public:
   enum { Auto, None, RGGB, GRBG, GBRG, BGGR, NumberOfItems, Default = Auto };

   SVBayerPattern( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

// When non-empty, ExecuteGlobal converts the AVI at filePath to a SER at this
// path instead of opening a frame.
class SVSaveSerPath : public MetaString
{
public:
   SVSaveSerPath( MetaProcess* );
   IsoString Id() const override;
   String DefaultValue() const override;
};

extern SVFilePath*     TheSVFilePathParameter;
extern SVFrameIndex*   TheSVFrameIndexParameter;
extern SVDebayer*      TheSVDebayerParameter;
extern SVBayerPattern* TheSVBayerPatternParameter;
extern SVSaveSerPath*  TheSVSaveSerPathParameter;

} // namespace pcl

#endif // __SERViewerParameters_h
