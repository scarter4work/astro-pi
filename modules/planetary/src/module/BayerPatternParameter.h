// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __BayerPatternParameter_h
#define __BayerPatternParameter_h

#include <pcl/MetaParameter.h>

#include "FrameSource.h"

namespace pcl
{

// The bayerPattern parameter every Planetary process shares (SERViewer,
// PlanetaryStack): same element ids and values, so a script or icon reads the
// same in both. AVI only (a SER carries its own ColorID): the CFA pattern.
// Auto = the ASIAIR sidecar's Bayer value as it stands; with no
// sidecar, raw mono and a warning. Each process derives its own parameter
// (its Id) from this.
class BayerPatternEnumeration : public MetaEnumeration
{
public:
   enum { Auto, None, RGGB, GRBG, GBRG, BGGR, NumberOfItems, Default = Auto };

   BayerPatternEnumeration( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;
};

// The reader's Bayer choice for a bayerPattern parameter value (element index).
// Throws pcl::Error, prefixed with `process`, for a value outside the enumeration.
planetary::ser::BayerChoice BayerChoiceFor( pcl_enum bayerPattern, const char* process );

} // namespace pcl

#endif // __BayerPatternParameter_h
