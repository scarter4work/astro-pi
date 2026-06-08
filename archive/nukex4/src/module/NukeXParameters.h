// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeXParameters_h
#define __NukeXParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// ── Input Light Frames Table ─────────────────────────────────────

class NXLightFrames : public MetaTable
{
public:
   NXLightFrames( MetaProcess* );
   IsoString Id() const override;
   size_type MinLength() const override;
};

class NXLightFramePath : public MetaString
{
public:
   NXLightFramePath( MetaTable* );
   IsoString Id() const override;
};

class NXLightFrameEnabled : public MetaBoolean
{
public:
   NXLightFrameEnabled( MetaTable* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

// ── Input Flat Frames Table ──────────────────────────────────────

class NXFlatFrames : public MetaTable
{
public:
   NXFlatFrames( MetaProcess* );
   IsoString Id() const override;
   size_type MinLength() const override;
};

class NXFlatFramePath : public MetaString
{
public:
   NXFlatFramePath( MetaTable* );
   IsoString Id() const override;
};

class NXFlatFrameEnabled : public MetaBoolean
{
public:
   NXFlatFrameEnabled( MetaTable* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

// ── Stretch Configuration ────────────────────────────────────────

class NXPrimaryStretch : public MetaEnumeration
{
public:
   NXPrimaryStretch( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;

   enum { Auto, VeraLux, GHS, MTF, ArcSinh, Log, Lupton, CLAHE, NumberOfItems };
};

class NXFinishingStretch : public MetaEnumeration
{
public:
   NXFinishingStretch( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;

   enum { None, NumberOfItems };
};

// ── GPU Configuration ────────────────────────────────────────────

class NXEnableGPU : public MetaBoolean
{
public:
   NXEnableGPU( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
};

// ── Cache Directory ──────────────────────────────────────────────

class NXCacheDirectory : public MetaString
{
public:
   NXCacheDirectory( MetaProcess* );
   IsoString Id() const override;
   String DefaultValue() const override;
};

// ── Output (read-only, populated after ExecuteGlobal) ────────────
//
// Exposing these as read-only PCL parameters lets PJSR harnesses (and
// any future scripted caller) inspect the result of a run directly:
//
//   var P = new NukeX;
//   P.lightFrames = […];
//   P.executeGlobal();
//   Console.writeln("failed: " + P.nFramesFailedAlignment);
//
// Without this we had to regex-parse the Process Console log.

class NXNFramesProcessed : public MetaInt32
{
public:
   NXNFramesProcessed( MetaProcess* );
   IsoString Id() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   bool IsReadOnly() const override;
};

class NXNFramesFailedAlignment : public MetaInt32
{
public:
   NXNFramesFailedAlignment( MetaProcess* );
   IsoString Id() const override;
   double DefaultValue() const override;
   double MinimumValue() const override;
   double MaximumValue() const override;
   bool IsReadOnly() const override;
};

// ── Global parameter pointers ────────────────────────────────────

extern NXLightFrames*      TheNXLightFramesParameter;
extern NXLightFramePath*   TheNXLightFramePathParameter;
extern NXLightFrameEnabled* TheNXLightFrameEnabledParameter;
extern NXFlatFrames*       TheNXFlatFramesParameter;
extern NXFlatFramePath*    TheNXFlatFramePathParameter;
extern NXFlatFrameEnabled* TheNXFlatFrameEnabledParameter;
extern NXPrimaryStretch*   TheNXPrimaryStretchParameter;
extern NXFinishingStretch* TheNXFinishingStretchParameter;
extern NXEnableGPU*        TheNXEnableGPUParameter;
extern NXCacheDirectory*   TheNXCacheDirectoryParameter;
extern NXNFramesProcessed*       TheNXNFramesProcessedParameter;
extern NXNFramesFailedAlignment* TheNXNFramesFailedAlignmentParameter;

} // namespace pcl

#endif // __NukeXParameters_h
