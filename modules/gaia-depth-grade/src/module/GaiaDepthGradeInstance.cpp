#include "GaiaDepthGradeInstance.h"

#include "GaiaDepthGradeParameters.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>

#include <cmath>
#include <exception>

#ifdef GDG_PROBES
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#endif

namespace pcl
{

gdg::Log ConsoleLog()
{
   return gdg::Log{ []( const std::string& m )
                    {
                       Console().WriteLn( "<end><cbr>[GDG] " + String::UTF8ToUTF16( m.c_str() ).EncodedHTMLSpecialChars() );
                    },
                    []( const std::string& m )
                    {
                       Console().WarningLn( "<end><cbr>[GDG] ** Warning: " +
                                            String::UTF8ToUTF16( m.c_str() ).EncodedHTMLSpecialChars() );
                    } };
}

GaiaDepthGradeInstance::GaiaDepthGradeInstance( const MetaProcess* m )
   : ProcessImplementation( m )
   , p_catalog( GDGCatalog::Default )
   , p_splitMode( GDGSplitMode::Default )
   , p_outputMode( GDGOutputMode::Default )
{
   SetSettings( gdg::GradeSettings() );
}

GaiaDepthGradeInstance::GaiaDepthGradeInstance( const GaiaDepthGradeInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

void GaiaDepthGradeInstance::Assign( const ProcessImplementation& p )
{
   const GaiaDepthGradeInstance* x = dynamic_cast<const GaiaDepthGradeInstance*>( &p );
   if ( x == nullptr )
      return;
   p_brightness = x->p_brightness;
   p_size = x->p_size;
   p_contrast = x->p_contrast;
   p_saturation = x->p_saturation;
   p_haloGain = x->p_haloGain;
   p_haloSigma = x->p_haloSigma;
   p_pLow = x->p_pLow;
   p_pHigh = x->p_pHigh;
   p_nebulaEnabled = x->p_nebulaEnabled;
   p_nebulaStrength = x->p_nebulaStrength;
   p_nebulaAtmos = x->p_nebulaAtmos;
   p_nebulaStructure = x->p_nebulaStructure;
   p_detectSigma = x->p_detectSigma;
   p_detectFWHM = x->p_detectFWHM;
   p_matchTolerance = x->p_matchTolerance;
   p_minMatchRate = x->p_minMatchRate;
   p_neutralStrength = x->p_neutralStrength;
   p_catalog = x->p_catalog;
   p_magLimitEnabled = x->p_magLimitEnabled;
   p_magLimit = x->p_magLimit;
   p_parallaxSNR = x->p_parallaxSNR;
   p_haloMinRefs = x->p_haloMinRefs;
   p_haloRadiusSigma = x->p_haloRadiusSigma;
   p_haloMoffatBeta = x->p_haloMoffatBeta;
   p_splitMode = x->p_splitMode;
   p_starsViewId = x->p_starsViewId;
   p_starlessViewId = x->p_starlessViewId;
   p_outputMode = x->p_outputMode;
   p_solveIndexFile = x->p_solveIndexFile;
   p_gaiaDatabaseFiles = x->p_gaiaDatabaseFiles;
   o_nDetected = x->o_nDetected;
   o_nMatched = x->o_nMatched;
   o_matchRate = x->o_matchRate;
   o_medianOffset = x->o_medianOffset;
   o_lowMatch = x->o_lowMatch;
}

gdg::GradeSettings GaiaDepthGradeInstance::Settings() const
{
   gdg::GradeSettings s;
   s.brightness = p_brightness;
   s.size = p_size;
   s.contrast = p_contrast;
   s.saturation = p_saturation;
   s.halo_gain = p_haloGain;
   s.halo_sigma = p_haloSigma;
   s.p_low = p_pLow;
   s.p_high = p_pHigh;
   s.nebula_enabled = p_nebulaEnabled;
   s.nebula_strength = p_nebulaStrength;
   s.nebula_atmos = p_nebulaAtmos;
   s.nebula_structure = p_nebulaStructure;
   s.neutral_strength = p_neutralStrength;
   s.halo_min_refs = p_haloMinRefs;
   s.halo_radius_sigma = p_haloRadiusSigma;
   s.halo_moffat_beta = p_haloMoffatBeta;
   s.detect_sigma = p_detectSigma;
   s.detect_fwhm = p_detectFWHM;
   s.match_tolerance = p_matchTolerance;
   s.min_match_rate = p_minMatchRate;
   s.online = p_catalog == GDGCatalog::Online;
   s.mag_limit_enabled = p_magLimitEnabled;
   s.mag_limit = p_magLimit;
   s.parallax_snr = p_parallaxSNR;
   return s;
}

void GaiaDepthGradeInstance::SetSettings( const gdg::GradeSettings& s )
{
   p_brightness = s.brightness;
   p_size = s.size;
   p_contrast = s.contrast;
   p_saturation = s.saturation;
   p_haloGain = s.halo_gain;
   p_haloSigma = s.halo_sigma;
   p_pLow = s.p_low;
   p_pHigh = s.p_high;
   p_nebulaEnabled = s.nebula_enabled;
   p_nebulaStrength = s.nebula_strength;
   p_nebulaAtmos = s.nebula_atmos;
   p_nebulaStructure = s.nebula_structure;
   p_neutralStrength = s.neutral_strength;
   p_haloMinRefs = s.halo_min_refs;
   p_haloRadiusSigma = s.halo_radius_sigma;
   p_haloMoffatBeta = s.halo_moffat_beta;
   p_detectSigma = s.detect_sigma;
   p_detectFWHM = s.detect_fwhm;
   p_matchTolerance = s.match_tolerance;
   p_minMatchRate = s.min_match_rate;
   p_catalog = s.online ? GDGCatalog::Online : GDGCatalog::Offline;
   p_magLimitEnabled = s.mag_limit_enabled;
   p_magLimit = s.mag_limit;
   p_parallaxSNR = s.parallax_snr;
}

GradeOptions GaiaDepthGradeInstance::Options() const
{
   GradeOptions o;
   o.settings = Settings();
   o.suppliedViews = p_splitMode == GDGSplitMode::SuppliedViews;
   o.starsViewId = p_starsViewId.Trimmed();
   o.starlessViewId = p_starlessViewId.Trimmed();
   o.solveIndexFile = p_solveIndexFile.Trimmed();
   for ( const String& f : p_gaiaDatabaseFiles )
      if ( !f.Trimmed().IsEmpty() )
         o.gaiaDatabaseFiles << f.Trimmed();
   return o;
}

void GaiaDepthGradeInstance::SetQA( const gdg::QA& qa )
{
   o_nDetected = qa.n_detected;
   o_nMatched = qa.n_matched;
   o_matchRate = qa.match_rate;
   o_medianOffset = std::isfinite( qa.median_offset_px ) ? qa.median_offset_px : -1.0;
   o_lowMatch = qa.low_match_warning;
}

bool GaiaDepthGradeInstance::IsHistoryUpdater( const View& ) const
{
   // A new output window leaves the target untouched (no history state for it).
   return p_outputMode == GDGOutputMode::ReplaceTarget;
}

UndoFlags GaiaDepthGradeInstance::UndoMode( const View& ) const
{
   return UndoFlag::PixelData | UndoFlag::Keywords;
}

bool GaiaDepthGradeInstance::CanExecuteOn( const View& view, String& whyNot ) const
{
   if ( view.IsPreview() )
   {
      whyNot = "GaiaDepthGrade works on a whole image (it plate-solves the frame); it cannot be executed on previews.";
      return false;
   }
   if ( view.Image().IsComplexSample() )
   {
      whyNot = "GaiaDepthGrade cannot be executed on complex images.";
      return false;
   }
   return true;
}

#ifdef GDG_PROBES
namespace
{

// Test builds only (never the release build): GDG_TEST_DUMP_SETTINGS=<file> writes the engine
// configuration ExecuteOn runs with (probe params_reach_core), GDG_TEST_LAST_ERROR=<file> the
// message of a failed execution (PixInsight reports it only on the console).
void TestWrite( const char* var, const std::string& text )
{
   if ( const char* path = std::getenv( var ) )
      if ( *path != '\0' )
      {
         std::ofstream f( path, std::ios::binary | std::ios::trunc );
         f << text;
      }
}

std::string N( double v )
{
   char b[ 64 ];
   std::snprintf( b, sizeof( b ), "%.17g", v );
   return b;
}

std::string Q( const String& s )
{
   std::string o = "\"";
   for ( char c : std::string( s.ToUTF8().c_str() ) )
      if ( c == '"' || c == '\\' )
         o += '\\', o += c;
      else if ( static_cast<unsigned char>( c ) >= 0x20 )
         o += c;
   return o + "\"";
}

void DumpSettings( const GradeOptions& o )
{
   const gdg::GradeConfig c = gdg::grade_config( o.settings );
   std::string j = "{\"brightness\":" + N( c.gains.brightness ) + ",\"size\":" + N( c.gains.size ) +
      ",\"contrast\":" + N( c.gains.contrast ) + ",\"saturation\":" + N( c.gains.saturation ) +
      ",\"halo_gain\":" + N( c.gains.halo_gain ) + ",\"p_low\":" + N( c.p_low ) + ",\"p_high\":" + N( c.p_high ) +
      ",\"match_tolerance_px\":" + N( c.match_tolerance_px ) + ",\"min_match_rate\":" + N( c.min_match_rate ) +
      ",\"detect_fwhm\":" + N( c.detect_fwhm ) + ",\"detect_threshold_sigma\":" + N( c.detect_threshold_sigma ) +
      ",\"base_sigma_px\":" + N( c.base_sigma_px ) + ",\"neutral_strength\":" + N( c.neutral_strength ) +
      ",\"gaia_mag_limit\":" + (c.gaia_mag_limit ? N( *c.gaia_mag_limit ) : std::string( "null" )) +
      ",\"gaia_parallax_snr\":" + N( c.gaia_parallax_snr ) + ",\"halo_min_refs\":" + std::to_string( c.halo_min_refs ) +
      ",\"halo_radius_sigma\":" + N( c.halo_radius_sigma ) + ",\"halo_moffat_beta\":" + N( c.halo_moffat_beta ) +
      ",\"nebula_enabled\":" + (o.settings.nebula_enabled ? "true" : "false") +
      ",\"nebula_strength\":" + N( o.settings.nebula_strength ) + ",\"nebula_atmos\":" + N( o.settings.nebula_atmos ) +
      ",\"nebula_structure\":" + N( o.settings.nebula_structure ) + ",\"online\":" + (o.settings.online ? "true" : "false") +
      ",\"suppliedViews\":" + (o.suppliedViews ? "true" : "false") + ",\"starsViewId\":" + Q( o.starsViewId ) +
      ",\"starlessViewId\":" + Q( o.starlessViewId ) + ",\"solveIndexFile\":" + Q( o.solveIndexFile ) +
      ",\"gaiaDatabaseFiles\":[";
   for ( size_type i = 0; i < o.gaiaDatabaseFiles.Length(); ++i )
      j += (i ? "," : "") + Q( o.gaiaDatabaseFiles[i] );
   TestWrite( "GDG_TEST_DUMP_SETTINGS", j + "]}\n" );
}

} // namespace
#endif

bool GaiaDepthGradeInstance::ExecuteOn( View& view )
{
   const gdg::Log log = ConsoleLog();
   Console console;
   try
   {
      const GradeOptions opt = Options();
#ifdef GDG_PROBES
      DumpSettings( opt );
#endif
      PrepareSession s = RunPrepare( view, opt, log, RunInline( log ) );
      SetQA( s.prep.qa );
      console.NoteLn( "<end><cbr>[GDG] " + String::UTF8ToUTF16( gdg::qa_summary( s.prep.qa ).c_str() ) );

      console.WriteLn( "<end><cbr>[GDG] Executing: full-res render + recombine..." );
      const gdg::Image composite = RenderComposite( s, opt.settings, RunInline( log ) );
      if ( p_outputMode == GDGOutputMode::ReplaceTarget )
      {
         AutoViewLock lock( view );
         WriteIntoView( view, composite );
         console.NoteLn( "<end><cbr>[GDG] Done: graded " + String( view.Id() ) + " in place" );
      }
      else
      {
         ImageWindow w = CreateOutputWindow( s, composite );
         w.Show();
         console.NoteLn( "<end><cbr>[GDG] Done: created " + String( w.MainView().Id() ) );
      }
      return true;
   }
   catch ( const ProcessAborted& )
   {
      throw;
   }
   catch ( const Exception& e )
   {
      console.CriticalLn( "<end><cbr>[GDG] ERROR: " + e.Message().EncodedHTMLSpecialChars() );
#ifdef GDG_PROBES
      TestWrite( "GDG_TEST_LAST_ERROR", std::string( e.Message().ToUTF8().c_str() ) );
#endif
      throw;
   }
   catch ( const std::exception& e )
   {
      const String msg = String::UTF8ToUTF16( e.what() );
      console.CriticalLn( "<end><cbr>[GDG] ERROR: " + msg.EncodedHTMLSpecialChars() );
#ifdef GDG_PROBES
      TestWrite( "GDG_TEST_LAST_ERROR", e.what() );
#endif
      throw Error( msg );
   }
}

void* GaiaDepthGradeInstance::LockParameter( const MetaParameter* p, size_type row )
{
   if ( p == TheGDGBrightnessParameter ) return &p_brightness;
   if ( p == TheGDGSizeParameter ) return &p_size;
   if ( p == TheGDGContrastParameter ) return &p_contrast;
   if ( p == TheGDGSaturationParameter ) return &p_saturation;
   if ( p == TheGDGHaloGainParameter ) return &p_haloGain;
   if ( p == TheGDGHaloSigmaParameter ) return &p_haloSigma;
   if ( p == TheGDGPLowParameter ) return &p_pLow;
   if ( p == TheGDGPHighParameter ) return &p_pHigh;
   if ( p == TheGDGNebulaEnabledParameter ) return &p_nebulaEnabled;
   if ( p == TheGDGNebulaStrengthParameter ) return &p_nebulaStrength;
   if ( p == TheGDGNebulaAtmosParameter ) return &p_nebulaAtmos;
   if ( p == TheGDGNebulaStructureParameter ) return &p_nebulaStructure;
   if ( p == TheGDGDetectSigmaParameter ) return &p_detectSigma;
   if ( p == TheGDGDetectFWHMParameter ) return &p_detectFWHM;
   if ( p == TheGDGMatchToleranceParameter ) return &p_matchTolerance;
   if ( p == TheGDGMinMatchRateParameter ) return &p_minMatchRate;
   if ( p == TheGDGNeutralStrengthParameter ) return &p_neutralStrength;
   if ( p == TheGDGCatalogParameter ) return &p_catalog;
   if ( p == TheGDGMagLimitEnabledParameter ) return &p_magLimitEnabled;
   if ( p == TheGDGMagLimitParameter ) return &p_magLimit;
   if ( p == TheGDGParallaxSNRParameter ) return &p_parallaxSNR;
   if ( p == TheGDGHaloMinRefsParameter ) return &p_haloMinRefs;
   if ( p == TheGDGHaloRadiusSigmaParameter ) return &p_haloRadiusSigma;
   if ( p == TheGDGHaloMoffatBetaParameter ) return &p_haloMoffatBeta;
   if ( p == TheGDGSplitModeParameter ) return &p_splitMode;
   if ( p == TheGDGStarsViewIdParameter ) return p_starsViewId.Begin();
   if ( p == TheGDGStarlessViewIdParameter ) return p_starlessViewId.Begin();
   if ( p == TheGDGOutputModeParameter ) return &p_outputMode;
   if ( p == TheGDGSolveIndexFileParameter ) return p_solveIndexFile.Begin();
   if ( p == TheGDGDatabaseFilePathParameter ) return p_gaiaDatabaseFiles[row].Begin();
   if ( p == TheGDGOutNDetectedParameter ) return &o_nDetected;
   if ( p == TheGDGOutNMatchedParameter ) return &o_nMatched;
   if ( p == TheGDGOutMatchRateParameter ) return &o_matchRate;
   if ( p == TheGDGOutMedianOffsetParameter ) return &o_medianOffset;
   if ( p == TheGDGOutLowMatchParameter ) return &o_lowMatch;
   return nullptr;
}

bool GaiaDepthGradeInstance::AllocateParameter( size_type n, const MetaParameter* p, size_type row )
{
   String* s = nullptr;
   if ( p == TheGDGStarsViewIdParameter )
      s = &p_starsViewId;
   else if ( p == TheGDGStarlessViewIdParameter )
      s = &p_starlessViewId;
   else if ( p == TheGDGSolveIndexFileParameter )
      s = &p_solveIndexFile;
   else if ( p == TheGDGDatabaseFilePathParameter )
      s = &p_gaiaDatabaseFiles[row];
   else if ( p == TheGDGDatabaseFilesParameter )
   {
      p_gaiaDatabaseFiles.Clear();
      if ( n > 0 )
         p_gaiaDatabaseFiles.Add( String(), n );
      return true;
   }
   else
      return false;
   s->Clear();
   if ( n > 0 )
      s->SetLength( n );
   return true;
}

size_type GaiaDepthGradeInstance::ParameterLength( const MetaParameter* p, size_type row ) const
{
   if ( p == TheGDGStarsViewIdParameter ) return p_starsViewId.Length();
   if ( p == TheGDGStarlessViewIdParameter ) return p_starlessViewId.Length();
   if ( p == TheGDGSolveIndexFileParameter ) return p_solveIndexFile.Length();
   if ( p == TheGDGDatabaseFilesParameter ) return p_gaiaDatabaseFiles.Length();
   if ( p == TheGDGDatabaseFilePathParameter ) return p_gaiaDatabaseFiles[row].Length();
   return 0;
}

} // namespace pcl
