#include "GaiaDepthGradeParameters.h"

#include "core/Grade.h"

namespace pcl
{

GDGReal*          TheGDGBrightnessParameter = nullptr;
GDGReal*          TheGDGSizeParameter = nullptr;
GDGReal*          TheGDGContrastParameter = nullptr;
GDGReal*          TheGDGSaturationParameter = nullptr;
GDGReal*          TheGDGHaloGainParameter = nullptr;
GDGReal*          TheGDGHaloSigmaParameter = nullptr;
GDGReal*          TheGDGPLowParameter = nullptr;
GDGReal*          TheGDGPHighParameter = nullptr;
GDGBool*          TheGDGNebulaEnabledParameter = nullptr;
GDGReal*          TheGDGNebulaStrengthParameter = nullptr;
GDGReal*          TheGDGNebulaAtmosParameter = nullptr;
GDGReal*          TheGDGNebulaStructureParameter = nullptr;
GDGReal*          TheGDGDetectSigmaParameter = nullptr;
GDGReal*          TheGDGDetectFWHMParameter = nullptr;
GDGReal*          TheGDGMatchToleranceParameter = nullptr;
GDGReal*          TheGDGMinMatchRateParameter = nullptr;
GDGReal*          TheGDGNeutralStrengthParameter = nullptr;
GDGCatalog*       TheGDGCatalogParameter = nullptr;
GDGBool*          TheGDGMagLimitEnabledParameter = nullptr;
GDGReal*          TheGDGMagLimitParameter = nullptr;
GDGReal*          TheGDGParallaxSNRParameter = nullptr;
GDGInt*           TheGDGHaloMinRefsParameter = nullptr;
GDGReal*          TheGDGHaloRadiusSigmaParameter = nullptr;
GDGReal*          TheGDGHaloMoffatBetaParameter = nullptr;
GDGSplitMode*     TheGDGSplitModeParameter = nullptr;
GDGString*        TheGDGStarsViewIdParameter = nullptr;
GDGString*        TheGDGStarlessViewIdParameter = nullptr;
GDGOutputMode*    TheGDGOutputModeParameter = nullptr;
GDGString*        TheGDGSolveIndexFileParameter = nullptr;
GDGDatabaseFiles* TheGDGDatabaseFilesParameter = nullptr;
GDGString*        TheGDGDatabaseFilePathParameter = nullptr;
GDGInt*           TheGDGOutNDetectedParameter = nullptr;
GDGInt*           TheGDGOutNMatchedParameter = nullptr;
GDGReal*          TheGDGOutMatchRateParameter = nullptr;
GDGReal*          TheGDGOutMedianOffsetParameter = nullptr;
GDGBool*          TheGDGOutLowMatchParameter = nullptr;

void CreateGaiaDepthGradeParameters( MetaProcess* P )
{
   const gdg::GradeSettings d; // the dialog defaults (test_grade: settings_defaults_are_the_dialogs)

   // Look (the dialog's live sliders: ranges and precisions as GaiaDepthGradeDialog.js).
   TheGDGBrightnessParameter      = new GDGReal( P, "brightness", d.brightness, 0, 2, 2 );
   TheGDGSizeParameter            = new GDGReal( P, "size", d.size, 0, 2, 2 );
   TheGDGContrastParameter        = new GDGReal( P, "contrast", d.contrast, 0, 2, 2 );
   TheGDGSaturationParameter      = new GDGReal( P, "saturation", d.saturation, 0, 2, 2 );
   TheGDGHaloGainParameter        = new GDGReal( P, "haloGain", d.halo_gain, 0, 4, 2 );
   TheGDGHaloSigmaParameter       = new GDGReal( P, "haloSigma", d.halo_sigma, 0.5, 6, 2 );
   TheGDGPLowParameter            = new GDGReal( P, "pLow", d.p_low, 0, 50, 1 );
   TheGDGPHighParameter           = new GDGReal( P, "pHigh", d.p_high, 50, 100, 1 );
   TheGDGNebulaEnabledParameter   = new GDGBool( P, "nebulaEnabled", d.nebula_enabled );
   TheGDGNebulaStrengthParameter  = new GDGReal( P, "nebulaStrength", d.nebula_strength, 0, 2, 2 );
   TheGDGNebulaAtmosParameter     = new GDGReal( P, "nebulaAtmos", d.nebula_atmos, 0, 2, 2 );
   TheGDGNebulaStructureParameter = new GDGReal( P, "nebulaStructure", d.nebula_structure, 0, 2, 2 );

   // Matching (prepare-time; the dialog's NumericEdits: 1..30 and 1..12, one decimal).
   TheGDGDetectSigmaParameter     = new GDGReal( P, "detectSigma", d.detect_sigma, 1, 30, 1 );
   TheGDGDetectFWHMParameter      = new GDGReal( P, "detectFWHM", d.detect_fwhm, 0.5, 20, 2 );
   TheGDGMatchToleranceParameter  = new GDGReal( P, "matchTolerance", d.match_tolerance, 1, 12, 1 );
   TheGDGMinMatchRateParameter    = new GDGReal( P, "minMatchRate", d.min_match_rate, 0, 1, 2 );
   TheGDGNeutralStrengthParameter = new GDGReal( P, "neutralStrength", d.neutral_strength, -1, 1, 2 );
   TheGDGCatalogParameter         = new GDGCatalog( P );
   TheGDGMagLimitEnabledParameter = new GDGBool( P, "magLimitEnabled", d.mag_limit_enabled );
   TheGDGMagLimitParameter        = new GDGReal( P, "magLimit", d.mag_limit, -1.5, 25, 1 );
   TheGDGParallaxSNRParameter     = new GDGReal( P, "parallaxSNR", d.parallax_snr, 0, 1000, 1 );
   TheGDGHaloMinRefsParameter     = new GDGInt( P, "haloMinRefs", d.halo_min_refs, 1, 10000 );
   TheGDGHaloRadiusSigmaParameter = new GDGReal( P, "haloRadiusSigma", d.halo_radius_sigma, 0.5, 100, 2 );
   TheGDGHaloMoffatBetaParameter  = new GDGReal( P, "haloMoffatBeta", d.halo_moffat_beta, 0.1, 20, 2 );

   // Split, output, catalogue files.
   TheGDGSplitModeParameter       = new GDGSplitMode( P );
   TheGDGStarsViewIdParameter     = new GDGString( P, "starsViewId" );
   TheGDGStarlessViewIdParameter  = new GDGString( P, "starlessViewId" );
   TheGDGOutputModeParameter      = new GDGOutputMode( P );
   TheGDGSolveIndexFileParameter  = new GDGString( P, "solveIndexFile" );
   TheGDGDatabaseFilesParameter   = new GDGDatabaseFiles( P );
   TheGDGDatabaseFilePathParameter = new GDGString( TheGDGDatabaseFilesParameter, "gaiaDatabaseFilePath" );

   // QA of the last execution (read-only).
   TheGDGOutNDetectedParameter    = new GDGInt( P, "outNDetected", 0, 0, int( uint32_max >> 1 ), true );
   TheGDGOutNMatchedParameter     = new GDGInt( P, "outNMatched", 0, 0, int( uint32_max >> 1 ), true );
   TheGDGOutMatchRateParameter    = new GDGReal( P, "outMatchRate", 0, 0, 1, 4, true );
   TheGDGOutMedianOffsetParameter = new GDGReal( P, "outMedianOffset", 0, -1, 1000, 3, true ); // -1: no match
   TheGDGOutLowMatchParameter     = new GDGBool( P, "outLowMatch", false, true );
}

} // namespace pcl
