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

// ── Background Target ────────────────────────────────────────────

/// Where the auto-stretch puts the sky.
///
/// 0.25 is the screen-autostretch convention and stays the default, so an
/// existing icon reproduces exactly. It is not a law, though: it is also what
/// lifts the noise floor into plain view, and someone who dislikes visible sky
/// grain wants it lower. Dropping to 0.15 darkens the sky without touching a
/// pixel of detail.
class NXBackgroundTarget : public MetaFloat
{
public:
   NXBackgroundTarget( MetaProcess* );
   IsoString Id() const override;
   int       Precision() const override;
   double    DefaultValue() const override;
   double    MinimumValue() const override;
   double    MaximumValue() const override;
};

// ── Estimator ────────────────────────────────────────────────────

/// Which Phase B estimator produces each pixel: the distribution model race
/// (NukeX's original method) or a Huber M-estimator (measured less noisy on
/// four real corpora and far cheaper).
class NXEstimator : public MetaEnumeration
{
public:
   NXEstimator( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;

   enum { ModelRace, Huber, NumberOfItems };
};

// ── Sky gradient ─────────────────────────────────────────────────

/// Remove the fixed-pattern sky TILT from the stacked output. A plane only:
/// the sky level stays where it was, and a curved surface is never fitted
/// because it can follow a large object's outskirts and subtract them. Off
/// leaves the stack exactly as accumulated, for users who remove gradients
/// themselves in PixInsight. The engine default has been ON since v5.0.5.0;
/// this parameter is what the 5.0.5.0 release notes promised and did not ship.
class NXRemoveSkyGradient : public MetaBoolean
{
public:
   NXRemoveSkyGradient( MetaProcess* );
   IsoString Id() const override;
   bool DefaultValue() const override;
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

// ── QE Override Path ─────────────────────────────────────────────

class NXQEOverridePath : public MetaString
{
public:
   NXQEOverridePath( MetaProcess* );
   IsoString Id() const override;
   String    DefaultValue() const override;
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

class NXNFramesRejectedFilter : public MetaInt32
{
public:
   NXNFramesRejectedFilter( MetaProcess* );
   IsoString Id() const override;
   double    DefaultValue() const override;
   double    MinimumValue() const override;
   double    MaximumValue() const override;
   bool      IsReadOnly() const override;
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
extern NXBackgroundTarget* TheNXBackgroundTargetParameter;
extern NXRemoveSkyGradient* TheNXRemoveSkyGradientParameter;
extern NXEstimator*        TheNXEstimatorParameter;
extern NXEnableGPU*        TheNXEnableGPUParameter;
extern NXCacheDirectory*   TheNXCacheDirectoryParameter;
extern NXQEOverridePath*   TheNXQEOverridePathParameter;
extern NXNFramesProcessed*       TheNXNFramesProcessedParameter;
extern NXNFramesFailedAlignment* TheNXNFramesFailedAlignmentParameter;
extern NXNFramesRejectedFilter*  TheNXNFramesRejectedFilterParameter;

} // namespace pcl

#endif // __NukeXParameters_h
