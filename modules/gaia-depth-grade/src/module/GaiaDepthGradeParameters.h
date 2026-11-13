// GaiaDepthGrade process parameters (plan Task 16). The ids are STABLE once shipped: process
// icons, projects and scripts name them. Defaults are the dialog's (GaiaDepthGradeDialog.js;
// plan D9); the engine-only ones are config.py's (gdg::GradeSettings holds both, and
// test_grade checks them).
#ifndef __GaiaDepthGradeParameters_h
#define __GaiaDepthGradeParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

PCL_BEGIN_LOCAL

/// A real parameter with its id, default, range and display precision.
class GDGReal : public MetaDouble
{
public:
   GDGReal( MetaProcess* P, const char* id, double def, double lo, double hi, int precision, bool readOnly = false )
      : MetaDouble( P ), m_id( id ), m_def( def ), m_lo( lo ), m_hi( hi ), m_precision( precision ), m_readOnly( readOnly )
   {
   }
   IsoString Id() const override { return m_id; }
   int Precision() const override { return m_precision; }
   double DefaultValue() const override { return m_def; }
   double MinimumValue() const override { return m_lo; }
   double MaximumValue() const override { return m_hi; }
   bool IsReadOnly() const override { return m_readOnly; }
private:
   IsoString m_id;
   double    m_def, m_lo, m_hi;
   int       m_precision;
   bool      m_readOnly;
};

class GDGInt : public MetaInt32
{
public:
   GDGInt( MetaProcess* P, const char* id, int def, int lo, int hi, bool readOnly = false )
      : MetaInt32( P ), m_id( id ), m_def( def ), m_lo( lo ), m_hi( hi ), m_readOnly( readOnly )
   {
   }
   IsoString Id() const override { return m_id; }
   double DefaultValue() const override { return m_def; }
   double MinimumValue() const override { return m_lo; }
   double MaximumValue() const override { return m_hi; }
   bool IsReadOnly() const override { return m_readOnly; }
private:
   IsoString m_id;
   int       m_def, m_lo, m_hi;
   bool      m_readOnly;
};

class GDGBool : public MetaBoolean
{
public:
   GDGBool( MetaProcess* P, const char* id, bool def, bool readOnly = false )
      : MetaBoolean( P ), m_id( id ), m_def( def ), m_readOnly( readOnly )
   {
   }
   IsoString Id() const override { return m_id; }
   bool DefaultValue() const override { return m_def; }
   bool IsReadOnly() const override { return m_readOnly; }
private:
   IsoString m_id;
   bool      m_def, m_readOnly;
};

class GDGString : public MetaString
{
public:
   GDGString( MetaProcess* P, const char* id ) : MetaString( P ), m_id( id ) {}
   GDGString( MetaTable* T, const char* id ) : MetaString( T ), m_id( id ) {}
   IsoString Id() const override { return m_id; }
private:
   IsoString m_id;
};

/// catalog: where the distances come from. Offline = the local Gaia DR3 database (parallax
/// distances; no network); Online = the Bailer-Jones distances of the ESA Gaia archive.
class GDGCatalog : public MetaEnumeration
{
public:
   enum { Offline, Online, NumberOfItems, Default = Offline };
   GDGCatalog( MetaProcess* P ) : MetaEnumeration( P ) {}
   IsoString Id() const override { return "catalog"; }
   size_type NumberOfElements() const override { return NumberOfItems; }
   IsoString ElementId( size_type i ) const override { return i == Online ? "Online" : "Offline"; }
   int ElementValue( size_type i ) const override { return int( i ); }
   size_type DefaultValueIndex() const override { return Default; }
};

/// splitMode: StarXTerminator (the shipped behaviour) or two views the user supplies.
class GDGSplitMode : public MetaEnumeration
{
public:
   enum { StarXTerminator, SuppliedViews, NumberOfItems, Default = StarXTerminator };
   GDGSplitMode( MetaProcess* P ) : MetaEnumeration( P ) {}
   IsoString Id() const override { return "splitMode"; }
   size_type NumberOfElements() const override { return NumberOfItems; }
   IsoString ElementId( size_type i ) const override { return i == SuppliedViews ? "SuppliedViews" : "StarXTerminator"; }
   int ElementValue( size_type i ) const override { return int( i ); }
   size_type DefaultValueIndex() const override { return Default; }
};

/// outputMode: a new <id>_depthgraded window, or the graded image written into the target
/// (process icons on ImageContainer, headless file in / file out).
class GDGOutputMode : public MetaEnumeration
{
public:
   enum { NewWindow, ReplaceTarget, NumberOfItems, Default = NewWindow };
   GDGOutputMode( MetaProcess* P ) : MetaEnumeration( P ) {}
   IsoString Id() const override { return "outputMode"; }
   size_type NumberOfElements() const override { return NumberOfItems; }
   IsoString ElementId( size_type i ) const override { return i == ReplaceTarget ? "ReplaceTarget" : "NewWindow"; }
   int ElementValue( size_type i ) const override { return int( i ); }
   size_type DefaultValueIndex() const override { return Default; }
};

/// gaiaDatabaseFiles: the Gaia DR3 database files (.xpsd); empty = those configured for
/// PixInsight's Gaia process.
class GDGDatabaseFiles : public MetaTable
{
public:
   GDGDatabaseFiles( MetaProcess* P ) : MetaTable( P ) {}
   IsoString Id() const override { return "gaiaDatabaseFiles"; }
};

PCL_END_LOCAL

// The parameters, created by GaiaDepthGradeProcess in this order (also the scripting order).
extern GDGReal*          TheGDGBrightnessParameter;
extern GDGReal*          TheGDGSizeParameter;
extern GDGReal*          TheGDGContrastParameter;
extern GDGReal*          TheGDGSaturationParameter;
extern GDGReal*          TheGDGHaloGainParameter;
extern GDGReal*          TheGDGHaloSigmaParameter;
extern GDGReal*          TheGDGPLowParameter;
extern GDGReal*          TheGDGPHighParameter;
extern GDGBool*          TheGDGNebulaEnabledParameter;
extern GDGReal*          TheGDGNebulaStrengthParameter;
extern GDGReal*          TheGDGNebulaAtmosParameter;
extern GDGReal*          TheGDGNebulaStructureParameter;
extern GDGReal*          TheGDGDetectSigmaParameter;
extern GDGReal*          TheGDGDetectFWHMParameter;
extern GDGReal*          TheGDGMatchToleranceParameter;
extern GDGReal*          TheGDGMinMatchRateParameter;
extern GDGReal*          TheGDGNeutralStrengthParameter;
extern GDGCatalog*       TheGDGCatalogParameter;
extern GDGBool*          TheGDGMagLimitEnabledParameter;
extern GDGReal*          TheGDGMagLimitParameter;
extern GDGReal*          TheGDGParallaxSNRParameter;
extern GDGInt*           TheGDGHaloMinRefsParameter;
extern GDGReal*          TheGDGHaloRadiusSigmaParameter;
extern GDGReal*          TheGDGHaloMoffatBetaParameter;
extern GDGSplitMode*     TheGDGSplitModeParameter;
extern GDGString*        TheGDGStarsViewIdParameter;
extern GDGString*        TheGDGStarlessViewIdParameter;
extern GDGOutputMode*    TheGDGOutputModeParameter;
extern GDGString*        TheGDGSolveIndexFileParameter;
extern GDGDatabaseFiles* TheGDGDatabaseFilesParameter;
extern GDGString*        TheGDGDatabaseFilePathParameter;
// Read-only QA outputs of the last execution.
extern GDGInt*           TheGDGOutNDetectedParameter;
extern GDGInt*           TheGDGOutNMatchedParameter;
extern GDGReal*          TheGDGOutMatchRateParameter;
extern GDGReal*          TheGDGOutMedianOffsetParameter;
extern GDGBool*          TheGDGOutLowMatchParameter;

/// Creates every parameter of `P` (GaiaDepthGradeProcess's constructor).
void CreateGaiaDepthGradeParameters( MetaProcess* P );

} // namespace pcl

#endif // __GaiaDepthGradeParameters_h
