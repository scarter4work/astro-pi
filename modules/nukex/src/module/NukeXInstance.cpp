// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NukeXInstance.h"
#include "NukeXParameters.h"
#include "NukeXProcess.h"
#include "NukeXVersion.h"

#include "NukeXProgress.h"
#include "NukeXConsoleText.hpp"
#include "nukex/compose/compose_image.hpp"
#include "RatingDialog.h"
#include "FilterDialog.h"
#include "nukex/io/filter_alias.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/calibration/qe_update_state.hpp"
#include "nukex/calibration/qe_database.hpp"
#include <filesystem>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/GlobalSettings.h>

// NukeX pipeline headers
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/compose/color_composer.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/stretch_pipeline.hpp"
#include "nukex/stretch/image_stats.hpp"
#include "nukex/stretch/layer_loader.hpp"
#include "nukex/stretch/param_model.hpp"
#include "nukex/learning/rating_db.hpp"
#include "nukex/learning/train_model.hpp"
#include "nukex/learning/atomic_write.hpp"
#include "nukex/io/fits_reader.hpp"
#include "stretch_factory.hpp"

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>   // std::getenv
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Build a FITS keyword set that records which build of NukeX produced this
// image and what the stacking result looked like.  Callers layer
// image-type-specific keywords (e.g., primaryStretch label) on top.
pcl::FITSKeywordArray base_output_keywords(
   const char* nukex_version,
   const char* image_kind,
   int n_frames_processed,
   int n_frames_failed_alignment )
{
   pcl::FITSKeywordArray ka;
   ka.Append( pcl::FITSHeaderKeyword(
      "CREATOR", pcl::IsoString( "'NukeX v" ) + nukex_version + "'",
      "Software that produced this image" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NUKEXVER", pcl::IsoString( "'" ) + nukex_version + "'",
      "NukeX module version" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NUKEXIMG", pcl::IsoString( "'" ) + image_kind + "'",
      "NukeX output kind: stacked | noise | stretched" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NFRAMES", pcl::IsoString().Format( "%d", n_frames_processed ),
      "Number of light frames processed" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "NFAILALN", pcl::IsoString().Format( "%d", n_frames_failed_alignment ),
      "Frames flagged as failed alignment (weight x 0.5)" ) );
   ka.Append( pcl::FITSHeaderKeyword(
      "HISTORY", pcl::IsoString(),
      pcl::IsoString( "NukeX v" ) + nukex_version
          + " - Distribution-Fitted Stacking" ) );
   return ka;
}

// Append the linear-calibration provenance every NukeX output window carries:
// what the chroma background match subtracted, and the rectangle the
// intersection trim kept. Both change the pixels a user is looking at, so
// both belong in the header rather than only in the Process Console, which
// scrolls away.
//
// Keyword names stay within the 8-character FITS convention the rest of
// base_output_keywords follows.
void append_calibration_keywords(
   pcl::FITSKeywordArray& ka,
   const nukex::StackingEngine::ExecuteResult& result )
{
   if ( !result.background_match.empty() )
   {
      pcl::IsoString v;
      for ( const auto& [name, off] : result.background_match )
      {
         if ( !v.IsEmpty() )
            v += " ";
         v += pcl::IsoString().Format( "%s=%.5f", name.c_str(),
                                       static_cast<double>( off ) );
      }
      ka.Append( pcl::FITSHeaderKeyword(
         "NUKEXBGM", pcl::IsoString( "'" ) + v + "'",
         "Sky level subtracted per colour channel to match backgrounds" ) );
   }

   if ( result.trim.applied )
      ka.Append( pcl::FITSHeaderKeyword(
         "NUKEXTRM",
         pcl::IsoString().Format( "'x0=%d y0=%d w=%d h=%d'",
                                  result.trim.x0, result.trim.y0,
                                  result.trim.width(), result.trim.height() ),
         "Kept region: covered by every frame (intersection)" ) );
}

// Phase 8 rating-DB filter-class encoding (rating_db.hpp schema v2).
//
// RatingDialog shows the color-balance axis only for classes whose output
// carries broadband chrominance: BROADBAND_RGB (2) and BROADBAND_OSC (3).
// Luminance, single-line narrowband and dual-NB composites hide it.
int filter_class_to_rating_int( nukex::FilterClass fc )
{
   switch ( fc )
   {
   case nukex::FilterClass::BROADBAND_L:       return 1;
   case nukex::FilterClass::BROADBAND_RGB:     return 2;
   case nukex::FilterClass::BROADBAND_OSC:     return 3;
   case nukex::FilterClass::NARROWBAND_SINGLE: return 4;
   case nukex::FilterClass::DUAL_NB_OSC:       return 5;
   case nukex::FilterClass::UNKNOWN:           return 0;
   }
   return 0;
}

// Serialize the trainable params on `op` to a compact JSON object so Task
// 19 can store it in the rating DB verbatim. Keys come from
// op.param_bounds(); only params whose current value is readable via
// get_param() are emitted.
std::string op_trainable_params_json( const nukex::StretchOp& op )
{
   using json = nlohmann::json;
   json j = json::object();
   for ( const auto& [pname, _] : op.param_bounds() )
   {
      auto v = op.get_param( pname );
      if ( v.has_value() )
         j[pname] = static_cast<double>( *v );
   }
   return j.dump();
}

// <PixInsight base>/share — where the Phase 8 bootstrap will live. (The
// shipped QE database is compiled into the module, not installed here.)
static std::string PIShareRoot()
{
   const std::string base =
       pcl::PixInsightSettings::GlobalString( "Application/BaseDirectory" ).ToUTF8().c_str();
   return base + "/share";
}

// <user-data>/nukex4 -- the same 0700 directory resolve_user_data_paths()
// creates for the Phase 8 state. A downloaded database lands here because
// <PI>/share is root-owned in a normal install and a user must not need
// write access there to receive a camera-database update.
static std::string QEUserDataRoot()
{
   const char* home = std::getenv( "HOME" );
   return ( home ? std::string( home ) + "/.config" : std::string( "/tmp" ) )
          + "/nukex4";
}

static std::string QEUpdateStatePath()
{
   return QEUserDataRoot() + "/qe_update_state.json";
}

// Precedence: a downloaded database is used only while it is newer than the
// one compiled into the module (nukex::active_qe_database). The built-in copy
// cannot be modified or removed, so deleting a single file always restores the
// as-installed behaviour, and a module update that ships a newer database is
// never shadowed by an older download. The user's qe_overrides.json still
// layers on top of whichever wins here -- the engine applies that separately.
static bool DownloadedQEDatabasePresent()
{
   std::error_code ec;
   const bool present = std::filesystem::exists(
       QEUserDataRoot() + "/qe_database.json", ec );
   return present && !ec;
}

static nukex::ActiveQEDatabase CurrentQEDatabase()
{
   return nukex::active_qe_database( nukex::load_update_state( QEUpdateStatePath() ),
                                     DownloadedQEDatabasePresent(),
                                     nukex::embedded_qe_database_version() );
}

static bool UsingDownloadedQEDatabase()
{
   return CurrentQEDatabase().use_downloaded;
}

static std::string ResolveQEDatabasePath()
{
   if ( UsingDownloadedQEDatabase() )
      return QEUserDataRoot() + "/qe_database.json";
   return std::string();   // empty => the engine loads the compiled-in database
}

// The version of the database actually loaded, which is not the same thing
// as the version last installed: delete the downloaded file, or update to a
// module that carries a newer one, and the built-in database takes over. A
// provenance keyword that names a version we did not use is worse than none
// at all, since explaining a changed result is its only job. 0 means a
// built-in database that was never published (a development build).
static int ActiveQEDatabaseVersion()
{
   return CurrentQEDatabase().version;
}

} // anonymous namespace

namespace pcl
{

// Task 19: persist the user's rating row, retrain Layer 3 for this stretch,
// and atomically replace the user-model JSON. Called from ExecuteGlobal's
// rating popup *and* from NukeXInterface's "Rate last run" button (Task 18).
//
// Path discipline: we use the paths stashed on lastRun at Execute time, not
// a fresh resolve_user_data_paths() call. If HOME changed between Execute
// and Save (rare, but e.g. user switching env), the row goes to the same DB
// the predict path read from -- no drift between Layer 3 reads and writes.
void NukeXInstance::SaveRatingFromLastRun( const pcl::RatingResult& res )
{
   if ( !lastRun.valid )
      return;

   pcl::Console console;

   // Open (or create) the per-user rating DB at the path we resolved at
   // Execute time. If the DB can't be opened, the rating is discarded --
   // we never silently "kind of saved it".
   sqlite3* db = nukex::learning::open_rating_db( lastRun.user_db_path );
   if ( db == nullptr )
   {
      console.CriticalLn( String( "NukeX: couldn't open rating DB; rating discarded." ) );
      return;
   }

   // Build the RunRecord. ImageStats::to_feature_row() returns the same
   // 29-column vector the predict path consumed, so training reads exactly
   // what prediction saw (24 per-channel followed by 5 global stats).
   nukex::learning::RunRecord rec;
   rec.run_id           = lastRun.run_id;
   rec.created_at_unix  = lastRun.created_at_unix;
   rec.stretch_name     = lastRun.stretch_name;
   rec.target_class     = lastRun.target_class;
   rec.filter_class     = lastRun.filter_class;

   const auto feature_row = lastRun.stats.to_feature_row();
   for ( int i = 0; i < 24; ++i )
      rec.per_channel_stats[i] = feature_row[i];
   rec.bright_concentration = lastRun.stats.bright_concentration;
   rec.color_rg             = lastRun.stats.color_rg;
   rec.color_bg             = lastRun.stats.color_bg;
   rec.fwhm_median          = lastRun.stats.fwhm_median;
   rec.star_count           = lastRun.stats.star_count;

   rec.params_json          = lastRun.params_json_applied;
   rec.rating_brightness    = res.brightness;
   rec.rating_saturation    = res.saturation;
   rec.rating_color         = res.color;          // std::optional<int>
   rec.rating_star_bloat    = res.star_bloat;
   rec.rating_overall       = res.overall;

   if ( !nukex::learning::insert_run( db, rec ) )
   {
      console.CriticalLn( String( "NukeX: rating insert failed; rating discarded." ) );
      nukex::learning::close_rating_db( db );
      return;
   }

   // Attach the read-only bootstrap DB if it ships with this version.
   // At v4.0.1.0 ship the bootstrap is empty (Phase 8.5 populates), so
   // attach is a no-op and train_one_stretch sees just the user rows.
   // Re-derive share_root only for the bootstrap path -- symmetrical with
   // ExecuteGlobal and avoids stashing another field on LastRunState.
   {
      const char* home = std::getenv( "HOME" );
      const std::string user_data_root =
          home ? std::string( home ) + "/.config" : std::string( "/tmp" );
      const std::string share_root = PIShareRoot();
      auto paths = nukex::learning::resolve_user_data_paths( user_data_root, share_root );
      nukex::learning::attach_bootstrap( db, paths.bootstrap_db );
   }

   // Retrain Layer 3 for this stretch only. If not enough rows yet, the
   // per_param map comes back empty and we simply skip the model update.
   auto stretch_coeffs = nukex::learning::train_one_stretch(
       db, lastRun.stretch_name, /*lambda=*/ 1.0 );

   nukex::learning::close_rating_db( db );
   db = nullptr;

   if ( stretch_coeffs.per_param.empty() )
   {
      // Not enough data yet -- Layer 3 stays unchanged. Still counts as
      // a successful rating save; the row is in the DB for future fits.
      console.NoteLn( String( "NukeX: rating saved. Layer 3 will update once more rows are collected." ) );
      return;
   }

   // Load existing user models, replace the retrained stretch, serialise
   // to a JSON string, then atomic_write over the target file.
   //
   // Corrupt-file guard: read_param_models_json clears `out` on any parse
   // failure. If the user file exists but is unparseable, blindly merging
   // into the empty map and writing back would silently destroy the
   // other stretches' coefficients. Preserve the corrupt file for forensics
   // and keep the rating row -- Layer 3 for this stretch stays stale until
   // the user resolves the corruption (or a future Reset-to-bootstrap
   // escape hatch lands in the deferred polish release).
   nukex::ParamModelMap models;
   if ( std::filesystem::exists( lastRun.user_model_json_path ) )
   {
      if ( !nukex::read_param_models_json( lastRun.user_model_json_path, models ) )
      {
         console.CriticalLn( String(
             "NukeX: user model JSON is corrupt; rating row saved but Layer 3 "
             "not rewritten. Original file preserved for forensics." ) );
         return;
      }
   }
   // else: fresh install, no user model yet -- proceed with empty map.

   nukex::ParamModel updated( lastRun.stretch_name );
   for ( const auto& [pname, c] : stretch_coeffs.per_param )
   {
      nukex::ParamCoefficients out;
      out.feature_mean = c.feature_mean;
      out.feature_std  = c.feature_std;
      out.coefficients = c.coefficients;
      out.intercept    = c.intercept;
      out.lambda       = c.lambda;
      out.n_train_rows = c.n_train_rows;
      out.cv_r_squared = c.cv_r_squared;
      updated.add_param( pname, std::move( out ) );
   }

   models.erase( lastRun.stretch_name );
   if ( !updated.empty() )
      models.emplace( lastRun.stretch_name, std::move( updated ) );

   // Stage-serialise: write_param_models_json takes a path. Write to a
   // staging path, slurp it back into a string, then atomic_write_file
   // over user_model_json_path. A future refactor should add a to_string()
   // overload; keeping the dance inline for v4.0.1.0 avoids touching the
   // serialiser signature and its callers.
   const std::string stage_path = lastRun.user_model_json_path + ".stage";
   if ( !nukex::write_param_models_json( models, stage_path ) )
   {
      console.CriticalLn( String(
          "NukeX: couldn't stage retrained model; rating row saved but Layer 3 stale." ) );
      return;
   }

   std::string staged_contents;
   {
      std::ifstream stage( stage_path, std::ios::binary );
      std::stringstream buf;
      buf << stage.rdbuf();
      staged_contents = buf.str();
   }
   std::error_code ec_rm;
   std::filesystem::remove( stage_path, ec_rm );

   if ( !nukex::learning::atomic_write_file( lastRun.user_model_json_path, staged_contents ) )
   {
      console.CriticalLn( String(
          "NukeX: couldn't persist retrained model; rating row saved but Layer 3 stale." ) );
      return;
   }

   console.NoteLn( String(
       IsoString( "NukeX: Layer 3 coefficients updated for " )
       + lastRun.stretch_name.c_str() + "." ) );
}

NukeXInstance::NukeXInstance( const MetaProcess* m )
   : ProcessImplementation( m )
{
}

NukeXInstance::NukeXInstance( const NukeXInstance& x )
   : ProcessImplementation( x )
{
   Assign( x );
}

void NukeXInstance::Assign( const ProcessImplementation& p )
{
   const NukeXInstance* x = dynamic_cast<const NukeXInstance*>( &p );
   if ( x != nullptr )
   {
      lightFrames      = x->lightFrames;
      flatFrames       = x->flatFrames;
      primaryStretch   = x->primaryStretch;
      finishingStretch = x->finishingStretch;
      backgroundTarget = x->backgroundTarget;
      removeSkyGradient = x->removeSkyGradient;
      estimator        = x->estimator;
      enableGPU        = x->enableGPU;
      cacheDirectory   = x->cacheDirectory;
      qeOverridePath   = x->qeOverridePath;
   }
}

bool NukeXInstance::IsHistoryUpdater( const View& ) const
{
   return false;  // Global process — doesn't modify existing views
}

UndoFlags NukeXInstance::UndoMode( const View& ) const
{
   return UndoFlag::DefaultMode;
}

bool NukeXInstance::CanExecuteOn( const View&, String& whyNot ) const
{
   whyNot = "NukeX is a global process. It cannot be executed on views.";
   return false;
}

bool NukeXInstance::Validate( String& whyNot )
{
   if ( lightFrames.IsEmpty() )
   {
      whyNot = "No light frames specified.";
      return false;
   }
   int n_enabled = 0;
   for ( const auto& f : lightFrames )
      if ( f.enabled ) ++n_enabled;
   if ( n_enabled == 0 )
   {
      whyNot = "No light frames are enabled.";
      return false;
   }

   // Cache directory writability check.  A non-writable cache is a
   // common misconfiguration (stale /tmp, wrong mount point, etc.)
   // and previously surfaced only as cryptic I/O errors deep inside
   // the ring-buffer later in Phase A.  Catch it at Validate time so
   // PI's Execute button stays disabled with a clear message.
   if ( !cacheDirectory.IsEmpty() )
   {
      String cache = cacheDirectory.Trimmed();
      if ( !cache.IsEmpty() )
      {
         IsoString cache_utf8 = cache.ToUTF8();
         const char* p = cache_utf8.c_str();
         struct stat st;
         if ( ::stat( p, &st ) != 0 )
         {
            // The default is $HOME/.cache/nukex4/cache, which need not exist
            // yet on a fresh install; create it rather than refuse to run.
            std::error_code ec;
            std::filesystem::create_directories( p, ec );
            if ( ec || ::stat( p, &st ) != 0 )
            {
               whyNot = "Cache directory does not exist and could not be created: " + cache;
               return false;
            }
         }
         if ( !S_ISDIR( st.st_mode ) )
         {
            whyNot = "Cache directory is not a directory: " + cache;
            return false;
         }
         if ( ::access( p, W_OK ) != 0 )
         {
            whyNot = "Cache directory is not writable by the current user: " + cache;
            return false;
         }
      }
   }

   return true;
}

bool NukeXInstance::CanExecuteGlobal( String& whyNot ) const
{
   if ( lightFrames.IsEmpty() )
   {
      whyNot = "No light frames specified.";
      return false;
   }
   int n_enabled = 0;
   for ( const auto& f : lightFrames )
      if ( f.enabled ) ++n_enabled;
   if ( n_enabled == 0 )
   {
      whyNot = "No light frames are enabled.";
      return false;
   }
   return true;
}


// ── Opening a multi-slot output without inventing transparency ──────────
//
// ImageWindow with color=true treats every plane past the THIRD as an ALPHA
// channel, and PixInsight draws alpha as a transparency checkerboard. A
// broadband OSC stack carries four slots -- R, G, B and a synthesized rec709
// L -- so `NukeX_stacked` and `NukeX_noise` were handing their luminance to
// PixInsight as transparency. The user sees a crosshatched image and no data.
//
// v5.0.3.2 found exactly this defect and fixed it for the STRETCHED window by
// routing that one through the 3-channel composed image. It left these two,
// and the E2E records `nc` without ever asserting it, so a 4-channel output
// sailed through every release since.
//
// R, G and B are selected BY NAME rather than by position: an LRGB-mono batch
// merges its slots in the order the frames arrive, so channel 0 is as likely
// to be L as R, and taking "the first three" would show L,R,G as an RGB
// triplet. Every slot that is not one of those three gets its own
// single-channel window, which loses nothing and keeps a real L inspectable.
static void OpenSlotWindows( const nukex::Image& img,
                             const nukex::ChannelConfig* cfg,
                             const IsoString& base_id,
                             const pcl::FITSKeywordArray& ka )
{
   const int w  = img.width();
   const int h  = img.height();
   const int nc = img.n_channels();
   if ( w <= 0 || h <= 0 || nc <= 0 )
      return;

   auto slot_of = [&]( int i ) -> std::string {
      if ( cfg != nullptr && i < static_cast<int>( cfg->n_channels ) )
         return cfg->slot_name( i );
      return std::string();
   };
   auto index_of = [&]( const char* name ) -> int {
      for ( int i = 0; i < nc; ++i )
         if ( slot_of( i ) == name )
            return i;
      return -1;
   };

   // Copy an arbitrary set of source planes into a freshly created window.
   auto emit = [&]( const Array<int>& planes, bool colour, const IsoString& id )
   {
      ImageWindow win( w, h, planes.Length(), 32, true, colour, true, id );
      View view = win.MainView();
      ImageVariant v = view.Image();
      if ( v.IsFloatSample() && v.BitsPerSample() == 32 )
      {
         pcl::Image& dst_img = static_cast<pcl::Image&>( *v );
         for ( size_type k = 0; k < planes.Length(); ++k )
         {
            const float* src = img.channel_data( planes[k] );
            float* dst = dst_img.PixelData( static_cast<int>( k ) );
            ::memcpy( dst, src, static_cast<size_t>( w ) * h * sizeof( float ) );
         }
      }
      win.SetKeywords( ka );
      win.Show();
   };

   const int ri = index_of( "R" ), gi = index_of( "G" ), bi = index_of( "B" );
   const bool have_rgb = ( ri >= 0 && gi >= 0 && bi >= 0 );

   if ( !have_rgb || nc <= 3 )
   {
      // Nothing to split: at three planes or fewer PixInsight has no spare
      // plane to reinterpret, and without a named R/G/B triple there is no
      // colour image to build. Emit as-is, which is the pre-existing
      // behaviour and stays bit-identical for every mono and dual-NB stack.
      Array<int> all;
      for ( int i = 0; i < nc; ++i )
         all.Add( i );
      emit( all, nc >= 3, base_id );
      return;
   }

   Array<int> rgb;
   rgb.Add( ri ); rgb.Add( gi ); rgb.Add( bi );
   emit( rgb, true, base_id );

   for ( int i = 0; i < nc; ++i )
   {
      if ( i == ri || i == gi || i == bi )
         continue;
      std::string slot = slot_of( i );
      if ( slot.empty() )
         slot = "ch" + std::to_string( i );
      // Window identifiers accept only alphanumerics and underscore.
      for ( char& c : slot )
         if ( !std::isalnum( static_cast<unsigned char>( c ) ) )
            c = '_';
      Array<int> one;
      one.Add( i );
      emit( one, false, base_id + "_" + IsoString( slot.c_str() ) );
   }
}


bool NukeXInstance::ExecuteGlobal()
{
   // Banner first, before anything else reaches the console.  It is UTF-8
   // block art, so it has to go through UTF8ToUTF16 -- String(const char*)
   // would decode it as ISO-8859-1 and ship garbage.  One WriteLn per row,
   // because the rows are joined with '\n' rather than console <br> tags.
   {
      pcl::Console console;
      const std::string banner = nukex::nukex_banner();
      for ( std::size_t start = 0; start <= banner.size(); )
      {
         const std::size_t nl  = banner.find( '\n', start );
         const std::size_t len = ( nl == std::string::npos ) ? std::string::npos
                                                             : nl - start;
         console.WriteLn( String::UTF8ToUTF16( banner.substr( start, len ).c_str() ) );
         if ( nl == std::string::npos )
            break;
         start = nl + 1;
      }
      console.WriteLn( String() );
      console.Flush();
   }

   NukeXProgress progress;
   progress.message( "NukeX " NUKEX_VERSION_STRING " -- Distribution-Fitted Stacking" );
   progress.message( String().Format( "Processing %zu light frame(s)", lightFrames.Length() ).ToUTF8().c_str() );

   // Collect enabled file paths
   std::vector<std::string> light_paths;
   for ( const auto& f : lightFrames )
   {
      if ( f.enabled )
         light_paths.push_back( f.path.ToUTF8().c_str() );
   }

   std::vector<std::string> flat_paths;
   for ( const auto& f : flatFrames )
   {
      if ( f.enabled )
         flat_paths.push_back( f.path.ToUTF8().c_str() );
   }

   if ( light_paths.empty() )
   {
      progress.message( "** No enabled light frames. Aborting." );
      return false;
   }

   // Advisory: Student-t / contamination fitters need n ≥ 5 samples per
   // voxel to converge; with fewer frames, Phase B falls back to robust
   // stats only and distributional modelling is skipped.  Surface this
   // so users understand the quality cliff they're on.
   if ( light_paths.size() < 5 )
   {
      progress.message( String().Format(
         "** WARNING ** Only %zu enabled light frame(s).  Distribution "
         "fitting needs n >= 5; stacking will fall back to robust "
         "statistics per voxel and skip Student-t / contamination "
         "modelling.  Add more frames for full Phase B output quality.",
         light_paths.size() ).ToUTF8().c_str() );
   }

   // Configure stacking engine
   nukex::StackingEngine::Config config;
   config.cache_dir = cacheDirectory.ToUTF8().c_str();
   config.gpu_config.force_cpu_fallback = !enableGPU;
   config.remove_sky_gradient = removeSkyGradient;
   config.estimator = ( estimator == NXEstimator::Huber )
       ? nukex::StackingEngine::Config::Estimator::HUBER
       : nukex::StackingEngine::Config::Estimator::MODEL_RACE;
   config.qe_override_path = qeOverridePath.ToUTF8().c_str();
   // Empty unless the updater has downloaded a newer camera database; empty
   // selects the database compiled into this module (no file to install, no
   // working-directory dependency).
   config.qe_database_path = ResolveQEDatabasePath();

   // Filter names this user has taught NukeX, beside the Phase 8 state.
   const std::string alias_path = QEUserDataRoot() + "/filter_aliases.json";
   config.filter_alias_path = alias_path;

   // Execute pipeline with progress reporting
   nukex::StackingEngine engine( config );
   auto result = engine.execute( light_paths, flat_paths, &progress );

   // An unrecognised FILTER stops the batch rather than guessing, which is
   // right, but FITS FILTER values are whatever the capture software wrote and
   // no shipped table can list them all. Offer to learn this one, then run
   // again -- once. A second failure is shown as itself, not asked about
   // again.
   if ( !result.ok && !result.unknown_filter.empty()
        && std::getenv( "NUKEX_PHASE8_NO_POPUP" ) == nullptr )
   {
      FilterDialog dlg( result.unknown_filter );
      auto taught = dlg.Run();
      if ( taught.saved )
      {
         nukex::FilterAliasStore aliases;
         aliases.load( alias_path );          // a missing file is fine
         aliases.set( result.unknown_filter, taught.canonical );
         if ( aliases.save( alias_path ) )
         {
            progress.message( String().Format(
               "Filter '%s' recorded as %s in %s. Re-stacking.",
               result.unknown_filter.c_str(), taught.canonical.c_str(),
               alias_path.c_str() ).ToUTF8().c_str() );
            nukex::StackingEngine retry( config );
            result = retry.execute( light_paths, flat_paths, &progress );
         }
         else
         {
            progress.message( String().Format(
               "** Could not write %s, so the filter was not remembered.",
               alias_path.c_str() ).ToUTF8().c_str() );
         }
      }
   }

   // Check cancellation
   if ( progress.is_cancelled() )
   {
      progress.message( "** Cancelled by user." );
      return false;
   }

   // The engine refused the batch. It says why, and the message is written to
   // be acted on, so the job here is only to add the install hint when the
   // install is genuinely the problem.
   //
   // This used to append "share/qe_database.json is missing from the plugin
   // install" to EVERY failure. A user whose filter simply was not in the
   // database -- one line after the console announced updating that same
   // database -- was told to go and check their installation. The shipped
   // database is now compiled in and cannot be missing; the one install-side
   // cause left is a downloaded database that fails to load, so name it.
   if ( !result.ok )
   {
      String msg = String().Format( "** %s", result.error.c_str() );
      if ( UsingDownloadedQEDatabase() && result.error.find( "QE database" ) != std::string::npos )
         msg += String().Format(
            "\n** NukeX is using a downloaded camera database (%s). Delete that "
            "file to fall back to the database built into the module.",
            ResolveQEDatabasePath().c_str() );
      progress.message( msg.ToUTF8().c_str() );
      return false;
   }

   if ( result.n_frames_processed == 0 )
   {
      progress.message( "** No frames were processed. Check input files." );
      return false;
   }

   // Publish result counts to PJSR-readable output parameters before logging.
   nFramesProcessed        = result.n_frames_processed;
   nFramesFailedAlignment  = result.n_frames_failed_alignment;
   nFramesRejectedFilter   = result.n_frames_rejected_filter;

   progress.message( String().Format(
      "Stacking complete: %d frame(s) processed, %d failed alignment, %d rejected (unknown filter)",
      result.n_frames_processed, result.n_frames_failed_alignment,
      result.n_frames_rejected_filter ).ToUTF8().c_str() );

   // Warn if any frames were dropped due to unknown filter coverage.
   if ( result.n_frames_rejected_filter > 0 )
   {
      progress.message( String().Format(
         "** WARNING ** %d frame(s) rejected: unknown FILTER value on Bayer frame. "
         "Add the filter to qe_overrides.json to recover those frames in a future run.",
         result.n_frames_rejected_filter ).ToUTF8().c_str() );
   }

   // Loud warnings on low alignment success rate.  Before v4.0.0.5,
   // a broken matcher silently weight-penalised 61/65 frames and the
   // only indicator was a buried summary line — the whole class of
   // "it ran, but most of my signal was at half weight" regression
   // can't be allowed to be quiet again.
   //
   // Rate is computed over frames that actually reached the aligner
   // (n_frames_processed). Filter-rejected frames (n_frames_rejected_filter)
   // are excluded — they never touched the aligner, so counting them
   // here would falsely inflate the alignment-failure percentage.
   //
   // Thresholds:
   //   > 50% failed  → ** CRITICAL ** with remediation hint
   //   10–50% failed → ** WARNING  ** so users notice but can decide
   //   ≤ 10% failed  → no warning (some drift / clouded frames is normal)
   if ( result.n_frames_processed > 0 )
   {
      double pct = 100.0 * double( result.n_frames_failed_alignment )
                        / double( result.n_frames_processed );
      if ( pct > 50.0 )
      {
         progress.message( String().Format(
            "** CRITICAL ** %.1f%% of frames (%d of %d) failed alignment. "
            "Stacked result uses weight-penalised frames; SNR is significantly "
            "degraded.  Check that the first light frame is a reasonable "
            "reference (no clouds, well-tracked, enough stars), and that all "
            "lights cover a similar field.",
            pct, result.n_frames_failed_alignment,
            result.n_frames_processed ).ToUTF8().c_str() );
      }
      else if ( pct > 10.0 )
      {
         progress.message( String().Format(
            "** WARNING ** %.1f%% of frames (%d of %d) failed alignment and "
            "are stacked at 0.5x weight penalty.  Review the per-frame 'aligned'"
            " log lines above for which frames are affected.",
            pct, result.n_frames_failed_alignment,
            result.n_frames_processed ).ToUTF8().c_str() );
      }
   }

   // Create output ImageWindow with the stacked result
   if ( !result.stacked.empty() )
   {
      // Provenance: stamp the FITS header so this window round-trips
      // through Save/Load with NukeX identity, version, and the run's
      // alignment-result counters intact.
      pcl::FITSKeywordArray ka = base_output_keywords(
          NUKEX_VERSION_STRING, "stacked",
          result.n_frames_processed, result.n_frames_failed_alignment );
      append_calibration_keywords( ka, result );

      OpenSlotWindows( result.stacked,
                       result.cube ? &result.cube->channel_config : nullptr,
                       "NukeX_stacked", ka );
      progress.message( "Stacked image opened." );
   }

   // The colour-science image: composed once here, shown as NukeX_composed
   // and then STRETCHED below as NukeX_stretched.
   //
   // Until v5.0.3.2 the stretch ran on result.stacked -- the raw per-channel
   // accumulation -- which never passes through ColorComposer. That is not a
   // subtle difference. On a 156-frame OSC stack of M63 the raw channels
   // measured R 1.000 G 1.000 B 0.955, saturation 0.080; the same target
   // integrated in PixInsight measures 1.000 / 0.736 / 0.660 at 0.337. The
   // stretched image -- which is the whole point of the program, an STF that
   // knows more than STF does -- was the one output that never saw the
   // colour science.
   nukex::Image composed_image;
   // Hoisted: the stretch below reuses the composer -- with the chroma gate
   // the compose step measured -- for the emission-line route.
   nukex::ColorComposer composer;
   bool have_emission_slots = false;
   nukex::Image gate_plane;   // the smoothed emission total the chroma gate judges

   // ── ColorComposer-driven 3-channel sRGB output (Task 12) ─────
   //
   // Compose a 3-channel sRGB ImageWindow from the Phase B derived semantic
   // slots (result.derived). ColorComposer turns each pixel's slot tuple
   // into an sRGB triplet via Lab/LCH composition. This is the primary
   // color-science display for v5; it coexists with the raw stacked window
   // and the stretch pipeline output below (which still reads result.stacked
   // for broadband/mono luminance work — kept for backwards compatibility).
   if ( !result.derived.slots.empty() )
   {
      const int w = result.derived.width;
      const int h = result.derived.height;
      const int N = w * h;

      // Pre-resolve the emission slot pointers the chroma gate samples.
      // compose_slots_to_image resolves the full set once for itself.
      auto slot_ptr = [&]( const std::string& name ) -> const float* {
         auto it = result.derived.slots.find( name );
         return it == result.derived.slots.end() ? nullptr : it->second.data();
      };
      const float* Ha_p   = slot_ptr( "Ha"   );
      const float* OIII_p = slot_ptr( "OIII" );
      const float* SII_p  = slot_ptr( "SII"  );
      have_emission_slots = ( Ha_p != nullptr ) || ( OIII_p != nullptr ) || ( SII_p != nullptr );

      // Chroma gate, driven by the stack's own statistics rather than a
      // constant. Normalising the emission chrominance by its total makes
      // hue a line ratio (Lupton et al. 2004), but it also means blank sky
      // would normalise to a bold colour. So chrominance vanishes at the
      // measured sky level and reaches full strength three robust sigma
      // above it. Sampled on a stride -- a median over every pixel of a
      // 24 MP frame would copy ~200 MB to produce one number.
      if ( Ha_p || OIII_p || SII_p )
      {
         const int stride = std::max( 1, N / 200000 );

         // Each line's sky level first: hue is a ratio of line FLUXES, and
         // the derived planes carry the sky pedestal of the channels they
         // were solved from. On a faint stack the pedestal is most of the
         // number (M16, 12 frames: Ha 0.0247 sky, nebula +0.0010), so a raw
         // ratio makes every pixel a 50/50 mix. The composer subtracts these
         // before it weighs the lines.
         auto plane_median = [&]( const float* plane ) -> double {
            if ( !plane ) return 0.0;
            std::vector<double> v;
            v.reserve( static_cast<std::size_t>( N / stride ) + 1 );
            for ( int p = 0; p < N; p += stride ) v.push_back( plane[p] );
            const std::size_t mid = v.size() / 2;
            std::nth_element( v.begin(), v.begin() + mid, v.end() );
            return v[mid];
         };
         const double sky_ha = plane_median( Ha_p ), sky_oiii = plane_median( OIII_p ), sky_sii = plane_median( SII_p );
         composer.set_line_backgrounds( sky_ha, sky_oiii, sky_sii );
         Console().WriteLn( String().Format(
            "Line sky levels subtracted before the ratio: Ha %.6f, OIII %.6f, SII %.6f.",
            sky_ha, sky_oiii, sky_sii ) );

         // The gate is judged on the sky-subtracted total SMOOTHED over a 7x7
         // neighbourhood. Extended faint emission is real when its
         // neighbourhood is; a lone noise excursion is not. Judged per pixel
         // the gate either painted sky noise (a ramp from the sky level) or
         // cut the nebula's outskirts to grey at a hard edge (a per-pixel
         // detection threshold); over a neighbourhood the noise averages
         // down sevenfold and the outskirts keep their colour.
         gate_plane = nukex::box_smooth(
             nukex::emission_total_image( w, h, result.derived.slots, composer ), 3 );
         const nukex::GateStats gs = nukex::gate_statistics( gate_plane );
         if ( gs.valid )
         {
            composer.set_chroma_gate( gs.start, gs.full );
            Console().WriteLn( String().Format(
               "Chroma gate on the 7x7-smoothed emission total: sky %.6f (lower quartile), "
               "noise %.6f (lag-16 differences); no colour below %.6f (+3 sigma), full at %.6f (+6 sigma).",
               gs.sky, gs.sigma, gs.start, gs.full ) );
         }
         else
         {
            // A constant emission field has no noise scale to gate on.
            // Say so rather than silently leaving colour ungated.
            Console().WarningLn(
               "Chroma gate disabled: the emission total has no measurable noise, "
               "so no sky level could be established." );
         }
      }

      ImageWindow cw( w, h, /*nc*/ 3,
                      32, true, true, true,
                      "NukeX_composed" );
      View cv = cw.MainView();
      ImageVariant cvi = cv.Image();

      // Compose ONCE. The same pixels are shown here and stretched below;
      // composing twice would double a Lab/LCH solve over every pixel.
      composed_image = nukex::compose_slots_to_image(
          w, h, result.derived.slots, composer, gate_plane.empty() ? nullptr : &gate_plane );

      if ( cvi.IsFloatSample() && cvi.BitsPerSample() == 32 && !composed_image.empty() )
      {
         pcl::Image& ci = static_cast<pcl::Image&>( *cvi );
         for ( int ch = 0; ch < 3; ++ch )
            ::memcpy( ci.PixelData( ch ), composed_image.channel_data( ch ),
                      static_cast<std::size_t>( N ) * sizeof( float ) );
      }

      // Provenance keywords: include gamut-clip diagnostic so users can
      // compare palette choices and see how many pixels needed clipping.
      pcl::FITSKeywordArray cw_ka = base_output_keywords(
          NUKEX_VERSION_STRING, "composed",
          result.n_frames_processed, result.n_frames_failed_alignment );
      append_calibration_keywords( cw_ka, result );
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_GAMUT_CLIPPED",
          pcl::IsoString().Format( "%lld",
              static_cast<long long>( composer.gamut_clipped_count() ) ),
          "Pixels clipped to sRGB gamut by ColorComposer" ) );
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_QE_CONFIDENCE",
          result.qe_generic_camera_fallback ? "generic-fallback" : "database",
          "QE source for Phase B: camera entry or generic Sony OSC fallback" ) );
      // A camera database that can update itself would otherwise make a
      // changed result unexplainable: same frames, different pixels, no
      // record of why. This keyword is that record.
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_QE_DB_VERSION",
          pcl::IsoString().Format( "%d", ActiveQEDatabaseVersion() ),
          "QE camera database version used for the Phase B solve" ) );
      cw.SetKeywords( cw_ka );

      cw.Show();
      progress.message( String().Format(
         "Composed image opened (%lld pixel(s) gamut-clipped).",
         static_cast<long long>( composer.gamut_clipped_count() ) ).ToUTF8().c_str() );
   }

   // ── Stretch pipeline (Phase 7 wiring + Phase 8 context) ──────
   //
   // Phase 8 Layer 3 → Layer 2 → Layer 1 fallback runs inside build_primary.
   // At v4.0.1.0 ship, Layer 2 ships empty (bootstrap deferred to Phase 8.5)
   // and Layer 3 is empty until the user saves a rating, so this reduces to
   // factory defaults — preserving bit-identical output vs v4.0.0.8.
   if ( !result.stacked.empty() && !light_paths.empty() )
   {
      // Stretch the colour-composed image when there is one. A mono batch has
      // no colour slots to compose, so it keeps its single channel rather than
      // being widened to a grey RGB triplet.
      //
      // This also ends a second defect. result.stacked carries one plane per
      // CUBE slot -- four for a broadband OSC stack, R G B and a synthesized
      // rec709 L -- and ImageWindow with color=true turns every plane past the
      // third into an ALPHA channel. NukeX was shipping that synthesized
      // luminance as transparency, unstretched, on every OSC and LRGB run.
      const bool have_colour =
          nukex::slots_have_colour( result.derived.slots ) && !composed_image.empty();

      // Emission-line data takes a different route. The composed image is
      // gamut-mapped at the LINEAR luminance of the data, where L* is a few
      // units and sRGB holds almost no chroma, so the palette has already
      // been walked to grey by the time it reaches the stretch -- measured on
      // M16: raw channels 0.099, composed 0.018, stretched 0.011. Hue and
      // chroma must not depend on brightness (Lupton et al. 2004), so they
      // stay the linear line ratios; only LIGHTNESS is stretched. The
      // luminance plane goes through the stretch alone, and the colour is
      // composed at the stretched L*, gamut-mapped once, there.
      //
      // Broadband stacks keep the composed-image route: their chroma is the
      // natural Lab chroma of the RGB data, which is small at low luminance
      // by construction and would not grow with L* the way the palette does.
      const bool emission_route = have_colour && have_emission_slots;
      nukex::Image emission_luminance;
      if ( emission_route )
         emission_luminance = nukex::compose_luminance_image(
             result.derived.width, result.derived.height, result.derived.slots );
      const nukex::Image& stretch_source =
          emission_route ? emission_luminance
        : have_colour    ? composed_image
                         : result.stacked;

      nukex::FrameMetadata meta = nukex::FITSReader::read_headers( light_paths.front() );

      // Resolve Phase 8 file paths. user_data_root is where per-user rating
      // DB + trained-model JSON live; share_root is where the read-only
      // bootstrap ships (absent today — LayerLoader falls back cleanly).
      const char* home = std::getenv( "HOME" );
      const std::string user_data_root =
          home ? std::string( home ) + "/.config" : std::string( "/tmp" );
      const std::string share_root = PIShareRoot();

      auto paths = nukex::learning::resolve_user_data_paths( user_data_root, share_root );

      nukex::LayerLoader layer_loader( paths.bootstrap_model_json,
                                       paths.user_model_json );
      nukex::ImageStats  stats = nukex::compute_image_stats( stretch_source );
      nukex::Phase8Context p8{ &layer_loader, &stats };

      std::string auto_log;
      auto primary_op   = nukex::build_primary(
          static_cast<nukex::PrimaryStretch>( primaryStretch ), meta, auto_log, &p8 );
      auto finishing_op = nukex::build_finishing(
          static_cast<nukex::FinishingStretch>( finishingStretch ) );

      if ( !auto_log.empty() )
         progress.message( auto_log.c_str() );

      // Solve the stretch intensity against THIS image rather than shipping
      // one number for every target. Measured across the regression corpora
      // the linear background spans 0.0166 to 0.1479 -- nearly an order of
      // magnitude -- so a single log_D put one stack's median at 0.08 and
      // another's at 0.43. Raising it globally is not the answer either: it
      // brightens but flattens, and on the mono corpus the spread between the
      // median and the 99.9th percentile FALLS from 0.191 to 0.073 as log_D
      // goes 2 to 5.
      //
      // The shadow point is the other half, and it is the one that buys
      // contrast: intensity MOVES the histogram, only a shadow point WIDENS
      // it. On a 74-frame stack whose p99.9 sits 44 sigma above the
      // background, solving both took the luminance spread from 0.040 to
      // 0.299 with the background landing on 0.2501.
      if ( primary_op != nullptr && primary_op->name == "VeraLux" )
      {
         if ( auto* vl = dynamic_cast<nukex::VeraLuxStretch*>( primary_op.get() ) )
         {
            const float solved = vl->auto_tune( stretch_source, backgroundTarget );
            progress.message( pcl::String().Format(
               "Stretch solved for this image: shadow point = %.4f, "
               "log_D = %.2f (background target %.3f).",
               vl->SP, solved, backgroundTarget ).ToUTF8().c_str() );
         }
      }

      // Capture last-run state BEFORE the unique_ptr is moved into the
      // pipeline, so we can read op.name / op.get_param() cheaply. The
      // rating dialog (below) and Task 18's "Rate last run" button both
      // depend on lastRun being populated for every successful Execute.
      if ( primary_op )
      {
         lastRun.valid               = true;
         lastRun.stats               = stats;
         lastRun.stretch_name        = primary_op->name;
         {
            nukex::FilterClassifier classifier;
            lastRun.filter_class = filter_class_to_rating_int( classifier.classify( meta ).cls );
         }
         lastRun.target_class        = 0; // TODO(Phase 8.5): FITS OBJECT -> class
         lastRun.params_json_applied = op_trainable_params_json( *primary_op );
         // Fresh 128-bit run id. std::rand is not seeded anywhere in NukeX
         // and would repeat across PI launches, so Task 19's DB would get
         // silent primary-key collisions; use random_device-seeded mt19937
         // instead. ExecuteGlobal is UI-thread only, so the static RNG is safe.
         {
            static std::mt19937_64 rng{ std::random_device{}() };
            std::uniform_int_distribution<int> byte_dist( 0, 255 );
            for ( auto& byte : lastRun.run_id )
               byte = static_cast<std::uint8_t>( byte_dist( rng ) );
         }
         lastRun.created_at_unix     =
             std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch() ).count();
         lastRun.user_db_path         = paths.user_db;
         lastRun.user_model_json_path = paths.user_model_json;
      }
      else
      {
         lastRun.valid = false;
      }

      // Deep copy — stretch is in-place; must not mutate result.stacked.
      // Safe: nukex::Image stores pixels in std::vector<float>, so operator=
      // performs a full element-wise deep copy (no shared buffer).
      nukex::Image stretched = stretch_source;

      nukex::StretchPipeline pipeline;
      if ( primary_op )
      {
         primary_op->enabled  = true;
         primary_op->position = 0;
         pipeline.ops.push_back( std::move( primary_op ) );
      }
      if ( finishing_op )
      {
         finishing_op->enabled  = true;
         finishing_op->position = 1;
         pipeline.ops.push_back( std::move( finishing_op ) );
      }
      pipeline.execute( stretched );

      if ( emission_route )
      {
         // Colour at the stretched lightness. Same slots, same gate, same
         // palette as the composed window; only L* differs.
         stretched = nukex::compose_slots_with_luminance(
             result.derived.width, result.derived.height, result.derived.slots,
             composer, stretched, gate_plane.empty() ? nullptr : &gate_plane );
         progress.message( "Emission-line colour composed at the stretched luminance." );
      }

      int sw  = stretched.width();
      int sh  = stretched.height();
      int snc = stretched.n_channels();

      ImageWindow sw_win( sw, sh, snc, 32, true, snc >= 3, true, "NukeX_stretched" );
      View sv = sw_win.MainView();
      ImageVariant svi = sv.Image();
      if ( svi.IsFloatSample() && svi.BitsPerSample() == 32 )
      {
         pcl::Image& si = static_cast<pcl::Image&>( *svi );
         for ( int ch = 0; ch < snc; ch++ )
         {
            const float* src = stretched.channel_data( ch );
            float* dst = si.PixelData( ch );
            ::memcpy( dst, src, sw * sh * sizeof( float ) );
         }
      }
      // Provenance on the stretched window: include the primary +
      // finishing stretch enum labels so a user inspecting the saved
      // FITS later can tell which curve produced the image.
      pcl::FITSKeywordArray sw_ka = base_output_keywords(
          NUKEX_VERSION_STRING, "stretched",
          result.n_frames_processed, result.n_frames_failed_alignment );
      append_calibration_keywords( sw_ka, result );
      static const char* kPrimaryNames[] = {
          "Auto", "VeraLux", "GHS", "MTF", "ArcSinh", "Log", "Lupton", "CLAHE"
      };
      const int pe = static_cast<int>( primaryStretch );
      const int ne = static_cast<int>(
          sizeof(kPrimaryNames) / sizeof(kPrimaryNames[0]) );
      const char* primary_name =
          ( pe >= 0 && pe < ne ) ? kPrimaryNames[pe] : "Unknown";
      sw_ka.Append( pcl::FITSHeaderKeyword(
          "PRIMSTR", pcl::IsoString( "'" ) + primary_name + "'",
          "Primary stretch curve applied" ) );
      sw_ka.Append( pcl::FITSHeaderKeyword(
          "FINSTR", pcl::IsoString( "'None'" ),
          "Finishing stretch (only None enrolled today)" ) );
      sw_win.SetKeywords( sw_ka );

      sw_win.Show();
      progress.message( "Stretched image opened." );

      // ── Phase 8 rating popup ─────────────────────────────────
      // Skip unconditionally when:
      //   (a) NUKEX_PHASE8_NO_POPUP is set — headless / E2E harness
      //   (b) the user has opted out via the "don't show again" checkbox
      //       (Task 18 persists this through PCL Settings)
      // Otherwise, open the dialog and let Task 19 persist anything the
      // user clicks Save on.  save_rating_from_last_run is currently a
      // no-op stub so the E2E regression is unaffected.
      const bool headless = ( std::getenv( "NUKEX_PHASE8_NO_POPUP" ) != nullptr );
      if ( lastRun.valid && !headless
           && TheNukeXProcess != nullptr
           && !TheNukeXProcess->rating_popup_suppressed() )
      {
         pcl::RatingDialog dlg( lastRun.filter_class );
         pcl::RatingResult res = dlg.Run();
         if ( res.dont_show_again )
            TheNukeXProcess->set_rating_popup_suppressed( true );
         if ( res.saved )
            SaveRatingFromLastRun( res );
      }
   }

   // Emission-line planes as their own windows.
   //
   // A narrowband imager processes Ha and OIII separately as a matter of
   // course, and until now NukeX composed them and threw the planes away.
   // These are the Q-solve's output in the stack's own linear units, before
   // any palette: the line ratio the colour is built from, as data.
   for ( const char* line : { "Ha", "OIII", "SII" } )
   {
      auto it = result.derived.slots.find( line );
      if ( it == result.derived.slots.end() )
         continue;
      const int lw_w = result.derived.width, lw_h = result.derived.height;
      if ( lw_w <= 0 || lw_h <= 0 ||
           it->second.size() < static_cast<size_t>( lw_w ) * static_cast<size_t>( lw_h ) )
         continue;
      ImageWindow lw( lw_w, lw_h, 1, 32, true, false, true, IsoString( "NukeX_" ) + line );
      View lv = lw.MainView();
      ImageVariant lvi = lv.Image();
      if ( lvi.IsFloatSample() && lvi.BitsPerSample() == 32 )
      {
         pcl::Image& li = static_cast<pcl::Image&>( *lvi );
         ::memcpy( li.PixelData( 0 ), it->second.data(),
                   static_cast<size_t>( lw_w ) * lw_h * sizeof( float ) );
      }
      const std::string kind = std::string( "line_" ) + line;
      pcl::FITSKeywordArray lka = base_output_keywords(
          NUKEX_VERSION_STRING, kind.c_str(),
          result.n_frames_processed, result.n_frames_failed_alignment );
      append_calibration_keywords( lka, result );
      lw.SetKeywords( lka );
      lw.Show();
      progress.message( ( std::string( line ) + " line plane opened." ).c_str() );
   }

   // Create noise map window
   if ( !result.noise_map.empty() )
   {
      pcl::FITSKeywordArray ka = base_output_keywords(
          NUKEX_VERSION_STRING, "noise",
          result.n_frames_processed, result.n_frames_failed_alignment );
      append_calibration_keywords( ka, result );

      OpenSlotWindows( result.noise_map,
                       result.cube ? &result.cube->channel_config : nullptr,
                       "NukeX_noise", ka );
      progress.message( "Noise map opened." );
   }

   // Create measured-noise window.
   //
   // NukeX_noise is the noise the CCD model PREDICTS for each estimate.
   // NukeX_measured_noise is the scatter the finished stack actually has.
   // Where the second exceeds the first, something between the samples and
   // the pixel is adding noise of its own -- a thing the predicted map is
   // structurally unable to show. Single channel (the spatial kernel measures
   // on a luminance window), so it is opened without a slot mapping.
   if ( !result.measured_noise.empty() )
   {
      pcl::FITSKeywordArray ka = base_output_keywords(
          NUKEX_VERSION_STRING, "measured_noise",
          result.n_frames_processed, result.n_frames_failed_alignment );
      append_calibration_keywords( ka, result );

      OpenSlotWindows( result.measured_noise, nullptr,
                       "NukeX_measured_noise", ka );
      progress.message( "Measured noise map opened." );
   }

   progress.message( "NukeX done." );
   return true;
}

// ── Parameter serialization ──────────────────────────────────────

void* NukeXInstance::LockParameter( const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXLightFramePathParameter )    return lightFrames[tableRow].path.Begin();
   if ( p == TheNXLightFrameEnabledParameter ) return &lightFrames[tableRow].enabled;
   if ( p == TheNXFlatFramePathParameter )     return flatFrames[tableRow].path.Begin();
   if ( p == TheNXFlatFrameEnabledParameter )  return &flatFrames[tableRow].enabled;
   if ( p == TheNXPrimaryStretchParameter )    return &primaryStretch;
   if ( p == TheNXFinishingStretchParameter )  return &finishingStretch;
   if ( p == TheNXBackgroundTargetParameter )  return &backgroundTarget;
   if ( p == TheNXRemoveSkyGradientParameter ) return &removeSkyGradient;
   if ( p == TheNXEstimatorParameter )         return &estimator;
   if ( p == TheNXEnableGPUParameter )         return &enableGPU;
   if ( p == TheNXCacheDirectoryParameter )         return cacheDirectory.Begin();
   if ( p == TheNXQEOverridePathParameter )         return qeOverridePath.Begin();
   if ( p == TheNXNFramesProcessedParameter )       return &nFramesProcessed;
   if ( p == TheNXNFramesFailedAlignmentParameter ) return &nFramesFailedAlignment;
   if ( p == TheNXNFramesRejectedFilterParameter )  return &nFramesRejectedFilter;
   return nullptr;
}

bool NukeXInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type tableRow )
{
   if ( p == TheNXLightFramesParameter )
   {
      lightFrames.Clear();
      if ( sizeOrLength > 0 )
         lightFrames.Add( FrameItem(), sizeOrLength );
   }
   else if ( p == TheNXLightFramePathParameter )
   {
      lightFrames[tableRow].path.Clear();
      if ( sizeOrLength > 0 )
         lightFrames[tableRow].path.SetLength( sizeOrLength );
   }
   else if ( p == TheNXFlatFramesParameter )
   {
      flatFrames.Clear();
      if ( sizeOrLength > 0 )
         flatFrames.Add( FrameItem(), sizeOrLength );
   }
   else if ( p == TheNXFlatFramePathParameter )
   {
      flatFrames[tableRow].path.Clear();
      if ( sizeOrLength > 0 )
         flatFrames[tableRow].path.SetLength( sizeOrLength );
   }
   else if ( p == TheNXCacheDirectoryParameter )
   {
      cacheDirectory.Clear();
      if ( sizeOrLength > 0 )
         cacheDirectory.SetLength( sizeOrLength );
   }
   else if ( p == TheNXQEOverridePathParameter )
   {
      qeOverridePath.Clear();
      if ( sizeOrLength > 0 )
         qeOverridePath.SetLength( sizeOrLength );
   }
   else
      return false;

   return true;
}

size_type NukeXInstance::ParameterLength( const MetaParameter* p, size_type tableRow ) const
{
   if ( p == TheNXLightFramesParameter )       return lightFrames.Length();
   if ( p == TheNXLightFramePathParameter )    return lightFrames[tableRow].path.Length();
   if ( p == TheNXFlatFramesParameter )        return flatFrames.Length();
   if ( p == TheNXFlatFramePathParameter )     return flatFrames[tableRow].path.Length();
   if ( p == TheNXCacheDirectoryParameter )    return cacheDirectory.Length();
   if ( p == TheNXQEOverridePathParameter )    return qeOverridePath.Length();
   return 0;
}

} // namespace pcl
