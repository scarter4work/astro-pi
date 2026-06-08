// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NukeXParameters.h"

namespace pcl
{

// ── Global parameter pointers ────────────────────────────────────

NXLightFrames*       TheNXLightFramesParameter = nullptr;
NXLightFramePath*    TheNXLightFramePathParameter = nullptr;
NXLightFrameEnabled* TheNXLightFrameEnabledParameter = nullptr;
NXFlatFrames*        TheNXFlatFramesParameter = nullptr;
NXFlatFramePath*     TheNXFlatFramePathParameter = nullptr;
NXFlatFrameEnabled*  TheNXFlatFrameEnabledParameter = nullptr;
NXPrimaryStretch*   TheNXPrimaryStretchParameter = nullptr;
NXFinishingStretch* TheNXFinishingStretchParameter = nullptr;
NXEnableGPU*         TheNXEnableGPUParameter = nullptr;
NXCacheDirectory*    TheNXCacheDirectoryParameter = nullptr;

// ── Light Frames Table ───────────────────────────────────────────

NXLightFrames::NXLightFrames( MetaProcess* p ) : MetaTable( p )
{
   TheNXLightFramesParameter = this;
}

IsoString NXLightFrames::Id() const { return "lightFrames"; }
size_type NXLightFrames::MinLength() const { return 0; }

NXLightFramePath::NXLightFramePath( MetaTable* t ) : MetaString( t )
{
   TheNXLightFramePathParameter = this;
}

IsoString NXLightFramePath::Id() const { return "path"; }

NXLightFrameEnabled::NXLightFrameEnabled( MetaTable* t ) : MetaBoolean( t )
{
   TheNXLightFrameEnabledParameter = this;
}

IsoString NXLightFrameEnabled::Id() const { return "enabled"; }
bool NXLightFrameEnabled::DefaultValue() const { return true; }

// ── Flat Frames Table ────────────────────────────────────────────

NXFlatFrames::NXFlatFrames( MetaProcess* p ) : MetaTable( p )
{
   TheNXFlatFramesParameter = this;
}

IsoString NXFlatFrames::Id() const { return "flatFrames"; }
size_type NXFlatFrames::MinLength() const { return 0; }

NXFlatFramePath::NXFlatFramePath( MetaTable* t ) : MetaString( t )
{
   TheNXFlatFramePathParameter = this;
}

IsoString NXFlatFramePath::Id() const { return "path"; }

NXFlatFrameEnabled::NXFlatFrameEnabled( MetaTable* t ) : MetaBoolean( t )
{
   TheNXFlatFrameEnabledParameter = this;
}

IsoString NXFlatFrameEnabled::Id() const { return "enabled"; }
bool NXFlatFrameEnabled::DefaultValue() const { return true; }

// ── Stretch Configuration ────────────────────────────────────────

NXPrimaryStretch::NXPrimaryStretch( MetaProcess* p ) : MetaEnumeration( p )
{
   TheNXPrimaryStretchParameter = this;
}

IsoString NXPrimaryStretch::Id() const { return "primaryStretch"; }
size_type NXPrimaryStretch::NumberOfElements() const { return NumberOfItems; }

IsoString NXPrimaryStretch::ElementId( size_type i ) const
{
   switch ( i )
   {
   case Auto:    return "Auto";
   case VeraLux: return "VeraLux";
   case GHS:     return "GHS";
   case MTF:     return "MTF";
   case ArcSinh: return "ArcSinh";
   case Log:     return "Log";
   case Lupton:  return "Lupton";
   case CLAHE:   return "CLAHE";
   default:      return IsoString();
   }
}

int NXPrimaryStretch::ElementValue( size_type i ) const { return int( i ); }
size_type NXPrimaryStretch::DefaultValueIndex() const { return Auto; }

NXFinishingStretch::NXFinishingStretch( MetaProcess* p ) : MetaEnumeration( p )
{
   TheNXFinishingStretchParameter = this;
}

IsoString NXFinishingStretch::Id() const { return "finishingStretch"; }
size_type NXFinishingStretch::NumberOfElements() const { return NumberOfItems; }

IsoString NXFinishingStretch::ElementId( size_type i ) const
{
   switch ( i )
   {
   case None: return "None";
   default:   return IsoString();
   }
}

int NXFinishingStretch::ElementValue( size_type i ) const { return int( i ); }
size_type NXFinishingStretch::DefaultValueIndex() const { return None; }

// ── GPU Configuration ────────────────────────────────────────────

NXEnableGPU::NXEnableGPU( MetaProcess* p ) : MetaBoolean( p )
{
   TheNXEnableGPUParameter = this;
}

IsoString NXEnableGPU::Id() const { return "enableGPU"; }
bool NXEnableGPU::DefaultValue() const { return true; }

// ── Cache Directory ──────────────────────────────────────────────

NXCacheDirectory::NXCacheDirectory( MetaProcess* p ) : MetaString( p )
{
   TheNXCacheDirectoryParameter = this;
}

IsoString NXCacheDirectory::Id() const { return "cacheDirectory"; }
String NXCacheDirectory::DefaultValue() const { return "/tmp"; }

// ── Output parameters ────────────────────────────────────────────

NXNFramesProcessed*       TheNXNFramesProcessedParameter       = nullptr;
NXNFramesFailedAlignment* TheNXNFramesFailedAlignmentParameter = nullptr;

NXNFramesProcessed::NXNFramesProcessed( MetaProcess* p ) : MetaInt32( p )
{
   TheNXNFramesProcessedParameter = this;
}
IsoString NXNFramesProcessed::Id() const           { return "nFramesProcessed"; }
double    NXNFramesProcessed::DefaultValue() const { return 0; }
double    NXNFramesProcessed::MinimumValue() const { return 0; }
double    NXNFramesProcessed::MaximumValue() const { return 2147483647; }  // INT32_MAX
bool      NXNFramesProcessed::IsReadOnly() const   { return true; }

NXNFramesFailedAlignment::NXNFramesFailedAlignment( MetaProcess* p ) : MetaInt32( p )
{
   TheNXNFramesFailedAlignmentParameter = this;
}
IsoString NXNFramesFailedAlignment::Id() const           { return "nFramesFailedAlignment"; }
double    NXNFramesFailedAlignment::DefaultValue() const { return 0; }
double    NXNFramesFailedAlignment::MinimumValue() const { return 0; }
double    NXNFramesFailedAlignment::MaximumValue() const { return 2147483647; }
bool      NXNFramesFailedAlignment::IsReadOnly() const   { return true; }

} // namespace pcl
