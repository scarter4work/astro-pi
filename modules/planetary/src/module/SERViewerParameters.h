// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __SERViewerParameters_h
#define __SERViewerParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// Absolute path of the .ser file to read.
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

extern SVFilePath*   TheSVFilePathParameter;
extern SVFrameIndex* TheSVFrameIndexParameter;
extern SVDebayer*    TheSVDebayerParameter;

} // namespace pcl

#endif // __SERViewerParameters_h
