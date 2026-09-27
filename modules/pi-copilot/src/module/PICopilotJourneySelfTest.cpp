// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentSession.h"
#include "AgentTools.h"
#include "AnthropicClient.h"
#include "ConfigDialog.h"
#include "EvalGuard.h"
#include "CopilotSettings.h"
#include "HistoryReader.h"
#include "JourneyConstants.h"
#include "JourneyExport.h"
#include "JourneyStepsDialog.h"
#include "JourneyStore.h"
#include "JourneyTracker.h"
#include "JourneyTools.h"
#include "JourneySpikeProbe.h"
#include "JourneyWriteup.h"
#include "MasterFacts.h"
#include "PICopilotInterface.h"
#include "PICopilotJourneySelfTest.h"
#include "PICopilotModule.h"
#include "PICopilotProcess.h"
#include "PjsrRunner.h"
#include "ProcessActivity.h"
#include "ProcessApply.h"
#include "SafeFileWrite.h"
#include "StepStats.h"
#include "SystemPrompt.h"
#include "ToolHelpers.h"
#include "Utf8.h"
#include "ViewCapture.h"
#include "ViewPreview.h"
#include "VisionTurn.h"
#include "SelfTestTiming.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
#include <pcl/Console.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FileFormat.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Settings.h>
#include <pcl/Thread.h>
#include <pcl/Variant.h>
#include <pcl/View.h>
#include <pcl/XML.h>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pcl
{

namespace
{

using jclock = std::chrono::steady_clock;

double MsSince( jclock::time_point t0 )
{
   return std::chrono::duration<double, std::milli>( jclock::now() - t0 ).count();
}

String JEvalJs( const String& src )
{
   // A script that returns nothing (undefined) yields an invalid Variant, whose
   // ToString() throws; statement-only scripts are "".
   // Guarded like every module caller: a timer-driven reader defers instead
   // of nesting an EvaluateScript inside this one (controller ruling).
   EvalDepthGuard guard;
   const Variant v = ThePICopilotModule->EvaluateScript( src, "JavaScript" );
   return v.IsValid() ? v.ToString() : String();
}

void JForceClose( const std::string& id )
{
   try
   {
      ImageWindow w = ImageWindow::WindowById( IsoString( id.c_str() ) );
      if ( !w.IsNull() )
         w.ForceClose();
   }
   catch ( ... )
   {
   }
}

void JPump( int ms )
{
   const jclock::time_point t0 = jclock::now();
   while ( MsSince( t0 ) < ms )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
   }
}

// A fresh directory under the system temp dir, removed (recursively) on destruction.
class JTempDir
{
public:

   explicit JTempDir( const char* prefix )
   {
      m_path = File::UniqueFileName( File::SystemTempDirectory(), 12, prefix );
      if ( !m_path.StartsWith( '/' ) )
         m_path = File::SystemTempDirectory() + '/' + m_path;
      File::CreateDirectory( m_path );
   }

   ~JTempDir()
   {
      try { RemoveDirectoryTree( m_path ); } catch ( ... ) {}
   }

   JTempDir( const JTempDir& ) = delete;
   JTempDir& operator =( const JTempDir& ) = delete;

   const String& Path() const { return m_path; }

private:

   String m_path;
};

// Hidden float window filled with one constant; force-closed on destruction.
// Holds only the window's id and re-resolves it for each use (re-review m-4r): a held ImageWindow whose
// window a test closed by id would be a stale handle whose destructor detaches a reused address.
class JWindow
{
public:

   JWindow( const char* id, int w, int h, int channels, double value )
   {
      ImageWindow win( w, h, channels, 32, true/*float*/, channels >= 3/*color*/, true/*initialProcessing*/, IsoString( id ) );
      if ( win.IsNull() )
         throw Error( String( "JWindow: null window " ) + id );
      View v = win.MainView();
      m_id = v.Id();
      AutoViewLock lock( v );
      ImageVariant iv = v.Image();
      static_cast<Image&>( *iv ).Fill( float( value ) );
   }

   ~JWindow()
   {
      try { ImageWindow w = ImageWindow::WindowById( m_id ); if ( !w.IsNull() ) w.ForceClose(); } catch ( ... ) {}
   }

   JWindow( const JWindow& ) = delete;
   JWindow& operator =( const JWindow& ) = delete;

   View MainView() const { return ImageWindow::WindowById( m_id ).MainView(); }
   ImageWindow Window() const { return ImageWindow::WindowById( m_id ); }

private:

   IsoString m_id;
};

// Deterministic Gaussian noise (xorshift + Box-Muller) around `level`, all channels.
void JFillNoise( Image& img, double level, double sigma, unsigned seed )
{
   uint32_t s = 2463534242u ^ (seed*2654435761u);
   auto u01 = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return ((s & 0xFFFFFFu) + 0.5)/double( 0x1000000 ); };
   for ( int c = 0; c < img.NumberOfChannels(); ++c )
   {
      float* p = img.PixelData( c );
      const size_type n = img.NumberOfPixels();
      for ( size_type i = 0; i < n; ++i )
      {
         const double g = std::sqrt( -2*std::log( u01() ) )*std::cos( 2*3.14159265358979323846*u01() );
         p[i] = float( level + sigma*g );
      }
   }
}

void JWriteFits( const String& path, const Image& img, const FITSKeywordArray& kw )
{
   FileFormat fits( ".fits", false/*toRead*/, true/*toWrite*/ );
   FileFormatInstance f( fits );
   if ( !f.Create( path ) )
      throw Error( "JWriteFits: cannot create " + path );
   if ( !kw.IsEmpty() && !f.WriteFITSKeywords( kw ) )
      throw Error( "JWriteFits: cannot write keywords to " + path );
   if ( !f.WriteImage( img ) )
      throw Error( "JWriteFits: cannot write " + path );
   f.Close();
}

// Largest per-pixel difference of two views' images (1e9 on a geometry mismatch).
double JMaxAbsDiff( View a, View b )
{
   AutoViewWriteLock la( a );
   AutoViewWriteLock lb( b );
   ImageVariant va = a.Image();
   ImageVariant vb = b.Image();
   const Image& ia = static_cast<const Image&>( *va );
   const Image& ib = static_cast<const Image&>( *vb );
   if ( ia.Width() != ib.Width() || ia.Height() != ib.Height() || ia.NumberOfChannels() != ib.NumberOfChannels() )
      return 1e9;
   double m = 0;
   for ( int c = 0; c < ia.NumberOfChannels(); ++c )
      for ( size_type i = 0; i < ia.NumberOfPixels(); ++i )
         m = std::max( m, std::fabs( double( ia.PixelData( c )[i] ) - ib.PixelData( c )[i] ) );
   return m;
}

// A 96x64 mono float "master" window with deterministic noise, no history.
void JMakeNoiseMaster( const char* id, unsigned seed )
{
   ImageWindow w( 96, 64, 1, 32, true, false, true, IsoString( id ) );
   if ( w.IsNull() )
      throw Error( String( "JMakeNoiseMaster: null window " ) + id );
   View v = w.MainView();
   AutoViewLock lock( v );
   ImageVariant iv = v.Image();
   JFillNoise( static_cast<Image&>( *iv ), 0.1, 0.01, seed );
}

// Used by later journey sections (plan Tasks 3-11).
[[maybe_unused]] double JMedian( View v, int ch )
{
   if ( !v.CanRead() || !v.CanWrite() )
      throw Error( "JMedian: view is busy" );
   AutoViewWriteLock lock( v );
   ImageVariant iv = v.Image();
   return iv.Median( iv.Bounds(), ch, ch );
}

// Raw second connection (the "other program" of the lock / damage tests).
struct RawDb
{
   sqlite3* db = nullptr;
   explicit RawDb( const String& path ) { sqlite3_open_v2( U8( path ).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr ); }
   ~RawDb() { if ( db != nullptr ) sqlite3_close( db ); }
   bool Exec( const char* sql ) { return sqlite3_exec( db, sql, nullptr, nullptr, nullptr ) == SQLITE_OK; }
   std::vector<std::string> Column( const char* sql )
   {
      std::vector<std::string> r;
      sqlite3_stmt* st = nullptr;
      if ( sqlite3_prepare_v2( db, sql, -1, &st, nullptr ) == SQLITE_OK )
         while ( sqlite3_step( st ) == SQLITE_ROW )
            r.push_back( sqlite3_column_text( st, 0 ) != nullptr ? reinterpret_cast<const char*>( sqlite3_column_text( st, 0 ) ) : "" );
      sqlite3_finalize( st );
      return r;
   }
};

// Runs one call on a PCL worker thread (never the root thread) and captures
// what it threw. Only the store's own guard runs there: no GUI, no views.
class JCallThread : public Thread
{
public:

   explicit JCallThread( std::function<void()> fn ) : m_fn( std::move( fn ) ) {}

   void Run() override
   {
      try { m_fn(); }
      catch ( const pcl::Exception& x ) { threw = true; error = x.Message(); }
      catch ( const std::exception& x ) { threw = true; error = String( x.what() ); }
      catch ( ... )                     { threw = true; error = "unknown exception"; }
   }

   bool   threw = false;
   String error;

private:

   std::function<void()> m_fn;
};

std::string FileBytes( const String& path )
{
   if ( !File::Exists( path ) )
      return std::string();
   const ByteArray b = File::ReadFile( path );
   return std::string( reinterpret_cast<const char*>( b.Begin() ), b.Length() );
}

// The process umask, read race-free from /proc (never umask(0)/umask(m),
// which changes it for every thread for a moment). Test-only: the module
// itself never reads the umask; the kernel applies it at open()/mkdir().
mode_t JProcUmask()
{
   // /proc files report size 0, so File::ReadFile() reads nothing: read() to EOF.
   std::string s;
   const int fd = ::open( "/proc/self/status", O_RDONLY | O_CLOEXEC );
   if ( fd < 0 )
      throw Error( "JProcUmask: cannot open /proc/self/status" );
   char buf[ 4096 ];
   for ( ssize_t n; (n = ::read( fd, buf, sizeof( buf ) )) > 0; )
      s.append( buf, size_t( n ) );
   ::close( fd );
   const size_t at = s.find( "\nUmask:" );
   if ( at == std::string::npos )
      throw Error( "JProcUmask: no Umask line in /proc/self/status" );
   return mode_t( std::strtoul( s.c_str() + at + 7, nullptr, 8 ) );
}

// Permission bits of a path (lstat: a link's own mode, never its target's); -1 if absent.
int JModeOf( const String& path )
{
   struct stat st;
   return ::lstat( U8( path ).c_str(), &st ) == 0 ? int( st.st_mode & 07777 ) : -1;
}

bool JIsLink( const String& path )
{
   struct stat st;
   return ::lstat( U8( path ).c_str(), &st ) == 0 && S_ISLNK( st.st_mode );
}

// Local, stride-aware copy of the preview's block average (Task 1 only: it
// measures the cost Ruling 7 is about before StepStats exists). It stays after
// Task 4 adds BlockAveragedCopy(): J0 must keep measuring the same loop the
// constants were derived from (pre-flight P24).
double TimeBlockAverage( View v, int stride )
{
   AutoViewWriteLock lock( v );
   ImageVariant src = v.Image();
   const Image& img = static_cast<const Image&>( *src );
   const int w = img.Width(), h = img.Height(), n = img.NumberOfNominalChannels();
   const int k = std::max( 1, (std::max( w, h ) + PICopilotPreviewBlockEdge - 1)/PICopilotPreviewBlockEdge );
   Image dst( w/k, h/k, n == 3 ? ColorSpace::RGB : ColorSpace::Gray );
   const jclock::time_point t0 = jclock::now();
   for ( int c = 0; c < n; ++c )
   {
      const float* s = img.PixelData( c );
      float* d = dst.PixelData( c );
      for ( int y = 0; y < h/k; ++y )
         for ( int x = 0; x < w/k; ++x )
         {
            double sum = 0;
            int count = 0;
            for ( int j = 0; j < k; j += stride )
            {
               const float* row = s + size_type( y*k + j )*w + size_type( x )*k;
               for ( int i = 0; i < k; ++i )
                  sum += row[i];
               count += k;
            }
            *d++ = float( sum/count );
         }
   }
   return MsSince( t0 );
}

// ---- Self-test phases (multi-phase harness) ----
// Results of the top-level check phases, in call order per phase id, plus
// "errors". Read by the sections of the final (main) run.
nlohmann::json& SelfTestPhaseStore()
{
   static nlohmann::json store = nlohmann::json::object();
   return store;
}

// j0.mc: ModifyCount of each payload.ids window, labelled payload.label.
nlohmann::json PhaseModifyCounts( const nlohmann::json& payload )
{
   nlohmann::json counts = nlohmann::json::object();
   for ( const nlohmann::json& id : payload.at( "ids" ) )
   {
      const ImageWindow w = ImageWindow::WindowById( IsoString( id.get<std::string>().c_str() ) );
      counts[id.get<std::string>()] = w.IsNull() ? nlohmann::json() : nlohmann::json( uint64_t( w.ModifyCount() ) );
   }
   return { { "label", payload.at( "label" ) }, { "counts", counts } };
}

// The payload is itself the measurement (made by the top-level script).
nlohmann::json PhaseRecordPayload( const nlohmann::json& payload )
{
   return payload;
}

// probe.nestedEval: {on} -- pauses/resumes the spike probe's nested EvaluateScript.
nlohmann::json PhaseProbeNestedEval( const nlohmann::json& payload )
{
   JourneySpikeProbeSetNestedEval( payload.at( "on" ).get<bool>() );
   return payload;
}

// j0.timerApply.arm: {id, goFile} -- stages the request; the probe's first tick
// after goFile exists applies PixelMath to id.
nlohmann::json PhaseTimerApplyArm( const nlohmann::json& payload )
{
   JourneySpikeProbeRequestTimerApply( payload.at( "id" ).get<std::string>(), payload.at( "goFile" ).get<std::string>() );
   return payload;
}

// j0.timerApply.check: the top-level history read (payload) + the tick's result.
nlohmann::json PhaseTimerApplyCheck( const nlohmann::json& payload )
{
   return { { "history", payload }, { "tick", JourneySpikeProbeTimerApplyResult() } };
}

// ---- Section JH (Task T-hist): an applied process lands in History, or
// fails loudly -- the forced-10 ms repro, phases hist.* ----

// hist.timer: {intervalS} -- the spike probe timer's period (10 ms for the repro).
nlohmann::json PhaseHistTimer( const nlohmann::json& payload )
{
   JourneySpikeProbeSetTickInterval( payload.at( "intervalS" ).get<double>() );
   return payload;
}

// hist.arm: {id, delayS, gated} -- stages the hazard apply (JourneySpikeProbe.h).
// Returning from here is the moment PixInsight is still finishing this
// executeGlobal(): the first 10 ms tick lands inside it.
nlohmann::json PhaseHistArm( const nlohmann::json& payload )
{
   JourneySpikeProbeRequestHazardApply( payload.at( "id" ).get<std::string>(), payload.at( "delayS" ).get<double>(),
                                        payload.at( "gated" ).get<bool>(),
                                        payload.value( "spec", nlohmann::json::object() ) );
   return payload;
}

// hist.check: the top-level measurements (payload) + the tick's result.
nlohmann::json PhaseHistCheck( const nlohmann::json& payload )
{
   return { { "top", payload }, { "tick", JourneySpikeProbeHazardApplyResult() } };
}

// ---- Section J2 (HistoryReader, Task 3) fixtures and phases ----

// Verbatim XPSM (PI 1.9.5, plan API facts) used by the pure parser tests.
const char* const kXpsmPixelMath =
   "<instance class=\"PixelMath\" version=\"256\" id=\"PixelMath_instance\">\n"
   "<time start=\"2026-09-25T20:47:50.344Z\" span=\"0.006297325\"/>\n"
   "<parameter id=\"expression\">$T*2</parameter>\n<parameter id=\"expression1\"></parameter>\n"
   "<parameter id=\"expression2\"></parameter>\n<parameter id=\"expression3\"></parameter>\n"
   "<parameter id=\"useSingleExpression\" value=\"true\"/>\n<parameter id=\"symbols\"></parameter>\n"
   "<parameter id=\"clearImageCacheAndExit\" value=\"false\"/>\n<parameter id=\"cacheGeneratedImages\" value=\"false\"/>\n"
   "<parameter id=\"generateOutput\" value=\"true\"/>\n<parameter id=\"singleThreaded\" value=\"false\"/>\n"
   "<parameter id=\"optimization\" value=\"true\"/>\n<parameter id=\"use64BitWorkingImage\" value=\"false\"/>\n"
   "<parameter id=\"rescale\" value=\"false\"/>\n<parameter id=\"rescaleLower\" value=\"0\"/>\n"
   "<parameter id=\"rescaleUpper\" value=\"1\"/>\n<parameter id=\"truncate\" value=\"true\"/>\n"
   "<parameter id=\"truncateLower\" value=\"0\"/>\n<parameter id=\"truncateUpper\" value=\"1\"/>\n"
   "<parameter id=\"createNewImage\" value=\"false\"/>\n<parameter id=\"showNewImage\" value=\"true\"/>\n"
   "<parameter id=\"newImageId\"></parameter>\n<parameter id=\"newImageWidth\" value=\"0\"/>\n"
   "<parameter id=\"newImageHeight\" value=\"0\"/>\n<parameter id=\"newImageAlpha\" value=\"false\"/>\n"
   "<parameter id=\"newImageColorSpace\" value=\"SameAsTarget\"/>\n<parameter id=\"newImageSampleFormat\" value=\"SameAsTarget\"/>\n"
   "<table id=\"outputData\" rows=\"0\"/>\n</instance>";

std::string HtXpsm( const char* rootAttr )
{
   std::string rows;
   for ( int r = 0; r < 5; ++r )
      rows += std::string( "<tr>\n<td id=\"c0\" value=\"0.00000000\"/>\n<td id=\"m\" value=\"" ) + (r == 3 ? "0.25000000" : "0.50000000")
            + "\"/>\n<td id=\"c1\" value=\"1.00000000\"/>\n<td id=\"r0\" value=\"0.00000000\"/>\n<td id=\"r1\" value=\"1.00000000\"/>\n</tr>\n";
   return std::string( "<instance class=\"HistogramTransformation\" version=\"256\" " ) + rootAttr + ">\n"
        + "<time start=\"2026-09-25T20:47:50.353Z\" span=\"0.00254753\"/>\n<table id=\"H\" rows=\"5\">\n" + rows + "</table>\n</instance>";
}

std::vector<KnownStep> KnownFrom( const std::vector<HistoryStep>& steps, int64 firstId, int active )
{
   std::vector<KnownStep> k;
   for ( size_t i = 0; i < steps.size(); ++i )
      k.push_back( { firstId + int64( i ), steps[i].combinedIndex + 1, steps[i].identity,
                     steps[i].combinedIndex + 1 <= active ? "active" : "undone" } );
   return k;
}

nlohmann::json SnapJson( const HistorySnapshot& s )
{
   return { { "ok", s.ok }, { "busy", s.busy }, { "error", U8( s.error ) }, { "init", s.initialLength }, { "len", s.length },
            { "hi", s.historyIndex }, { "from", s.from }, { "n", s.steps.size() } };
}

// State the j2.hr phases carry between top-level steps (the history they read
// is made by test/selftest.js at top level; see the harness notes). Section J2
// reads the verdicts.
struct J2State
{
   std::vector<KnownStep> known;       // pcHrA after the three top-level steps (ids from 100)
   int                    tot = 0;     // its TotalCount() then
   HistorySnapshot        maskSnap;    // pcHrA after the masked step (before the rename)
   HistorySnapshot        renamed;     // pcHrRenamed: the full read after the rename
   bool liveReadOk = false, undoRedoOk = false, branchOk = false, maskOk = false, renameOk = false, reopenOk = false,
        utf8Ok = false;
   std::vector<std::string> steps;     // phase steps seen, in order
   std::string            reopenedId;  // the reopened window (closed by Section J2)
   nlohmann::json         detail = nlohmann::json::object();
};

J2State& J2()
{
   static J2State s;
   return s;
}

// j2.hr: {step, id?} -- one read of the pcHrA fixture between top-level steps.
nlohmann::json PhaseHistoryReader( const nlohmann::json& payload )
{
   J2State& st = J2();
   const std::string step = payload.at( "step" ).get<std::string>();
   st.steps.push_back( step );
   nlohmann::json& d = st.detail;
   if ( step == "live0" )
   {
      // (e) Three steps done by hand at top level, read in full. The window was
      //     made by a script, so initialProcessing holds its Script creation entry.
      const HistorySnapshot s0 = ReadViewHistory( "pcHrA", 0 );
      d["live0"] = SnapJson( s0 );
      st.liveReadOk = s0.ok && s0.length == 3 && s0.historyIndex == 3 && s0.steps.size() == size_t( s0.TotalCount() )
                   && s0.steps.back().parameters.at( "expression" ) == "$T*2" && s0.steps.back().combinedIndex == s0.TotalCount() - 1;
      const HistoryDiff d0 = DiffHistory( {}, s0 );
      st.liveReadOk = st.liveReadOk && !d0.needFullRead && d0.appended.size() == s0.steps.size()
                   && std::all_of( d0.appendedState.begin(), d0.appendedState.end(), []( const std::string& x ) { return x == "active"; } )
                   && HistoryReadFrom( {} ) == 0;
      st.known = KnownFrom( s0.steps, 100, s0.ActiveCount() );
      st.tot = s0.TotalCount();
   }
   else if ( step == "undo" )
   {
      // (f1) Two steps undone at top level: states only, no new rows.
      const HistorySnapshot s1 = ReadViewHistory( "pcHrA", HistoryReadFrom( st.known ) );
      const HistoryDiff d1 = DiffHistory( st.known, s1 );
      d["undo"] = { { "snap", SnapJson( s1 ) }, { "from", HistoryReadFrom( st.known ) }, { "undone", d1.toUndone } };
      st.undoRedoOk = s1.ok && !d1.needFullRead && d1.appended.empty() && d1.toSuperseded.empty() && d1.toActive.empty()
                   && d1.toUndone == std::vector<int64>( { 100 + st.tot - 2, 100 + st.tot - 1 } );
   }
   else if ( step == "redo" )
   {
      // (f2) Redone at top level.
      std::vector<KnownStep> knownUndone = st.known;
      for ( KnownStep& k : knownUndone )
         if ( k.seq > st.tot - 2 )
            k.state = "undone";
      const HistorySnapshot s2 = ReadViewHistory( "pcHrA", HistoryReadFrom( knownUndone ) );
      const HistoryDiff d2 = DiffHistory( knownUndone, s2 );
      d["redo"] = { { "snap", SnapJson( s2 ) }, { "active", d2.toActive } };
      st.undoRedoOk = st.undoRedoOk && s2.ok && !d2.needFullRead && d2.appended.empty() && d2.toSuperseded.empty()
                   && d2.toActive == std::vector<int64>( { 100 + st.tot - 2, 100 + st.tot - 1 } ) && d2.toUndone.empty();
   }
   else if ( step == "branch" )
   {
      // (g) Undo two, then TWO new steps before the next read: the tail check
      //     fails -> full read -> superseded + 2 appended.
      const HistorySnapshot s3 = ReadViewHistory( "pcHrA", HistoryReadFrom( st.known ) );
      const HistoryDiff d3 = DiffHistory( st.known, s3 );
      const HistorySnapshot s4 = ReadViewHistory( "pcHrA", 0 );
      const HistoryDiff d4 = DiffHistory( st.known, s4 );
      d["branch"] = { { "tailNeedsFull", d3.needFullRead }, { "superseded", d4.toSuperseded }, { "appended", d4.appended.size() },
                      { "snap", SnapJson( s4 ) } };
      st.branchOk = s3.ok && s4.ok && d3.needFullRead && !d4.needFullRead
                 && d4.toSuperseded == std::vector<int64>( { 100 + st.tot - 2, 100 + st.tot - 1 } )
                 && d4.toActive.empty() && d4.toUndone.empty()
                 && d4.appended.size() == 2 && d4.appended[0].combinedIndex == st.tot - 2
                 && d4.appended[0].parameters.at( "expression" ) == "$T-0.05"
                 && d4.appended[1].parameters.at( "expression" ) == "$T*0.9"
                 && d4.appendedState == std::vector<std::string>( { "active", "active" } );
   }
   else if ( step == "mask" )
   {
      // (h) A step applied through an inverted mask carries the mask id.
      st.maskSnap = ReadViewHistory( "pcHrA", 0 );
      const HistorySnapshot& s5 = st.maskSnap;
      d["mask"] = { { "snap", SnapJson( s5 ) }, { "id", s5.ok && !s5.steps.empty() ? s5.steps.back().maskId : std::string() },
                    { "inverted", s5.ok && !s5.steps.empty() && s5.steps.back().maskInverted },
                    { "processId", s5.ok && !s5.steps.empty() ? s5.steps.back().processId : std::string() } };
      st.maskOk = s5.ok && !s5.steps.empty() && s5.steps.back().processId == "PixelMath"
               && s5.steps.back().maskId == "pcHrMask" && s5.steps.back().maskInverted
               && s5.steps.front().maskId.empty();
   }
   else if ( step == "rename" )
   {
      // (h2) A top-level `view.id = ...` rename is itself a history step
      //      (ImageIdentifier, measured in Task 1): it is read and appended like
      //      any other step, and no earlier identity depends on the view id.
      const std::vector<KnownStep> k = KnownFrom( st.maskSnap.steps, 200, st.maskSnap.ActiveCount() );
      const HistorySnapshot old = ReadViewHistory( "pcHrA", 0 );
      const HistorySnapshot tail = ReadViewHistory( "pcHrRenamed", HistoryReadFrom( k ) );
      const HistoryDiff dt = DiffHistory( k, tail );
      st.renamed = ReadViewHistory( "pcHrRenamed", 0 );
      const HistoryDiff df = DiffHistory( k, st.renamed );
      const HistorySnapshot& r = st.renamed;
      bool sameEarlier = r.ok && r.TotalCount() == st.maskSnap.TotalCount() + 1;
      for ( int i = 0; sameEarlier && i < st.maskSnap.TotalCount(); ++i )
         sameEarlier = r.steps[i].identity == st.maskSnap.steps[i].identity;
      const HistoryStep* last = (r.ok && !r.steps.empty()) ? &r.steps.back() : nullptr;
      d["rename"] = { { "oldIdGone", !old.ok ? U8( old.error ) : std::string( "still readable" ) },
                      { "tail", SnapJson( tail ) }, { "tailAppended", dt.appended.size() }, { "tailNeedsFull", dt.needFullRead },
                      { "full", SnapJson( r ) }, { "sameEarlier", sameEarlier },
                      { "lastProcessId", last ? last->processId : std::string() },
                      { "lastParameters", last ? last->parameters : nlohmann::json() },
                      { "lastReplayable", last != nullptr && last->replayable }, { "lastNote", last ? last->parseNote : std::string() } };
      st.renameOk = !old.ok && old.error.Contains( "no view" )
                 && tail.ok && !dt.needFullRead && dt.toSuperseded.empty() && dt.toUndone.empty() && dt.toActive.empty()
                 && dt.appended.size() == 1 && dt.appendedState == std::vector<std::string>( { "active" } )
                 && df.appended.size() == 1 && df.toSuperseded.empty()
                 && sameEarlier && last != nullptr && last->processId == "ImageIdentifier"
                 && last->combinedIndex == st.maskSnap.TotalCount() && dt.appended[0].identity == last->identity;
   }
   else if ( step == "utf8" )
   {
      // (i2) Review fix: a parameter holding non-ASCII text (Latin-1, BMP and
      //      astral), XML specials and a newline, set at top level, comes back
      //      from HistoryReader byte for byte as the core RECORDED it (the
      //      top-level read of the step's own XPSM, sent as UTF-16 code units).
      //      Measured: the core records the astral U+1F4F7 as U+1F4FD, so the
      //      sent text is only compared up to that character (coreAstralIntact).
      auto fromCodes = []( const nlohmann::json& a )
      {
         String t;
         for ( const nlohmann::json& c : a )
            t += String::char_type( c.get<int>() );
         return U8( t );
      };
      const std::string sent = fromCodes( payload.at( "sentCodes" ) );
      const std::string recorded = fromCodes( payload.at( "recordedCodes" ) );
      const std::string want = "iif($T<0.5 && 1,\n$T,0) /* \xC3\xA9\xE2\x80\x94";   // up to the astral character
      const HistorySnapshot u = ReadViewHistory( "pcHrUtf", 0 );
      const std::string got = u.ok && !u.steps.empty() && u.steps.back().parameters.contains( "expression" )
                            ? u.steps.back().parameters.at( "expression" ).get<std::string>() : std::string();
      d["utf8"] = { { "snap", SnapJson( u ) }, { "got", got }, { "recorded", recorded }, { "sent", sent },
                    { "coreAstralIntact", recorded == sent } };
      st.utf8Ok = u.ok && !recorded.empty() && got == recorded
               && sent.rfind( want, 0 ) == 0 && got.rfind( want, 0 ) == 0
               && got.size() >= 3 && got.compare( got.size() - 3, 3, " */" ) == 0
               && u.steps.back().processId == "PixelMath" && u.steps.back().replayable;
   }
   else if ( step == "reopen" )
   {
      // (i) Save + reopen: the history moves into initialProcessing with the
      //     SAME identities (the rename step included).
      const std::string id = payload.at( "id" ).get<std::string>();
      st.reopenedId = id;
      const HistorySnapshot r = ReadViewHistory( IsoString( id.c_str() ), 0 );
      const HistorySnapshot& before = st.renamed;
      bool same = r.ok && before.ok && r.length == 0 && r.historyIndex == 0 && r.initialLength == before.ActiveCount()
               && r.steps.size() == size_t( r.initialLength )
               && r.droppedReopenExtra == (PICopilotJourneyReopenExtraSteps == 1);
      for ( int i = 0; same && i < r.initialLength; ++i )
         same = r.steps[i].identity == before.steps[i].identity;
      d["reopen"] = { { "id", id }, { "snap", SnapJson( r ) }, { "expected", before.ActiveCount() },
                      { "dropped", r.droppedReopenExtra }, { "same", same } };
      st.reopenOk = same;
   }
   else
      throw Error( String( "j2.hr: unknown step " ) + step.c_str() );
   return { { "step", step } };
}

using SelfTestPhaseHandler = nlohmann::json (*)( const nlohmann::json& payload );

nlohmann::json PhaseJourneyTracker( const nlohmann::json& payload );   // Section J6 (defined below)

nlohmann::json PhaseJourneyExport( const nlohmann::json& payload );   // j7.exp (Section J7's fixture, below)

// gui.panel -- test/gui-smoke.sh only: with payload.moveTo [x, y, w, h],
// sizes, moves and SHOWS the panel (launchInterface() from a script leaves it
// hidden); its first show then applies the default side placement, so the
// geometry REPORTED (payload.out) is where the driver clicks.
nlohmann::json PhaseGuiPanel( const nlohmann::json& payload )
{
   if ( ThePICopilotInterface == nullptr )
      throw Error( "gui.panel: ThePICopilotInterface is null" );
   PICopilotInterface& ui = *ThePICopilotInterface;
   if ( payload.contains( "moveTo" ) )
   {
      const nlohmann::json& m = payload.at( "moveTo" );
      ui.Resize( m.at( 2 ).get<int>(), m.at( 3 ).get<int>() );
      ui.Move( m.at( 0 ).get<int>(), m.at( 1 ).get<int>() );
      ui.Show();   // launchInterface() from a script builds the panel but PixInsight leaves it hidden (measured)
      ui.BringToFront();
   }
   const pcl::Point p = ui.Position();
   const nlohmann::json r = { { "x", p.x }, { "y", p.y }, { "w", ui.Width() }, { "h", ui.Height() },
                              { "visible", ui.IsVisible() } };
   if ( payload.contains( "out" ) )
      File::WriteTextFile( String( payload.at( "out" ).get<std::string>().c_str() ), IsoString( r.dump().c_str() ) );
   return r;
}

// gui.keepInFlight -- test/gui-smoke.sh only (a GUI run, not the headless
// self-test): keeps the production service's journey of payload.viewId with
// its write-up pointed at payload.url (a loopback that never answers), so the
// PixInsight exit that follows happens with a keeper write-up in flight
// (JourneyService::Stop() must cancel it: no crash, no hang).
nlohmann::json PhaseGuiKeepInFlight( const nlohmann::json& payload )
{
   JourneyService& svc = JourneyService::Instance();
   if ( !svc.Started() || svc.Store() == nullptr )
      throw Error( "the production journey service has no store: " + svc.StoreError() );
   const IsoString vid( payload.at( "viewId" ).get<std::string>().c_str() );
   const int64 jid = svc.Tracker().JourneyOfView( vid );
   const int64 img = svc.Tracker().ImageOfView( vid );
   if ( jid == 0 || img == 0 )
      throw Error( "gui.keepInFlight: the production tracker records no journey for " + String( vid ) );
   const KeepOutcome o = svc.Keeper().Keep( jid, img, "sk-test-loopback", String(),
                                            String( payload.at( "url" ).get<std::string>().c_str() ) );
   const nlohmann::json r = { { "journeyId", jid }, { "marked", o.marked }, { "writeupStarted", o.writeupStarted },
                              { "writeupError", U8( o.writeupError ) }, { "busy", svc.Keeper().Busy() } };
   if ( payload.contains( "out" ) )   // the GUI run has no self-test verdict to carry it
      File::WriteTextFile( String( payload.at( "out" ).get<std::string>().c_str() ), IsoString( r.dump().c_str() ) );
   return r;
}

// Adding a phase: one entry here + one checkPhase( id, payload ) call in
// test/selftest.js. Handlers run in-process: they may READ history, never
// create it.
const std::map<std::string, SelfTestPhaseHandler>& SelfTestPhaseHandlers()
{
   static const std::map<std::string, SelfTestPhaseHandler> handlers = {
      { "probe.nestedEval",    PhaseProbeNestedEval },
      { "j0.timerApply.arm",   PhaseTimerApplyArm },
      { "j0.timerApply.check", PhaseTimerApplyCheck },
      { "j0.mc",       PhaseModifyCounts },
      { "j0.identity", PhaseRecordPayload },
      { "j0.reopen",   PhaseRecordPayload },
      { "j2.hr",       PhaseHistoryReader },
      { "hist.timer",  PhaseHistTimer },
      { "hist.arm",    PhaseHistArm },
      { "hist.check",  PhaseHistCheck },
      { "hist.kind.check", PhaseHistCheck },
      { "hist.preview.check", PhaseHistCheck },
      { "j6",          PhaseJourneyTracker },
      { "j7.exp",      PhaseJourneyExport },
      { "gui.keepInFlight", PhaseGuiKeepInFlight },
      { "gui.panel", PhaseGuiPanel },
   };
   return handlers;
}

// ---- Section J5 (MasterFacts, Task 6) helpers ----

FITSKeywordArray Kw( std::initializer_list<std::pair<const char*, const char*>> nv )
{
   FITSKeywordArray k;
   for ( const auto& p : nv )
      if ( std::string( p.first ) == "HISTORY" || std::string( p.first ) == "COMMENT" )
         k << FITSHeaderKeyword( p.first, "", p.second );
      else
         k << FITSHeaderKeyword( p.first, p.second, "" );
   return k;
}

// The real WBPP master keyword set (plan API facts: M16, 2022-10-09), abridged
// to the keywords that matter plus the location keywords that must never leak.
FITSKeywordArray WbppMasterKeywords()
{
   return Kw( { { "COMMENT", "PixInsight image preprocessing pipeline" },
                { "COMMENT", "Master frame generated with Weighted Batch Preprocessing Script v2.5.3" },
                { "IMAGETYP", "'Master Light'" }, { "XBINNING", "1" }, { "FILTER", "'NoFilter'" }, { "EXPTIME", "300.00" },
                { "INSTRUME", "'ZWO ASI071MC Pro'" }, { "TELESCOP", "'EQMod Mount'" }, { "FOCALLEN", "853.61377" },
                { "DATE-OBS", "'2022-10-09T00:48:08.260'" }, { "SITELAT", "'+40 11 12'" }, { "SITELONG", "'-86 01 02'" },
                { "OBSERVER", "'Jane Observer'" },
                { "HISTORY", "Integration with ImageIntegration module version 1.5.0" },
                { "HISTORY", "ImageIntegration.pixelCombination: Average" },
                { "HISTORY", "ImageIntegration.numberOfImages: 10" },
                { "HISTORY", "ImageIntegration.noise: 1.1693e-03" } } );
}

// ---- Section J6 (JourneyTracker, Task 7) helpers and phases ----
//
// Every step whose History the tracker must record is made at TOP LEVEL by
// test/selftest.js (a process executed inside PICopilot.executeGlobal() is
// never recorded in History -- harness fact, global constraints). Phase j6
// {step} drives this section's own tracker (a temp library, explicit ticks)
// between those top-level steps, and keeps every verdict in J6State; the main
// run's Section J6 reports them. Windows made here or at top level are closed
// by Section J6.

// Sets keywords on an open window (the "file" keywords a master carries).
void JSetKeywords( const char* id, const FITSKeywordArray& kw )
{
   ImageWindow w = ImageWindow::WindowById( IsoString( id ) );
   if ( w.IsNull() )
      throw Error( String( "JSetKeywords: no window " ) + id );
   w.SetKeywords( kw );
}

// Pumps until the tracker's busy gate (CurrentProcessActivity: console abort,
// a locked view, < 0.3 s since the last image notification) is idle, max 3 s;
// throws naming the reason otherwise -- a J6 tick must never be silently skipped.
void JWaitIdle()
{
   const jclock::time_point t0 = jclock::now();
   for ( ;; )
   {
      const ProcessActivityState a = CurrentProcessActivity();
      if ( !a.busy )
         return;
      if ( MsSince( t0 ) > 3000 )
         throw Error( "PixInsight stayed busy for 3 s (" + a.reason + "); the tracker cannot tick" );
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
   }
}

void JTick( JourneyTracker& t, int n = 1 )
{
   for ( int i = 0; i < n; ++i )
   {
      JWaitIdle();
      t.Tick( JourneyWallNow(), true/*forceScan*/ );
      JPump( 60 );
   }
}

int JourneyCount( JourneyStore& s )
{
   return int( s.ListJourneys( false, "", 100000 ).size() );
}

// Image rows (any journey) currently naming viewId.
int ImageRowsOf( JourneyStore& s, const std::string& viewId )
{
   int n = 0;
   for ( const JourneyRow& j : s.ListJourneys( false, "", 100000 ) )
      for ( const ImageRow& i : s.Images( j.id ) )
         if ( i.viewId == viewId )
            ++n;
   return n;
}

// True when f() throws a pcl::Error whose message contains `what`.
template <class F>
bool JThrowsWith( F f, const char* what, std::string& msg )
{
   try
   {
      f();
   }
   catch ( const pcl::Exception& x )
   {
      msg = U8( x.Message() );
      return msg.find( what ) != std::string::npos;
   }
   msg = "(no exception)";
   return false;
}

int ActiveSteps( JourneyStore& s, int64 image )
{
   int n = 0;
   for ( const StepRow& r : s.Steps( image, false ) )
      if ( r.state == "active" && !r.params.value( "base", false ) )
         ++n;
   return n;
}

View J6MainView( const char* id )
{
   const ImageWindow w = ImageWindow::WindowById( IsoString( id ) );
   if ( w.IsNull() )
      throw Error( String( "no window " ) + id );
   return w.MainView();
}

// The J6 checks, in report order.
const char* const kJ6Checks[] = { "settings", "master", "manual", "undo", "copilot", "timing", "rgbTiming", "reference",
                                  "copilotLink", "defer", "rename", "reopen", "keywordOnly", "locked", "gap", "off",
                                  "preview", "budget", "redact", "retention", "scanModes", "aux", "inherit", "ccGlobal",
                                  "offRootDtor", "late", "notifyFree", "gate", "noFalseTiming", "dupMaster", "joinAtomic",
                                  "startJourney", "perImagePause", "gapResolved", "retentionOnce", "resumeTouch",
                                  "transaction", "mutatorsLoud", "retentionOpen", "rowGone", "offThenOn", "gateStallLock",
                                  "gateStallWaiting", "gateStallGap", "txDefer", "noopNoLock", "owner", "gapKinds",
                                  "pjsrAbortRestored", "gapRowGone", "linkRowGone", "ignoredThenMaster", "chaos",
                                  "recentRowGone", "ownerPrune", "writeCap", "deathOverflow", "deathWhileOff",
                                  "createdAfterDeath", "startAfterDeath" };

struct J6State
{
   // Declaration order = reverse destruction order: tracker, store, then the directories.
   std::unique_ptr<JTempDir>       root;
   std::unique_ptr<JTempDir>       frames;
   std::unique_ptr<JourneyStore>   store;
   std::unique_ptr<JourneyTracker> trk;
   int64                    jid = 0, mimg = 0, rgbJ = 0;
   std::vector<std::string> made;
   std::vector<std::string> steps;           // phase steps seen, in order
   StringList               errors;
   nlohmann::json           d = nlohmann::json::object();
   std::map<std::string, bool> ok;
   int  afterUndo = -1, afterRedo = -1, reopenActiveBefore = -1, offBefore = -1, scanBefore = -1, gapCalls = 0;
   std::string reopenClosedStatus, reopenClosedUpdated;
   int64 reopenAgain = 0;
   int  reopenResumed = -1;
   bool scanModes = true;
   bool serviceFlushed = false;
   // Fix round 3 fault injection (chaos*).
   std::vector<std::string> chaosViews;
   std::vector<int64>       chaosJourneys;
   int  chaosBefore = 0, chaosNew = 0, chaosTickFailures = 0;
   std::string chaosFirstFailure;
   std::set<std::string> gapLogSeen;
   int  writeCapBefore = 0, writeCapReads = 0;
   bool writeCapPaused = false;
   std::map<std::string, int> chaosCount;   // m-g coverage
};

J6State& J6()
{
   static J6State s;
   return s;
}

JourneyTracker& J6Tracker( J6State& st )
{
   if ( !st.trk || !st.store )
      throw Error( "no tracker (step begin did not complete)" );
   return *st.trk;
}

// Closes a J6 window and gives J6's tracker the ImageDeleted notification first (the in-process close may
// send none): a still-open view, read before the close (re-review I-1r (5)).
void J6Close( J6State& st, const std::string& id )
{
   if ( st.trk )
      try
      {
         const ImageWindow w = ImageWindow::WindowById( IsoString( id.c_str() ) );
         if ( !w.IsNull() )
            st.trk->OnImageDeleted( w.MainView(), JourneyWallNow() );
      }
      catch ( ... )
      {
      }
   JForceClose( id );
}

void J6Step( J6State& st, const std::string& step, const nlohmann::json& payload )
{
   if ( step == "begin" )
   {
      // A tracker of our own on a temp library, driven by explicit ticks.
      st.root.reset( new JTempDir( "picopilot-trk-" ) );
      String oe;
      st.store = JourneyStore::Open( st.root->Path(), oe );
      if ( !st.store )
         throw Error( "store: " + oe );
      st.trk.reset( new JourneyTracker( st.store.get() ) );
      // Re-review I-1r (5): J6's tracker gets the real notifications (the interface's handlers, through the
      // paused production service); in-process closes / renames are also fed explicitly (J6Close, renames).
      JourneyService::Instance().SetNotificationForwardForSelfTest( st.trk.get() );
      JourneyTracker& trk = *st.trk;
      JourneyStore& store = *st.store;

      // (b) A master from ImageIntegration over synthetic frames, keywords set on the result. Rejection maps
      //     are generated on purpose: they carry an integration-first history too and must NOT become
      //     masters (Ruling 29, pre-flight P35). The result is renamed only after the tracker has seen it
      //     under its own id (a rename is followed through the scan, check (k)). The run is in-process:
      //     a created window's creating step is recorded in its initialProcessing (Task 1 measured
      //     initialIds [ImageIntegration] this way); only steps on an EXISTING view need top level.
      st.frames.reset( new JTempDir( "picopilot-trk-frames-" ) );
      nlohmann::json rows = nlohmann::json::array();
      for ( int i = 0; i < 3; ++i )
      {
         Image img( 64, 64, ColorSpace::Gray );
         JFillNoise( img, 0.1, 0.01, unsigned( 20 + i ) );
         const String p = st.frames->Path() + String().Format( "/l%d.fits", i );
         JWriteFits( p, img, FITSKeywordArray() );
         rows.push_back( { true, U8( p ), "", "" } );
      }
      const GlobalRunResult g = RunGlobalProcess( "ImageIntegration",
                                                  { { "weightMode", "DontCare" }, { "generateRejectionMaps", true } },
                                                  { { "images", rows } } );
      if ( !g.ok || g.createdWindows.empty() )
         throw Error( "ImageIntegration: " + g.error );
      for ( const std::string& id : g.createdWindows )
         st.made.push_back( id );
      const std::string mid = g.createdWindows.front();
      {
         ImageWindow mw = ImageWindow::WindowById( IsoString( mid.c_str() ) );
         FITSKeywordArray kw = mw.Keywords();
         kw << FITSHeaderKeyword( "IMAGETYP", "'Master Light'", "" ) << FITSHeaderKeyword( "OBJECT", "'TrkM31'", "" )
            << FITSHeaderKeyword( "FILTER", "'Ha'", "" ) << FITSHeaderKeyword( "EXPTIME", "300", "" )
            << FITSHeaderKeyword( "SITELAT", "'+40 11 12'", "" );
         mw.SetKeywords( kw );
      }
      // The first scan finds every window earlier fixtures left open (some with 500-step histories); the
      // per-tick cap (review M8) spreads their evaluation over several ticks.
      JTick( trk, 8 );
      // Ruling 29: the auxiliary outputs (every created window but the first) stay untracked when Task 1
      // measured that their history names the result's id; otherwise they are masters of their own (the
      // documented limit), and this check only requires that they exist.
      {
         nlohmann::json aux = nlohmann::json::array();
         bool untracked = true;
         for ( size_t i = 1; i < g.createdWindows.size(); ++i )
         {
            aux.push_back( g.createdWindows[i] );
            untracked = untracked && trk.ImageOfView( IsoString( g.createdWindows[i].c_str() ) ) == 0;
         }
         st.d["aux"] = { { "ids", aux }, { "idInHistory", PICopilotJourneyIntegrationIdInHistory } };
         st.ok["aux"] = g.createdWindows.size() > 1 && (!PICopilotJourneyIntegrationIdInHistory || untracked)
                     && trk.ImageOfView( IsoString( mid.c_str() ) ) != 0;
      }
      ImageWindow::WindowById( IsoString( mid.c_str() ) ).MainView().Rename( "pcTrkMaster" );
      trk.OnImageRenamed( J6MainView( "pcTrkMaster" ), JourneyWallNow() );   // the notification a rename sends
      st.made.push_back( "pcTrkMaster" );
      JTick( trk );
      st.jid = trk.JourneyOfView( "pcTrkMaster" );
      st.mimg = trk.ImageOfView( "pcTrkMaster" );
      JourneyRow jr;
      store.GetJourney( st.jid, jr );
      AcquisitionFacts acq;
      store.Acquisition( st.mimg, acq );
      st.d["master"] = { { "journey", st.jid }, { "name", jr.name }, { "subCount", acq.subCount.value_or( -1 ) },
                         { "why", trk.StatusFor( "pcTrkMaster" ).why } };
      st.ok["master"] = st.jid != 0 && jr.name.rfind( "TrkM31 Ha ", 0 ) == 0 && acq.filter == "Ha" && acq.subCount == 3
                     && store.Stats( st.mimg, 0 ).size() == 1
                     && File::Exists( store.JourneyDir( st.jid ) + String().Format( "/thumbs/start-%lld.jpg", (long long)st.mimg ) )
                     && ActiveSteps( store, st.mimg ) == 0 && trk.StatusFor( "pcTrkMaster" ).state == RecordingState::Recording;
      return;
   }

   if ( step == "service" )
   {
      // Right after the pre-phase: the production service has recorded it. Flush it and stop it for the
      // rest of the run, so it never interleaves with the timing-sensitive fixture phases and sections.
      JourneyService::Instance().FlushAndPauseForSelfTest();
      st.serviceFlushed = true;
      return;
   }

   JourneyTracker& trk = J6Tracker( st );
   JourneyStore& store = *st.store;
   const int64 jid = st.jid, mimg = st.mimg;

   if ( step == "manual" )
   {
      // (c) Two manual steps (top level): rows, stats, thumbnails.
      //     Review M2: recording them (history read, geometry, statistics locks) is no "process activity":
      //     the busy gate is idle right after the tick, so neither the tool loop nor the tracker waits.
      JWaitIdle();
      trk.Tick( JourneyWallNow(), true );
      const ProcessActivityState after = CurrentProcessActivity();
      st.d["notifyFree"] = { { "busy", after.busy }, { "reason", U8( after.reason ) } };
      st.ok["notifyFree"] = !after.busy;
      JPump( 60 );
      const std::vector<StepRow> s = store.Steps( mimg, false );
      const int64 lastId = s.empty() ? 0 : s.back().id;
      st.d["manual"] = { { "active", ActiveSteps( store, mimg ) }, { "actor", s.empty() ? "" : s.back().actor },
                         { "processId", s.empty() ? "" : s.back().processId } };
      st.ok["manual"] = ActiveSteps( store, mimg ) == 2 && !s.empty() && s.back().actor == "user" && s.back().processId == "PixelMath"
                     && store.Stats( mimg, lastId ).size() == 1
                     && File::Exists( store.JourneyDir( jid ) + String().Format( "/thumbs/%lld.jpg", (long long)lastId ) );
   }
   else if ( step == "undo" )
   {
      // (d) Undo -> undone; redo -> active; undo + two new steps between ticks -> superseded + 2 new.
      JTick( trk );
      st.afterUndo = ActiveSteps( store, mimg );
   }
   else if ( step == "redo" )
   {
      JTick( trk );
      st.afterRedo = ActiveSteps( store, mimg );
   }
   else if ( step == "branch" )
   {
      JTick( trk );
      int superseded = 0;
      for ( const StepRow& r : store.Steps( mimg, true ) )
         if ( r.state == "superseded" ) ++superseded;
      st.d["undo"] = { st.afterUndo, st.afterRedo, ActiveSteps( store, mimg ), superseded };
      st.ok["undo"] = st.afterUndo == 1 && st.afterRedo == 2 && ActiveSteps( store, mimg ) == 3 && superseded == 1;
   }
   else if ( step == "copilotNote" )
   {
      // (e) Copilot attribution (Ruling 21): the note, then the step (top level), then the tick.
      trk.NoteCopilotStep( "pcTrkMaster", "PixelMath", "lift the background", {}, false, JourneyWallNow() );
   }
   else if ( step == "copilot" )
   {
      JTick( trk );
      const std::vector<StepRow> s = store.Steps( mimg, false );
      st.d["copilot"] = { { "actor", s.empty() ? "" : s.back().actor }, { "reason", s.empty() ? "" : s.back().reason } };
      st.ok["copilot"] = !s.empty() && s.back().actor == "copilot" && s.back().reason == "lift the background";
   }
   else if ( step == "focus" )
   {
      // The focus notification the panel forwards, fed through the same entry point.
      const std::string id = payload.at( "id" ).get<std::string>();
      trk.OnImageFocused( J6MainView( id.c_str() ), JourneyWallNow() );
      JPump( 100 );
   }
   else if ( step == "timing" )
   {
      // (f) Timing (b): PixelMath createNewImage done by hand adds no step to its source; the source was the
      //     active view when the creating step started.
      st.made.push_back( "pcTrkClone" );
      JTick( trk, 3 );
      const int64 cloneImg = trk.ImageOfView( "pcTrkClone" );
      std::string cloneEvidence;
      for ( const LinkRow& l : store.Links( jid ) )
         if ( l.toImageId == cloneImg ) cloneEvidence = l.evidence;
      st.d["timing"] = { { "image", cloneImg }, { "evidence", cloneEvidence }, { "why", trk.StatusFor( "pcTrkClone" ).why } };
      st.ok["timing"] = cloneImg != 0 && cloneEvidence == "timing" && trk.JourneyOfView( "pcTrkClone" ) == jid;
   }
   else if ( step == "inherit" )
   {
      // (f2) Ruling 28 (pre-flight P14): a derived window carrying its source's master keywords is linked,
      //      not made a new master. The inherited keywords are forced, so the case is covered whatever
      //      PICopilotJourneyCreatedWindowsInheritKeywords measured.
      st.made.push_back( "pcTrkInherit" );
      JSetKeywords( "pcTrkInherit", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'TrkM31'" } } ) );
      JTick( trk, 3 );
      const int64 inhImg = trk.ImageOfView( "pcTrkInherit" );
      ImageRow inhRow;
      const bool found = inhImg != 0 && store.GetImage( inhImg, inhRow );
      std::string inhEvidence;
      for ( const LinkRow& l : store.Links( jid ) )
         if ( l.toImageId == inhImg ) inhEvidence = l.evidence;
      st.d["inherit"] = { { "image", inhImg }, { "evidence", inhEvidence }, { "why", trk.StatusFor( "pcTrkInherit" ).why },
                          { "measuredInherit", PICopilotJourneyCreatedWindowsInheritKeywords } };
      st.ok["inherit"] = found && !inhRow.isMaster && trk.JourneyOfView( "pcTrkInherit" ) == jid && inhEvidence == "timing";
   }
   else if ( step == "late" )
   {
      // (f3) A window first seen before its creating step is attached (a tick running while the creating
      //      process still executes -- the likely cause of the production pre-phase flake, where
      //      pcJourneyPreNew stayed untracked in 1 of 8 runs; unconfirmed). The brief's timing (c) saw two recent steps
      //      and REJECTED it for good. Forced here: the first read of pcTrkLate returns an empty history,
      //      with two steps recorded just before. The window must still join by timing (b) once its
      //      history is readable.
      st.made.push_back( "pcTrkLate" );
      int lateReads = 0;
      trk.SetHistoryReaderForSelfTest( [&lateReads]( const IsoString& id, int from )
      {
         if ( id == "pcTrkLate" && lateReads++ == 0 )
         {
            HistorySnapshot s;
            s.ok = true;   // readable, but nothing attached yet: initialLength 0, no steps
            return s;
         }
         return ReadViewHistory( id, from );
      } );
      const int before = ActiveSteps( store, mimg );
      JTick( trk );
      const int64 firstLook = trk.ImageOfView( "pcTrkLate" );
      trk.SetHistoryReaderForSelfTest( HistoryReadFn() );
      JTick( trk, 3 );
      const int64 lateImg = trk.ImageOfView( "pcTrkLate" );
      std::string lateEvidence;
      for ( const LinkRow& l : store.Links( jid ) )
         if ( l.toImageId == lateImg ) lateEvidence = l.evidence;
      std::vector<std::string> dec;
      for ( const std::string& x : trk.RecentDecisionsForSelfTest() )
         if ( x.find( "pcTrkLate" ) != std::string::npos )
            dec.push_back( x );
      st.d["late"] = { { "stepsRecorded", ActiveSteps( store, mimg ) - before }, { "fakeReads", lateReads },
                       { "firstLook", firstLook }, { "image", lateImg }, { "evidence", lateEvidence }, { "decisions", dec } };
      st.ok["late"] = ActiveSteps( store, mimg ) - before == 2 && lateReads >= 1 && firstLook == 0 && lateImg != 0
                   && trk.JourneyOfView( "pcTrkLate" ) == jid && lateEvidence == "timing";
   }
   else if ( step == "rgbJoin" )
   {
      // (g) An RGB master (made at top level); focused before ChannelExtraction runs.
      st.made.push_back( "pcTrkRgb" );
      JTick( trk, 2 );
      trk.OnImageFocused( J6MainView( "pcTrkRgb" ), JourneyWallNow() );
      JPump( 100 );
   }
   else if ( step == "rgb" )
   {
      // (g) Timing on an RGB master: ChannelExtraction makes three linked windows.
      for ( const char* c : { "pcTrkRgb_R", "pcTrkRgb_G", "pcTrkRgb_B" } )
         st.made.push_back( c );
      JTick( trk, 3 );
      st.rgbJ = trk.JourneyOfView( "pcTrkRgb" );
      const int64 rgbJ = st.rgbJ;
      st.d["rgbTiming"] = { { "journey", rgbJ }, { "links", store.Links( rgbJ ).size() },
                            { "R", trk.JourneyOfView( "pcTrkRgb_R" ) }, { "G", trk.JourneyOfView( "pcTrkRgb_G" ) },
                            { "B", trk.JourneyOfView( "pcTrkRgb_B" ) } };
      st.ok["rgbTiming"] = rgbJ != 0 && trk.JourneyOfView( "pcTrkRgb_R" ) == rgbJ && trk.JourneyOfView( "pcTrkRgb_G" ) == rgbJ
                        && trk.JourneyOfView( "pcTrkRgb_B" ) == rgbJ && store.Links( rgbJ ).size() == 3;
   }
   else if ( step == "reference" )
   {
      // (h) Reference: ChannelCombination into a fresh window naming the three channel views; PixelMath on a
      //     fresh window naming the master in its expression (both at top level).
      st.made.push_back( "pcTrkCC" );
      st.made.push_back( "pcTrkRef" );
      JTick( trk, 3 );
      int ccRefs = 0, pmRefs = 0;
      for ( const LinkRow& l : store.Links( st.rgbJ ) )
         if ( l.toImageId == trk.ImageOfView( "pcTrkCC" ) && l.evidence == "reference" ) ++ccRefs;
      for ( const LinkRow& l : store.Links( jid ) )
         if ( l.toImageId == trk.ImageOfView( "pcTrkRef" ) && l.evidence == "reference" ) ++pmRefs;
      st.d["reference"] = { ccRefs, pmRefs };
      st.ok["reference"] = ccRefs == 3 && pmRefs == 1;
   }
   else if ( step == "ccGlobal" )
   {
      // (h2) Pre-flight P13: a hand-run GLOBAL ChannelCombination makes a window of its own. Its creating
      //      step (initialProcessing[0], spec §5 evidence 3) names the channel views: linked by reference.
      const std::string gid = payload.at( "id" ).get<std::string>();
      if ( gid.empty() )
         throw Error( "global ChannelCombination created no window" );
      st.made.push_back( gid );
      JTick( trk, 3 );
      const int64 gImg = trk.ImageOfView( IsoString( gid.c_str() ) );
      int gRefs = 0;
      for ( const LinkRow& l : store.Links( st.rgbJ ) )
         if ( l.toImageId == gImg && l.evidence == "reference" ) ++gRefs;
      st.d["ccGlobal"] = { { "id", gid }, { "image", gImg }, { "refs", gRefs } };
      st.ok["ccGlobal"] = gImg != 0 && trk.JourneyOfView( IsoString( gid.c_str() ) ) == st.rgbJ && gRefs == 3;
   }
   else if ( step == "copilotLink" )
   {
      // (i) Copilot evidence: a window a Copilot tool reported as created (no history needed: made here).
      trk.NoteCopilotStep( "pcTrkMaster", "PixelMath", "star mask", { "pcTrkCop" }, false, JourneyWallNow() );
      JEvalJs( "(function(){ var w = new ImageWindow( 64, 64, 1, 32, true, false, \"pcTrkCop\" ); })()" );
      st.made.push_back( "pcTrkCop" );
      JTick( trk, 2 );
      std::string copEvidence;
      for ( const LinkRow& l : store.Links( jid ) )
         if ( l.toImageId == trk.ImageOfView( "pcTrkCop" ) ) copEvidence = l.evidence;
      st.d["copilotLink"] = copEvidence;
      st.ok["copilotLink"] = copEvidence == "copilot";
   }
   else if ( step == "defer" )
   {
      // (j) Busy view (a step made at top level, then the view locked here): deferred without waiting,
      //     recorded once free.
      const int before = ActiveSteps( store, mimg );
      View mv = J6MainView( "pcTrkMaster" );
      double ms = 0;
      int deferrals = 0;
      {
         AutoViewLock lock( mv );
         const jclock::time_point t0 = jclock::now();
         const int d0 = trk.Deferrals();
         trk.Tick( JourneyWallNow(), true );
         ms = MsSince( t0 );
         deferrals = trk.Deferrals() - d0;
      }
      const int during = ActiveSteps( store, mimg );
      JTick( trk );
      st.d["defer"] = { before, during, ActiveSteps( store, mimg ), ms, deferrals };
      st.ok["defer"] = during == before && deferrals >= 1 && ms < 100 && ActiveSteps( store, mimg ) == before + 1;
   }
   else if ( step == "rename" )
   {
      // (k) Rename: same image row, new id.
      J6MainView( "pcTrkMaster" ).Rename( "pcTrkRenamed" );
      trk.OnImageRenamed( J6MainView( "pcTrkRenamed" ), JourneyWallNow() );
      st.made.push_back( "pcTrkRenamed" );
      JTick( trk );
      ImageRow ir;
      store.GetImage( mimg, ir );
      st.d["rename"] = ir.viewId;
      st.ok["rename"] = ir.viewId == "pcTrkRenamed" && trk.ImageOfView( "pcTrkRenamed" ) == mimg;
   }
   else if ( step == "reopenClose" )
   {
      // (l) Saved (top level), then closed: the journey only ends when NO image of it is open (Ruling 24),
      //     so its other open images are closed first (pre-flight P1).
      st.reopenActiveBefore = ActiveSteps( store, mimg );
      // (closed at top level, from JS, just before this phase)
      JTick( trk );
      JourneyRow closed;
      store.GetJourney( jid, closed );
      st.reopenClosedStatus = closed.status;
      st.reopenClosedUpdated = closed.updated;
      JPump( 20 );   // NowIso() has ms resolution: the resume's touch must be a later time
   }
   else if ( step == "reopened" )
   {
      // Reopened at top level: the same journey resumed (Ruling 27: ReadViewHistory already drops any
      // extra entry a reopen adds, so the resumed identities match), no duplicate steps.
      JTick( trk, 2 );
      st.reopenAgain = trk.ImageOfView( "pcTrkRenamed" );
      st.reopenResumed = ActiveSteps( store, mimg );
      // Review I2: resuming touches the journey (retention must not prune it as stale while it is open).
      JourneyRow resumed;
      store.GetJourney( jid, resumed );
      st.d["resumeTouch"] = { st.reopenClosedUpdated, resumed.updated };
      st.ok["resumeTouch"] = st.reopenResumed == st.reopenActiveBefore && resumed.updated > st.reopenClosedUpdated;
   }
   else if ( step == "reopenStep" )
   {
      // ... and the next step (top level) appended.
      JTick( trk );
      JourneyRow reopened;
      store.GetJourney( jid, reopened );
      st.d["reopen"] = { { "closedStatus", st.reopenClosedStatus }, { "again", st.reopenAgain },
                         { "before", st.reopenActiveBefore }, { "resumed", st.reopenResumed },
                         { "after", ActiveSteps( store, mimg ) }, { "status", reopened.status } };
      st.ok["reopen"] = st.reopenClosedStatus == "ended" && st.reopenAgain == mimg && st.reopenResumed == st.reopenActiveBefore
                     && ActiveSteps( store, mimg ) == st.reopenActiveBefore + 1 && reopened.status == "recording";
   }
   else if ( step == "keywordOnly" )
   {
      // (m) Keyword-only master (WBPP: no history at all) - Review Focus 1.
      JEvalJs( "(function(){ var w = new ImageWindow( 40, 40, 1, 32, true, false, \"pcTrkWbpp\" ); })()" );
      st.made.push_back( "pcTrkWbpp" );
      JSetKeywords( "pcTrkWbpp", WbppMasterKeywords() );
      JTick( trk, 2 );
      const int64 wi = trk.ImageOfView( "pcTrkWbpp" );
      AcquisitionFacts wa;
      store.Acquisition( wi, wa );
      st.d["keywordOnly"] = { { "image", wi }, { "subCount", wa.subCount.value_or( -1 ) }, { "filter", wa.filter },
                              { "why", trk.StatusFor( "pcTrkWbpp" ).why } };
      st.ok["keywordOnly"] = wi != 0 && wa.subCount == 10 && wa.filter == "NoFilter"
                          && trk.StatusFor( "pcTrkWbpp" ).why == "keyword IMAGETYP='Master Light'";
   }
   else if ( step == "locked" )
   {
      // (n) Library locked by another program while a step (made at top level) waits: paused, kept
      //     queued, recorded after release (Review Focus 5).
      const int before = ActiveSteps( store, mimg );
      JourneyStatus ps;
      {
         RawDb other( store.DbPath() );
         if ( !other.Exec( "BEGIN EXCLUSIVE" ) )
            throw Error( "locked: BEGIN EXCLUSIVE failed on the second connection" );
         JTick( trk );
         ps = trk.StatusFor( "pcTrkRenamed" );
         if ( !other.Exec( "COMMIT" ) )
            throw Error( "locked: COMMIT failed on the second connection" );
      }
      JTick( trk );
      st.d["locked"] = { { "state", int( ps.state ) }, { "reason", U8( ps.reason ) }, { "gaps", store.Gaps( jid ).size() },
                         { "before", before }, { "after", ActiveSteps( store, mimg ) } };
      st.ok["locked"] = ps.state == RecordingState::Paused && ps.reason.Contains( "locked" )
                     && ActiveSteps( store, mimg ) == before + 1 && store.Gaps( jid ).empty();
   }
   else if ( step == "gapArm" )
   {
      // (o) History read of pcTrkRenamed keeps failing -> a gap after 3 tries, then recording goes on.
      //     Other images read normally (review M3: the pause is per image).
      st.gapCalls = 0;
      trk.SetHistoryReaderForSelfTest( [&st]( const IsoString& id, int from )
      {
         if ( id != "pcTrkRenamed" )
            return ReadViewHistory( id, from );
         HistorySnapshot s;
         ++st.gapCalls;
         s.error = "history read of " + String( id ) + " failed: injected";
         return s;
      } );
   }
   else if ( step == "gapTick" )
   {
      JTick( trk );
      if ( payload.value( "first", false ) )
      {
         // Review M3: pcTrkRgb recorded a step in the same tick; pcTrkRenamed's failure stays ITS pause.
         const JourneyStatus a = trk.StatusFor( "pcTrkRenamed" ), b = trk.StatusFor( "pcTrkRgb" );
         st.d["perImagePause"] = { { "failing", int( a.state ) }, { "failingReason", U8( a.reason ) },
                                   { "other", int( b.state ) }, { "otherReason", U8( b.reason ) } };
         st.ok["perImagePause"] = a.state == RecordingState::Paused && a.reason.Contains( "injected" )
                               && b.state == RecordingState::Recording;
      }
   }
   else if ( step == "gapEnd" )
   {
      trk.SetHistoryReaderForSelfTest( HistoryReadFn() );   // back to ReadViewHistory
      const std::vector<GapRow> gaps = store.Gaps( jid );
      st.d["gap"] = { { "calls", st.gapCalls }, { "gaps", gaps.size() }, { "reason", gaps.empty() ? "" : gaps[0].reason } };
      st.ok["gap"] = gaps.size() == 1 && gaps[0].reason.find( "injected" ) != std::string::npos && gaps[0].imageId == mimg;
      const int before = ActiveSteps( store, mimg );
      JTick( trk );   // the real reader catches up on the next change: the missed steps are recorded now ...
      // ... and the gap that stood for them is resolved (review M4), not left in front of recorded steps.
      st.d["gapResolved"] = { { "gapsAfter", store.Gaps( jid ).size() }, { "stepsBefore", before },
                              { "stepsAfter", ActiveSteps( store, mimg ) },
                              { "state", int( trk.StatusFor( "pcTrkRenamed" ).state ) } };
      st.ok["gapResolved"] = store.Gaps( jid ).empty() && ActiveSteps( store, mimg ) == before + 4
                          && trk.StatusFor( "pcTrkRenamed" ).state == RecordingState::Recording;
   }
   else if ( step == "offBegin" )
   {
      // (p) Recording off: nothing read or stored; status Off.
      st.offBefore = ActiveSteps( store, mimg );
      trk.SetEnabled( false );
   }
   else if ( step == "off" )
   {
      JTick( trk );
      st.d["off"] = { st.offBefore, ActiveSteps( store, mimg ), int( trk.StatusFor( "pcTrkRenamed" ).state ) };
      st.ok["off"] = ActiveSteps( store, mimg ) == st.offBefore && trk.StatusFor( "pcTrkRenamed" ).state == RecordingState::Off;
      trk.SetEnabled( true );
      JTick( trk );
   }
   else if ( step == "preview" )
   {
      // (q) A step on a preview (made at top level) is not part of the image's journey (Ruling 25).
      const int before = ActiveSteps( store, mimg );
      JTick( trk );
      st.d["preview"] = { before, ActiveSteps( store, mimg ) };
      st.ok["preview"] = ActiveSteps( store, mimg ) == before;
   }
   else if ( step == "bigJoin" )
   {
      // (r) Per-step cost on 60 MP RGB float within the Task 1 budget (spec §9): the window (made at top
      //     level with master keywords) joins first ...
      st.made.push_back( "pcTrkBig" );
      JTick( trk, 2 );
   }
   else if ( step == "big" )
   {
      // ... then one step (top level) is recorded and timed.
      JTick( trk );
      st.d["budget"] = { { "lastStepMs", trk.LastStepMs() }, { "budgetMs", PICopilotJourneyStepBudgetMs },
                         { "image", trk.ImageOfView( "pcTrkBig" ) } };
      st.ok["budget"] = trk.ImageOfView( "pcTrkBig" ) != 0 && trk.LastStepMs() <= PICopilotJourneyStepBudgetMs;
      // (closed at top level, from JS, after this phase)
   }
   else if ( step == "scanMode" )
   {
      // (s) Both scan modes detect a step with no notification (the backstop).
      trk.SetScanUsesModifyCountForSelfTest( payload.at( "mc" ).get<bool>() );
      st.scanBefore = ActiveSteps( store, mimg );
   }
   else if ( step == "scanCheck" )
   {
      JTick( trk );
      const bool one = ActiveSteps( store, mimg ) == st.scanBefore + 1;
      st.d["scanModes"][payload.at( "mc" ).get<bool>() ? "modifyCount" : "batch"] = { st.scanBefore, ActiveSteps( store, mimg ) };
      st.scanModes = st.scanModes && one;
   }
   else if ( step == "abortSeen" )
   {
      // Re-review R5 (b): the console abort state after a TOP-LEVEL ImageWindow.open(), as the script saw it
      // right after the open (js) and as this phase sees it at its start, before anything resets it.
      const std::string where = payload.at( "where" ).get<std::string>();
      st.d["abortMeasured"][where] = { { "jsRightAfterOpen", payload.value( "js", false ) },
                                       { "cppAtNextPhaseStart", Console().AbortEnabled() } };
   }
   else if ( step == "abortReset" )
   {
      st.d["abortMeasured"][payload.at( "where" ).get<std::string>()]["fixtureReset"] = true;
   }
   else if ( step == "pjsrOpen" )
   {
      // R5 (b): Copilot's own run_pjsr path (RunPjsr) running a script that opens a file.
      const bool before = Console().AbortEnabled();
      const std::string path = payload.at( "path" ).get<std::string>();
      const PjsrRun r = RunPjsr( String( "var ws = ImageWindow.open( " ) + String( ScriptLiteral( FromU8( path ) ).c_str() )
                                 + " ); ws[0].mainView.id = \"pcTrkPjsrOpen\"; return console.abortEnabled;", IsoString() );
      const bool after = Console().AbortEnabled();
      st.made.push_back( "pcTrkPjsrOpen" );
      st.d["abortMeasured"]["runPjsrOpen"] = { { "before", before }, { "insideScriptAfterOpen", U8( r.value ) },
                                               { "cppAfterRunPjsrReturned", after }, { "ok", r.ok }, { "error", U8( r.error ) } };
      // The fix: RunPjsr restores the abort state the script found (it was left enabled, measured).
      st.ok["pjsrAbortRestored"] = r.ok && !before && !after;
   }
   else if ( step == "pjsrOpenNext" )
   {
      st.d["abortMeasured"]["runPjsrOpen"]["cppAtNextPhaseStart"] = Console().AbortEnabled();
      J6Close( st, "pcTrkPjsrOpen" );   // made in-process (run_pjsr): no JS wrapper holds it
      JTick( trk );
   }
   else if ( step == "retentionOpen" )
   {
      // Re-review R1: retention never prunes an OPEN journey -- not this instance's (touched once per date,
      // and excluded), not another instance's (its touch wins the per-journey re-check), and a journey
      // kept by another instance between the SELECT and the DELETE survives with its folder, silently.
      auto backdate = [&store]( int64 id )
      {
         RawDb raw( store.DbPath() );
         if ( !raw.Exec( ( "UPDATE journey SET updated='2000-01-01T00:00:00.000Z' WHERE id=" + std::to_string( id ) ).c_str() ) )
            throw Error( "retentionOpen: backdating failed" );
      };
      JourneyRow j;
      // (1) this instance's open journey.
      backdate( jid );
      std::string touched, lastRun;
      const int n1 = RetentionPass( store, trk, 30, "2026-10-01", touched, lastRun, nullptr );
      const bool survived1 = store.GetJourney( jid, j );
      const std::string updated1 = j.updated;
      // (2) another instance (a second handle on the same library, no open journeys of its own) prunes on
      //     the same date after this instance touched its open journey.
      String oe;
      std::unique_ptr<JourneyStore> other = JourneyStore::Open( store.Root(), oe );
      if ( !other )
         throw Error( "retentionOpen: second handle: " + oe );
      backdate( jid );
      RetentionPass( store, trk, 30, "2026-10-02", touched, lastRun, nullptr );   // this instance touches first
      std::string lastOther;
      const int n2 = RunRetentionIfDue( *other, 30, "2026-10-02", lastOther, nullptr );
      const bool survived2 = store.GetJourney( jid, j );
      // (3) kept by the other instance between the SELECT and the DELETE.
      const int64 x = store.CreateJourney( "raced", "Raced", NowIso() );
      backdate( x );
      if ( !EnsurePrivateDirectory( store.JourneyDir( x ) ).IsEmpty() )
         throw Error( "retentionOpen: cannot make the raced journey's folder" );
      store.SetPruneHookForSelfTest( [&other, x]( int64 id ) { if ( id == x ) other->MarkKept( x, 0, NowIso() ); } );
      std::string raceError, lastRace;
      int n3 = -2;
      try { n3 = RunRetentionIfDue( store, 30, "2026-10-03", lastRace, nullptr ); }
      catch ( const pcl::Exception& e ) { raceError = U8( e.Message() ); }
      store.SetPruneHookForSelfTest( nullptr );
      JourneyRow xr;
      const bool xKept = store.GetJourney( x, xr ) && xr.kept;
      const bool xFolder = File::DirectoryExists( store.JourneyDir( x ) );
      other.reset();
      st.d["retentionOpen"] = { { "n1", n1 }, { "survived1", survived1 }, { "updatedAfterPass", updated1 },
                                { "n2", n2 }, { "survived2", survived2 }, { "n3", n3 }, { "raceError", raceError },
                                { "raceKept", xKept }, { "raceFolder", xFolder } };
      st.ok["retentionOpen"] = survived1 && updated1 > "2026" && survived2 && raceError.empty() && n3 >= 0 && xKept && xFolder;
   }
   else if ( step == "rowGoneSetup" )
   {
      // Re-review R2: journey rows removed under a live tracker (another instance, a restore).
      JEvalJs( "(function(){ new ImageWindow( 34, 34, 1, 32, true, false, \"pcTrkG1\" );"
               " new ImageWindow( 34, 34, 1, 32, true, false, \"pcTrkG2\" );"
               " new ImageWindow( 34, 34, 1, 32, true, false, \"pcTrkG3\" ); })()" );
      for ( const char* g : { "pcTrkG1", "pcTrkG2", "pcTrkG3" } )
      {
         JSetKeywords( g, Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", ( std::string( "'" ) + g + "'" ).c_str() } } ) );
         st.made.push_back( g );
      }
      st.made.push_back( "pcTrkG2r" );
      JTick( trk, 3 );
      std::string ids;
      nlohmann::json js = nlohmann::json::array();
      for ( const char* g : { "pcTrkG1", "pcTrkG2", "pcTrkG3" } )
      {
         const int64 j = trk.JourneyOfView( g );
         if ( j == 0 )
            throw Error( String( "rowGoneSetup: " ) + g + " did not join" );
         ids += (ids.empty() ? "" : ",") + std::to_string( j );
         js.push_back( j );
         st.d["rowGone"]["images"][g] = trk.ImageOfView( g );
      }
      {
         RawDb raw( store.DbPath() );
         if ( !raw.Exec( "PRAGMA foreign_keys=ON" ) || !raw.Exec( ( "DELETE FROM journey WHERE id IN (" + ids + ")" ).c_str() ) )
            throw Error( "rowGoneSetup: delete failed" );
      }
      st.d["rowGone"]["deleted"] = js;
      trk.TakeJoinNotes();
      J6Close( st, "pcTrkG1" );                        // (a) closed
      J6MainView( "pcTrkG2" ).Rename( "pcTrkG2r" );   // (b) renamed; (c) pcTrkG3 is stepped at top level
      trk.OnImageRenamed( J6MainView( "pcTrkG2r" ), JourneyWallNow() );
   }
   else if ( step == "rowGone" )
   {
      const int before = ActiveSteps( store, mimg );
      JEvalJs( "(function(){ new ImageWindow( 30, 30, 1, 32, true, false, \"pcTrkNewAfter\" ); })()" );
      JSetKeywords( "pcTrkNewAfter", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'TrkNewAfter'" } } ) );
      st.made.push_back( "pcTrkNewAfter" );
      JTick( trk, 3 );
      int removedNotes = 0;
      for ( const String& n : trk.TakeJoinNotes() )
         if ( n.Contains( "was removed" ) )
            ++removedNotes;
      const JourneyStatus s = trk.StatusFor( "pcTrkRenamed" );
      nlohmann::json& d = st.d["rowGone"];
      // The removed images are no longer recorded into their vanished rows: dropped, then (master keywords)
      // joined as NEW images of NEW journeys; journey ids are never reused (AUTOINCREMENT).
      bool noneInDeleted = trk.ImageOfView( "pcTrkG2r" ) != d["images"]["pcTrkG2"].get<int64>()
                        && trk.ImageOfView( "pcTrkG3" ) != d["images"]["pcTrkG3"].get<int64>();
      for ( const char* g : { "pcTrkG2r", "pcTrkG3", "pcTrkNewAfter" } )
         for ( const nlohmann::json& del : d["deleted"] )
            noneInDeleted = noneInDeleted && trk.JourneyOfView( g ) != del.get<int64>();
      d["after"] = { { "renamedSteps", ActiveSteps( store, mimg ) - before }, { "state", int( s.state ) }, { "reason", U8( s.reason ) },
                     { "newJoined", trk.JourneyOfView( "pcTrkNewAfter" ) }, { "removedNotes", removedNotes },
                     { "g2", trk.JourneyOfView( "pcTrkG2r" ) }, { "g3", trk.JourneyOfView( "pcTrkG3" ) } };
      st.ok["rowGone"] = ActiveSteps( store, mimg ) == before + 1 && s.state == RecordingState::Recording
                      && trk.JourneyOfView( "pcTrkNewAfter" ) != 0 && removedNotes >= 2 && noneInDeleted;
   }
   else if ( step == "offOn1" )
   {
      // Re-review R3: a window created and processed while recording was OFF brings that processing as its
      // past (base) when recording comes back on; only later steps are the user's recorded steps.
      trk.SetEnabled( false );
      st.made.push_back( "pcTrkOff" );
   }
   else if ( step == "offOn2" )
   {
      JSetKeywords( "pcTrkOff", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'TrkOff'" } } ) );
      trk.SetEnabled( true );
      JTick( trk, 3 );
      const int64 img = trk.ImageOfView( "pcTrkOff" );
      st.d["offThenOn"] = { { "image", img }, { "activeAtJoin", img != 0 ? ActiveSteps( store, img ) : -1 } };
   }
   else if ( step == "offOn3" )
   {
      JTick( trk );
      const int64 img = trk.ImageOfView( "pcTrkOff" );
      st.d["offThenOn"]["activeAfterStep"] = img != 0 ? ActiveSteps( store, img ) : -1;
      st.ok["offThenOn"] = img != 0 && st.d["offThenOn"]["activeAtJoin"] == 0 && st.d["offThenOn"]["activeAfterStep"] == 1;
   }
   else if ( step == "gateStall1" )
   {
      // Re-review R4 (1): an UNRELATED locked view (Blink-like) does not stall recording.
      const int before = ActiveSteps( store, mimg );
      JWaitIdle();
      int deferred = 0;
      {
         View other = J6MainView( "pcTrkRgb" );
         AutoViewLock lock( other );
         JPump( 400 );   // past the quiet period of the lock's own notification; the lock stays held
         const int d0 = trk.Deferrals();
         trk.Tick( JourneyWallNow(), true );
         deferred = trk.Deferrals() - d0;
      }
      st.d["gateStall"]["lockedElsewhere"] = { { "before", before }, { "after", ActiveSteps( store, mimg ) }, { "deferred", deferred } };
      st.ok["gateStallLock"] = ActiveSteps( store, mimg ) == before + 1 && deferred == 0;
      JPump( 400 );
   }
   else if ( step == "gateStall2" )
   {
      // R4 (2): the console abort left on for more than 5 s: the strip says "waiting: ...", and one note.
      JWaitIdle();
      trk.TakeJoinNotes();
      JourneyStatus during;
      {
         Console c;
         c.EnableAbort();
         const jclock::time_point t0 = jclock::now();
         while ( MsSince( t0 ) < 5600 )
         {
            trk.Tick( JourneyWallNow(), true );
            JPump( 200 );
         }
         during = trk.StatusFor( "pcTrkRenamed" );
         c.DisableAbort();
      }
      int waitingNotes = 0;
      for ( const String& n : trk.TakeJoinNotes() )
         if ( n.Contains( "waiting" ) )
            ++waitingNotes;
      JTick( trk );
      const JourneyStatus after = trk.StatusFor( "pcTrkRenamed" );
      st.d["gateStall"]["abortOn"] = { { "state", int( during.state ) }, { "reason", U8( during.reason ) },
                                        { "waitingNotes", waitingNotes }, { "afterState", int( after.state ) } };
      st.ok["gateStallWaiting"] = during.state == RecordingState::Paused && during.reason.StartsWith( "waiting: " )
                               && waitingNotes == 1 && after.state == RecordingState::Recording;
   }
   else if ( step == "gateStall3" )
   {
      // R4 (3): a tracked window closed with an unrecorded step while the gate stalls: a gap, not nothing.
      const int64 rImg = trk.ImageOfView( "pcTrkRgb_R" );
      const size_t gaps0 = store.Gaps( st.rgbJ ).size();
      trk.OnImageUpdated( J6MainView( "pcTrkRgb_R" ), JourneyWallNow() );   // the notification the panel forwards
      {
         Console c;
         c.EnableAbort();
         J6Close( st, "pcTrkRgb_R" );
         trk.Tick( JourneyWallNow(), true );   // gated: the close is seen, nothing is read
         c.DisableAbort();
      }
      JTick( trk );   // the gap is written
      int closedGaps = 0;
      for ( const GapRow& g : store.Gaps( st.rgbJ ) )
         if ( g.imageId == rImg && g.reason.find( "closed before its last steps were recorded" ) != std::string::npos )
            ++closedGaps;
      st.d["gateStall"]["closedDirty"] = { { "image", rImg }, { "gapsBefore", gaps0 }, { "closedGaps", closedGaps } };
      st.ok["gateStallGap"] = rImg != 0 && closedGaps == 1;
   }
   else if ( step == "txDefer" )
   {
      // Re-review m1: a tick while a JourneyStore::Transaction is open defers (never fails / nests).
      // m2: a dirty image with nothing to write takes no write lock (another program's lock is no pause).
      const int before = ActiveSteps( store, mimg );
      JWaitIdle();
      int deferred = 0, during = -1;
      {
         JourneyStore::Transaction tx( store );
         const int d0 = trk.Deferrals();
         trk.Tick( JourneyWallNow(), true );
         deferred = trk.Deferrals() - d0;
         during = ActiveSteps( store, mimg );
         tx.Commit();
      }
      JTick( trk );
      const int after = ActiveSteps( store, mimg );
      JourneyStatus noop;
      {
         RawDb other( store.DbPath() );
         if ( !other.Exec( "BEGIN EXCLUSIVE" ) )
            throw Error( "txDefer: BEGIN EXCLUSIVE failed" );
         trk.OnImageUpdated( J6MainView( "pcTrkRenamed" ), JourneyWallNow() );   // dirty, but History unchanged
         JPump( 400 );
         trk.Tick( JourneyWallNow(), true );
         noop = trk.StatusFor( "pcTrkRenamed" );
         other.Exec( "COMMIT" );
      }
      st.d["txDefer"] = { { "deferred", deferred }, { "during", during }, { "before", before }, { "after", after },
                          { "noopState", int( noop.state ) }, { "noopReason", U8( noop.reason ) } };
      st.ok["txDefer"] = deferred >= 1 && during == before && after == before + 1;
      st.ok["noopNoLock"] = noop.state == RecordingState::Recording;
   }
   else if ( step == "owner" )
   {
      // Re-review m6: a row recorded by ANOTHER running PixInsight instance is never resumed; a row whose
      // owner process is gone is. (History-less windows so that their fingerprints match.)
      trk.SetHistoryReaderForSelfTest( []( const IsoString& id, int from )
      {
         if ( !id.StartsWith( "pcTrkOwn" ) )
            return ReadViewHistory( id, from );
         HistorySnapshot s;
         s.ok = true;
         return s;
      } );
      auto make = [&]( const char* id )
      {
         JEvalJs( String( "(function(){ new ImageWindow( 42, 42, 1, 32, true, false, \"" ) + id + "\" ); })()" );
         JSetKeywords( id, WbppMasterKeywords() );
         JTick( trk, 2 );
      };
      auto setOwner = [&store]( int64 img, const std::string& owner )
      {
         RawDb raw( store.DbPath() );
         if ( !raw.Exec( ( "UPDATE image SET owner='" + owner + "' WHERE id=" + std::to_string( img ) ).c_str() ) )
            throw Error( "owner: update failed" );
      };
      make( "pcTrkOwn1" );
      const int64 a = trk.ImageOfView( "pcTrkOwn1" );
      ImageRow ra;
      store.GetImage( a, ra );
      const std::string ownerWhileOpen = ra.owner;
      J6Close( st, "pcTrkOwn1" );
      JTick( trk );
      store.GetImage( a, ra );
      const std::string ownerAfterClose = ra.owner;
      const std::string live = JourneyOwnerOf( 1 );   // pid 1: a live process that is not this one
      setOwner( a, live );
      make( "pcTrkOwn2" );
      const int64 b = trk.ImageOfView( "pcTrkOwn2" );
      J6Close( st, "pcTrkOwn2" );
      JTick( trk );
      setOwner( b, live );
      setOwner( a, "999999999:1" );   // a process that no longer exists
      make( "pcTrkOwn3" );
      const int64 c = trk.ImageOfView( "pcTrkOwn3" );
      J6Close( st, "pcTrkOwn3" );
      JTick( trk );
      trk.SetHistoryReaderForSelfTest( HistoryReadFn() );
      st.d["owner"] = { { "self", JourneyOwnerOf() }, { "whileOpen", ownerWhileOpen }, { "afterClose", ownerAfterClose },
                        { "live", live }, { "a", a }, { "b", b }, { "c", c } };
      st.ok["owner"] = a != 0 && ownerWhileOpen == JourneyOwnerOf() && ownerAfterClose.empty() && !live.empty()
                    && JourneyOwnerAlive( live ) && !JourneyOwnerAlive( "999999999:1" )
                    && b != 0 && b != a && c == a;
   }
   else if ( step == "gapRowGone" )
   {
      // Re-review N1: a queued gap whose image vanished must not wedge every later tick.
      auto master = [&]( const char* id )
      {
         JEvalJs( String( "(function(){ new ImageWindow( 28, 28, 1, 32, true, false, \"" ) + id + "\" ); })()" );
         JSetKeywords( id, Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", ( std::string( "'" ) + id + "'" ).c_str() } } ) );
         st.made.push_back( id );
      };
      auto deleteJourney = [&store]( int64 j )
      {
         RawDb raw( store.DbPath() );
         if ( !raw.Exec( "PRAGMA foreign_keys=ON" ) || !raw.Exec( ( "DELETE FROM journey WHERE id=" + std::to_string( j ) ).c_str() ) )
            throw Error( "delete journey failed" );
      };
      const int before = ActiveSteps( store, mimg );   // pcTrkRenamed was stepped at top level: recorded below
      master( "pcTrkGG" );
      master( "pcTrkGH" );
      JTick( trk, 3 );
      deleteJourney( trk.JourneyOfView( "pcTrkGG" ) );
      deleteJourney( trk.JourneyOfView( "pcTrkGH" ) );
      trk.SetHistoryReaderForSelfTest( []( const IsoString& id, int from )
      {
         if ( id != "pcTrkGG" )
            return ReadViewHistory( id, from );
         HistorySnapshot s;
         s.error = "injected read failure";
         return s;
      } );
      nlohmann::json failures = nlohmann::json::array();
      for ( int i = 0; i < 4; ++i )   // (a) 3 failed reads -> a gap for a vanished image
      {
         trk.OnImageUpdated( J6MainView( "pcTrkGG" ), JourneyWallNow() );
         JTick( trk );
         failures.push_back( U8( trk.TickFailureForSelfTest() ) );
      }
      trk.SetHistoryReaderForSelfTest( HistoryReadFn() );
      // (b) a dirty close under the gate after its row was deleted.
      trk.OnImageUpdated( J6MainView( "pcTrkGH" ), JourneyWallNow() );
      {
         Console c;
         c.EnableAbort();
         J6Close( st, "pcTrkGH" );
         trk.Tick( JourneyWallNow(), true );
         c.DisableAbort();
      }
      for ( int i = 0; i < 3; ++i )
      {
         JTick( trk );
         failures.push_back( U8( trk.TickFailureForSelfTest() ) );
      }
      bool anyFailure = false;
      for ( const nlohmann::json& f : failures )
         anyFailure = anyFailure || !f.get<std::string>().empty();
      const JourneyStatus s = trk.StatusFor( "pcTrkRenamed" );
      st.d["gapRowGone"] = { { "tickFailures", failures }, { "renamedSteps", ActiveSteps( store, mimg ) - before },
                             { "state", int( s.state ) }, { "pending", trk.PendingCount() } };
      st.ok["gapRowGone"] = !anyFailure && ActiveSteps( store, mimg ) == before + 1 && s.state == RecordingState::Recording;
      J6Close( st, "pcTrkGG" );
      JTick( trk );
   }
   else if ( step == "linkRowGone" )
   {
      // Re-review N2 / I-C (the reviewer's sequence): a Copilot-linked join into a vanished journey is reached
      // (" rowMissing: " on the derived window), never escapes the candidate loop, and another closed
      // journey still ends.
      for ( const char* id : { "pcTrkLM", "pcTrkLE" } )
      {
         JEvalJs( String( "(function(){ new ImageWindow( 27, 27, 1, 32, true, false, \"" ) + id + "\" ); })()" );
         JSetKeywords( id, Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", ( std::string( "'" ) + id + "'" ).c_str() } } ) );
         st.made.push_back( id );
      }
      JTick( trk, 3 );   // 1. both tracked and clean
      const int64 lm = trk.JourneyOfView( "pcTrkLM" ), le = trk.JourneyOfView( "pcTrkLE" );
      trk.NoteCopilotStep( "pcTrkLM", "PixelMath", "derived", { "pcTrkLD" }, false, JourneyWallNow() );   // 2.
      JTick( trk );      // 3. LM is re-read, finds nothing new; the note stays
      {                  // 4.
         RawDb raw( store.DbPath() );
         if ( !raw.Exec( "PRAGMA foreign_keys=ON" ) || !raw.Exec( ( "DELETE FROM journey WHERE id=" + std::to_string( lm ) ).c_str() ) )
            throw Error( "delete journey failed" );
      }
      trk.TakeJoinNotes();
      JEvalJs( "(function(){ new ImageWindow( 27, 27, 1, 32, true, false, \"pcTrkLD\" ); })()" );   // 5.
      st.made.push_back( "pcTrkLD" );
      J6Close( st, "pcTrkLE" );   // its journey must still end
      nlohmann::json failures = nlohmann::json::array();
      for ( int i = 0; i < 5; ++i )   // 6.
      {
         JTick( trk );
         failures.push_back( U8( trk.TickFailureForSelfTest() ) );
      }
      bool anyFailure = false;
      for ( const nlohmann::json& f : failures )
         anyFailure = anyFailure || !f.get<std::string>().empty();
      int removedNotes = 0;
      for ( const String& n : trk.TakeJoinNotes() )
         if ( n.Contains( "was removed" ) )
            ++removedNotes;
      int ldRowMissing = 0;   // 7. the N2 path was reached
      for ( const std::string& x : trk.RecentDecisionsForSelfTest() )
         if ( x.find( " pcTrkLD " ) != std::string::npos && x.find( " rowMissing: " ) != std::string::npos )
            ++ldRowMissing;
      JourneyRow ler;
      store.GetJourney( le, ler );
      st.d["linkRowGone"] = { { "tickFailures", failures }, { "removedNotes", removedNotes }, { "leStatus", ler.status },
                              { "ld", trk.JourneyOfView( "pcTrkLD" ) }, { "lm", trk.JourneyOfView( "pcTrkLM" ) }, { "deleted", lm },
                              { "ldRowMissingDecisions", ldRowMissing } };
      st.ok["linkRowGone"] = !anyFailure && ldRowMissing >= 1 && removedNotes >= 1 && ler.status == "ended"
                          && trk.JourneyOfView( "pcTrkLD" ) != lm && trk.JourneyOfView( "pcTrkLM" ) != lm;
      J6Close( st, "pcTrkLD" );
      J6Close( st, "pcTrkLM" );
      JTick( trk );
   }
   else if ( step == "recentSetup" )
   {
      // Re-review I-A: a tracked master whose recorded step will vanish.
      JEvalJs( "(function(){ new ImageWindow( 29, 29, 1, 32, true, false, \"pcTrkRX\" ); })()" );
      JSetKeywords( "pcTrkRX", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'TrkRX'" } } ) );
      st.made.push_back( "pcTrkRX" );
      JTick( trk, 3 );
   }
   else if ( step == "recentRowGone" )
   {
      JTick( trk );   // 1. X's top-level step S is recorded (and remembered for timing)
      const int64 ximg = trk.ImageOfView( "pcTrkRX" );
      const std::vector<StepRow> xs = ximg != 0 ? store.Steps( ximg, false ) : std::vector<StepRow>();
      if ( xs.empty() )
         throw Error( "recentRowGone: pcTrkRX recorded no step" );
      const int64 sId = xs.back().id;
      // 2. within the timing slack: a fresh window with no file, first look defers (timing (c) never decides on it).
      JEvalJs( "(function(){ new ImageWindow( 29, 29, 1, 32, true, false, \"pcTrkRT\" ); })()" );
      st.made.push_back( "pcTrkRT" );
      JTick( trk );   // idle tick: the first look (deferred)
      {  // 3. S vanishes (a restore / another instance); journey and image stay
         RawDb raw( store.DbPath() );
         if ( !raw.Exec( "PRAGMA foreign_keys=ON" ) || !raw.Exec( ( "DELETE FROM step WHERE id=" + std::to_string( sId ) ).c_str() ) )
            throw Error( "recentRowGone: delete failed" );
         const std::vector<std::string> ch = raw.Column( "SELECT changes()" );
         st.d["recentRowGone"]["deleteChanges"] = ch.empty() ? std::string( "?" ) : ch.front();
      }
      {
         StepRow gone;
         st.d["recentRowGone"]["stepGoneForStore"] = !store.GetStep( sId, gone );
      }
      nlohmann::json failures = nlohmann::json::array();
      for ( int i = 0; i < 8; ++i )   // 4. kCandidateTicks + 3
      {
         JTick( trk );
         failures.push_back( U8( trk.TickFailureForSelfTest() ) );
      }
      bool anyFailure = false;
      for ( const nlohmann::json& f : failures )
         anyFailure = anyFailure || !f.get<std::string>().empty();
      int rtRowMissing = 0, rtLooks = 0;
      nlohmann::json rtDecisions = nlohmann::json::array();
      for ( const std::string& x : trk.RecentDecisionsForSelfTest() )
         if ( x.find( " pcTrkRT " ) != std::string::npos )
         {
            rtDecisions.push_back( x );
            ++rtLooks;
            if ( x.find( " rowMissing: " ) != std::string::npos )
               ++rtRowMissing;
         }
      // Settled: T is ignored (no more looks) or linked; the last looks are not row-missing repeats.
      int looksBefore = rtLooks;
      JTick( trk, 3 );
      int looksAfter = 0;
      for ( const std::string& x : trk.RecentDecisionsForSelfTest() )
         if ( x.find( " pcTrkRT " ) != std::string::npos )
            ++looksAfter;
      const JourneyStatus xs2 = trk.StatusFor( "pcTrkRX" );
      nlohmann::json rtLinks = nlohmann::json::array();
      if ( trk.JourneyOfView( "pcTrkRT" ) != 0 )
         for ( const LinkRow& l : store.Links( trk.JourneyOfView( "pcTrkRT" ) ) )
            if ( l.toImageId == trk.ImageOfView( "pcTrkRT" ) )
               rtLinks.push_back( { l.fromImageId, l.viaStepId, l.evidence } );
      st.d["recentRowGone"].update( nlohmann::json( {
                                { "deletedStep", sId }, { "rowMissingDecisions", rtRowMissing }, { "looks", looksBefore },
                                { "decisions", rtDecisions }, { "links", rtLinks }, { "ximg", ximg },
                                { "looksAfterSettling", looksAfter }, { "tickFailures", failures },
                                { "rt", trk.ImageOfView( "pcTrkRT" ) }, { "xState", int( xs2.state ) }, { "xReason", U8( xs2.reason ) } } ) );
      st.ok["recentRowGone"] = !anyFailure && rtRowMissing <= 2 && (looksAfter == looksBefore || trk.ImageOfView( "pcTrkRT" ) != 0)
                            && xs2.state == RecordingState::Recording;
      J6Close( st, "pcTrkRT" );
      J6Close( st, "pcTrkRX" );
      JTick( trk );
   }
   else if ( step == "writeCap" )
   {
      // Re-review m-h: a persistent (non-missing-row) write failure parks the image after 3 tries instead of
      // re-reading its whole history on every tick; the next change retries and records everything.
      int reads = 0;
      trk.SetHistoryReaderForSelfTest( [&reads]( const IsoString& id, int from )
      {
         if ( id == "pcTrkRenamed" )
            ++reads;
         return ReadViewHistory( id, from );
      } );
      JourneyStatus during;
      {
         RawDb other( store.DbPath() );
         if ( !other.Exec( "BEGIN EXCLUSIVE" ) )
            throw Error( "writeCap: BEGIN EXCLUSIVE failed" );
         for ( int i = 0; i < 8; ++i )
         {
            trk.Tick( JourneyWallNow(), true );
            JPump( 60 );
         }
         during = trk.StatusFor( "pcTrkRenamed" );
         other.Exec( "COMMIT" );
      }
      trk.SetHistoryReaderForSelfTest( HistoryReadFn() );
      st.writeCapBefore = ActiveSteps( store, mimg );
      st.d["writeCap"] = { { "historyReadsIn8LockedTicks", reads }, { "state", int( during.state ) }, { "reason", U8( during.reason ) } };
      st.writeCapReads = reads;
      st.writeCapPaused = during.state == RecordingState::Paused && during.reason.Contains( "locked" );
   }
   else if ( step == "writeCapAfter" )
   {
      JTick( trk );
      st.d["writeCap"]["recordedAfterNextChange"] = ActiveSteps( store, mimg ) - st.writeCapBefore;
      st.ok["writeCap"] = st.writeCapReads <= 6 && st.writeCapPaused && ActiveSteps( store, mimg ) == st.writeCapBefore + 2
                       && trk.StatusFor( "pcTrkRenamed" ).state == RecordingState::Recording;
   }
   else if ( step == "ignSetup" )
   {
      // Re-review N3: windows seen, ignored, processed, and then given master keywords.
      JPump( 3500 );   // no recorded step within the timing slack
      for ( const char* id : { "pcTrkIgA", "pcTrkIgB", "pcTrkIgC" } )
         st.made.push_back( id );
      JTick( trk, 6 );
      st.d["ignoredThenMaster"]["ignoredFirst"] = { trk.ImageOfView( "pcTrkIgA" ), trk.ImageOfView( "pcTrkIgB" ), trk.ImageOfView( "pcTrkIgC" ) };
   }
   else if ( step == "ignCheck" )
   {
      // B: the notification path right after the keyword step; A and C: the scan path 1 s later (C with
      // the batch scan mode). With the first sighting carried over, all three agree.
      trk.OnImageUpdated( J6MainView( "pcTrkIgB" ), JourneyWallNow() );
      JPump( 1000 );
      trk.SetScanUsesModifyCountForSelfTest( false );
      JTick( trk, 3 );
      trk.SetScanUsesModifyCountForSelfTest( PICopilotJourneyScanUsesModifyCount && PICopilotJourneyNotificationsWork );
      nlohmann::json counts = nlohmann::json::array();
      bool allThree = true;
      for ( const char* id : { "pcTrkIgA", "pcTrkIgB", "pcTrkIgC" } )
      {
         const int64 img = trk.ImageOfView( id );
         const int n = img != 0 ? ActiveSteps( store, img ) : -1;
         counts.push_back( n );
         allThree = allThree && n == 3;
      }
      st.d["ignoredThenMaster"]["activeUserSteps"] = counts;
      st.ok["ignoredThenMaster"] = allThree && st.d["ignoredThenMaster"]["ignoredFirst"] == nlohmann::json( { 0, 0, 0 } );
   }
   else if ( step == "chaosSetup" )
   {
      // Fix round 3: fault injection. Images in every state under a live tracker; rows deleted in a
      // seeded-random order between ticks.
      for ( int i = 1; i <= 5; ++i )
      {
         const std::string id = "pcTrkC" + std::to_string( i );
         JEvalJs( String( "(function(){ new ImageWindow( 26, 26, 1, 32, true, false, \"" ) + id.c_str() + "\" ); })()" );
         JSetKeywords( id.c_str(), Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", ( "'" + id + "'" ).c_str() } } ) );
         st.made.push_back( id );
         st.chaosViews.push_back( id );
      }
      JTick( trk, 3 );
      for ( const std::string& id : st.chaosViews )
         st.chaosJourneys.push_back( trk.JourneyOfView( IsoString( id.c_str() ) ) );
      // A candidate linked to a master (Copilot note), not joined yet.
      trk.NoteCopilotStep( "pcTrkC1", "PixelMath", "chaos", { "pcTrkCL" }, false, JourneyWallNow() );
      JEvalJs( "(function(){ new ImageWindow( 26, 26, 1, 32, true, false, \"pcTrkCL\" ); })()" );
      st.made.push_back( "pcTrkCL" );
      st.chaosViews.push_back( "pcTrkCL" );
      // An image whose reads fail (-> queued gaps).
      trk.SetHistoryReaderForSelfTest( []( const IsoString& id, int from )
      {
         if ( id != "pcTrkC2" )
            return ReadViewHistory( id, from );
         HistorySnapshot s;
         s.error = "chaos: injected read failure";
         return s;
      } );
      st.chaosBefore = ActiveSteps( store, mimg );
      st.d["chaos"]["journeys"] = st.chaosJourneys;
   }
   else if ( step == "chaosRound" )
   {
      const int round = payload.at( "round" ).get<int>();
      std::mt19937 rng( 7919u + unsigned( round ) );
      Console c;
      nlohmann::json& log = st.d["chaos"]["rounds"][round];
      const int64 masterImage = payload.contains( "master" )
                              ? trk.ImageOfView( IsoString( payload.at( "master" ).get<std::string>().c_str() ) ) : 0;
      bool masterCounted = false;
      for ( int i = 0; i < 10; ++i )
      {
         std::string action;
         const unsigned a0 = rng() % 9;
         const unsigned a = a0 >= 7 ? 0 : a0;   // deletions: 3 in 9; 5 = rename; 6 = image row vanishes, then a dirty close
         // Derived windows the top level made from a chaos master between rounds join the set.
         for ( const ImageWindow& w : ImageWindow::AllWindows() )
         {
            const std::string wid( w.MainView().Id().c_str() );
            if ( wid.rfind( "pcTrkCD", 0 ) == 0 && std::find( st.chaosViews.begin(), st.chaosViews.end(), wid ) == st.chaosViews.end() )
            {
               st.chaosViews.push_back( wid );
               ++st.chaosCount["derivedWindows"];
            }
         }
         std::vector<std::string> openViews;
         for ( const std::string& v : st.chaosViews )
            if ( !ImageWindow::WindowById( IsoString( v.c_str() ) ).IsNull() )
               openViews.push_back( v );
         if ( a == 0 )
         {
            // Delete a random journey / image / step row of the chaos set (another program, a restore).
            std::vector<std::string> rows;
            for ( int64 j : st.chaosJourneys )
            {
               JourneyRow jr;
               if ( !store.GetJourney( j, jr ) )
                  continue;
               rows.push_back( "journey:" + std::to_string( j ) );
               for ( const ImageRow& im : store.Images( j ) )
               {
                  rows.push_back( "image:" + std::to_string( im.id ) );
                  for ( const StepRow& sr : store.Steps( im.id, true ) )
                     rows.push_back( "step:" + std::to_string( sr.id ) );
               }
            }
            if ( !rows.empty() )
            {
               const std::string r = rows[rng() % rows.size()];
               const size_t colon = r.find( ':' );
               RawDb raw( store.DbPath() );
               raw.Exec( "PRAGMA foreign_keys=ON" );
               if ( !raw.Exec( ( "DELETE FROM " + r.substr( 0, colon ) + " WHERE id=" + r.substr( colon + 1 ) ).c_str() ) )
                  throw Error( "chaos: delete failed" );
               action = "delete " + r;
               ++st.chaosCount["deletes"];
            }
         }
         else if ( a == 1 && !openViews.empty() )
         {
            const std::string v = openViews[rng() % openViews.size()];
            trk.OnImageUpdated( J6MainView( v.c_str() ), JourneyWallNow() );
            action = "dirty " + v;
         }
         else if ( a == 2 )
         {
            if ( c.AbortEnabled() ) c.DisableAbort(); else c.EnableAbort();
            action = std::string( "abort " ) + (c.AbortEnabled() ? "on" : "off");
         }
         else if ( a == 3 && openViews.size() > 1 )
         {
            const std::string v = openViews[rng() % openViews.size()];
            trk.OnImageUpdated( J6MainView( v.c_str() ), JourneyWallNow() );   // closed with unrecorded changes
            J6Close( st, v );
            action = "close " + v;
            ++st.chaosCount["closes"];
         }
         else if ( a == 4 )
         {
            const std::string id = "pcTrkCN" + std::to_string( ++st.chaosNew );
            if ( !openViews.empty() && rng() % 2 == 0 )
               trk.NoteCopilotStep( IsoString( openViews[rng() % openViews.size()].c_str() ), "PixelMath", "chaos", { id }, false,
                                    JourneyWallNow() );
            JEvalJs( String( "(function(){ new ImageWindow( 25, 25, 1, 32, true, false, \"" ) + id.c_str() + "\" ); })()" );
            if ( rng() % 2 == 0 )
               JSetKeywords( id.c_str(), Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", ( "'" + id + "'" ).c_str() } } ) );
            st.made.push_back( id );
            st.chaosViews.push_back( id );
            action = "new " + id;
         }
         else if ( a == 5 && !openViews.empty() )
         {
            const std::string v = openViews[rng() % openViews.size()];
            const std::string nv = v + "r";
            J6MainView( v.c_str() ).Rename( IsoString( nv.c_str() ) );
            trk.OnImageRenamed( J6MainView( nv.c_str() ), JourneyWallNow() );
            std::replace( st.chaosViews.begin(), st.chaosViews.end(), v, nv );
            st.made.push_back( nv );
            action = "rename " + v + " -> " + nv;
            ++st.chaosCount["renames"];
         }
         else if ( a == 6 )
         {
            // An image row vanishes (another instance / a restore) and its window is then closed with an
            // unrecorded change: DropClosed queues a gap for an image that no longer exists.
            for ( const std::string& v : openViews )
            {
               const int64 img = trk.ImageOfView( IsoString( v.c_str() ) );
               if ( img == 0 )
                  continue;
               {
                  RawDb raw( store.DbPath() );
                  raw.Exec( "PRAGMA foreign_keys=ON" );
                  raw.Exec( ( "DELETE FROM image WHERE id=" + std::to_string( img ) ).c_str() );
               }
               trk.OnImageUpdated( J6MainView( v.c_str() ), JourneyWallNow() );
               J6Close( st, v );
               action = "vanish+close " + v;
               ++st.chaosCount["deletes"];
               ++st.chaosCount["closes"];
               break;
            }
         }
         if ( action.empty() )
            action = "noop";
         const std::vector<std::string> pre = trk.RecentDecisionsForSelfTest();
         const std::string mark = pre.empty() ? std::string() : pre.back();
         trk.Tick( JourneyWallNow(), true );
         JPump( 60 );
         {
            // m-g: what this tick actually did (Decisions after the mark).
            const std::vector<std::string> post = trk.RecentDecisionsForSelfTest();
            bool after = mark.empty() || std::find( post.begin(), post.end(), mark ) == post.end();
            for ( const std::string& x : post )
            {
               if ( after )
               {
                  if ( x.rfind( "dropped ", 0 ) == 0 ) ++st.chaosCount["dropped"];
                  if ( x.rfind( "gap of vanished image", 0 ) == 0 ) ++st.chaosCount["gapOfVanishedImage"];
                  if ( x.find( " rowMissing: " ) != std::string::npos ) ++st.chaosCount["candidateRowMissing"];
               }
               if ( x == mark )
                  after = true;
            }
         }
         // A new chaos journey (a joined new window) joins the deletable set.
         for ( const std::string& v : st.chaosViews )
         {
            const int64 j = trk.JourneyOfView( IsoString( v.c_str() ) );
            if ( j != 0 && std::find( st.chaosJourneys.begin(), st.chaosJourneys.end(), j ) == st.chaosJourneys.end()
              && j != jid && j != st.rgbJ )
               st.chaosJourneys.push_back( j );
         }
         const String f = trk.TickFailureForSelfTest();
         if ( !f.IsEmpty() )
         {
            ++st.chaosTickFailures;
            if ( st.chaosFirstFailure.empty() )
               st.chaosFirstFailure = U8( f );
         }
         log.push_back( action + (f.IsEmpty() ? "" : " -> TICK FAILED: " + U8( f )) );
         // m-2r: the step the top level made on the chaos master, once the tracker RECORDED it (not the JS
         // attempt): an active, non-base step row of the master's image.
         if ( !masterCounted && masterImage != 0 )
            for ( const StepRow& r : store.Steps( masterImage, false ) )
               if ( r.state == "active" && !r.params.value( "base", false ) )
               {
                  ++st.chaosCount["masterSteps"];
                  masterCounted = true;
                  break;
               }
      }
      if ( c.AbortEnabled() )
         c.DisableAbort();
   }
   else if ( step == "chaosEnd" )
   {
      trk.SetHistoryReaderForSelfTest( HistoryReadFn() );
      Console().DisableAbort();
      for ( const std::string& v : st.chaosViews )   // every still-open chaos image is re-read once
         if ( !ImageWindow::WindowById( IsoString( v.c_str() ) ).IsNull() )
            trk.OnImageUpdated( J6MainView( v.c_str() ), JourneyWallNow() );
      JTick( trk, 3 );
      // After the chaos: 10 idle ticks; nothing may fail as a whole, and no per-candidate failure repeats.
      int failuresAfter = 0;
      const std::vector<std::string> pre = trk.RecentDecisionsForSelfTest();
      const std::string lastBefore = pre.empty() ? std::string() : pre.back();
      for ( int i = 0; i < 10; ++i )
      {
         JTick( trk );
         if ( !trk.TickFailureForSelfTest().IsEmpty() )
            ++failuresAfter;
      }
      int repeated = 0;
      {
         const std::vector<std::string> post = trk.RecentDecisionsForSelfTest();
         bool after = lastBefore.empty() || std::find( post.begin(), post.end(), lastBefore ) == post.end();
         for ( const std::string& x : post )
         {
            if ( after && (x.find( " failed: " ) != std::string::npos || x.find( "rowMissing" ) != std::string::npos) )
               ++repeated;   // an item failure still happening in the idle ticks after the chaos
            if ( x == lastBefore )
               after = true;
         }
      }
      // m-g (ii): no per-image pause and no retried gap survives the idle ticks.
      nlohmann::json pausedViews = nlohmann::json::array();
      for ( const std::string& v : st.chaosViews )
         if ( !ImageWindow::WindowById( IsoString( v.c_str() ) ).IsNull() )
         {
            const JourneyStatus s = trk.StatusFor( IsoString( v.c_str() ) );
            if ( s.state == RecordingState::Paused )
               pausedViews.push_back( v + ": " + U8( s.reason ) );
         }
      const size_type pendingGaps = trk.PendingGapsForSelfTest();
      JEvalJs( "(function(){ new ImageWindow( 24, 24, 1, 32, true, false, \"pcTrkChaosNew\" ); })()" );
      JSetKeywords( "pcTrkChaosNew", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'ChaosNew'" } } ) );
      st.made.push_back( "pcTrkChaosNew" );
      JTick( trk, 3 );
      const JourneyStatus sr = trk.StatusFor( "pcTrkRenamed" ), sg = trk.StatusFor( "pcTrkRgb" );
      st.d["chaos"]["end"] = { { "tickFailures", st.chaosTickFailures }, { "firstFailure", st.chaosFirstFailure },
                               { "failuresAfter", failuresAfter }, { "itemFailuresAfterChaos", repeated },
                               { "survivorSteps", ActiveSteps( store, mimg ) - st.chaosBefore },
                               { "renamedState", int( sr.state ) }, { "renamedReason", U8( sr.reason ) },
                               { "rgbState", int( sg.state ) }, { "newJoined", trk.JourneyOfView( "pcTrkChaosNew" ) },
                               { "pausedChaosViews", pausedViews }, { "pendingGaps", pendingGaps } };
      // m-g (i): the run did every kind of thing it claims to test.
      bool coverage = true;
      nlohmann::json cov = nlohmann::json::object();
      for ( const char* k : { "deletes", "closes", "renames", "dropped", "gapOfVanishedImage", "candidateRowMissing", "masterSteps",
                              "derivedWindows" } )
      {
         cov[k] = st.chaosCount[k];
         coverage = coverage && st.chaosCount[k] >= 1;
      }
      st.d["chaos"]["coverage"] = cov;
      st.ok["chaos"] = coverage && pausedViews.empty() && pendingGaps == 0
                    && st.chaosTickFailures == 0 && failuresAfter == 0 && repeated == 0
                    && ActiveSteps( store, mimg ) == st.chaosBefore + 5 && sr.state == RecordingState::Recording
                    && sg.state == RecordingState::Recording && trk.JourneyOfView( "pcTrkChaosNew" ) != 0;
      for ( const std::string& v : st.chaosViews )
         J6Close( st, v );   // made in-process: no JS wrapper holds them
      J6Close( st, "pcTrkChaosNew" );
      JTick( trk );
   }
   else if ( step == "deaths" )
   {
      // Re-review I-1r: a death is kept out of the droppable queue, recorded while recording is off, ordered
      // against the queued events, and consumed exactly once (StartJourneyFor too). A fake OnImageDeleted on a
      // still-open window is, for the tracker, a close followed by a new window at the same address.
      auto master = [&]( const char* id )
      {
         JEvalJs( String( "(function(){ new ImageWindow( 23, 23, 1, 32, true, false, \"" ) + id + "\" ); })()" );
         JSetKeywords( id, Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", ( std::string( "'" ) + id + "'" ).c_str() } } ) );
         st.made.push_back( id );
      };
      auto closedByDeath = [&]( const char* id )
      {
         int n = 0;
         for ( const std::string& x : trk.RecentDecisionsForSelfTest() )
            if ( x == std::string( "closed (death): " ) + id )
               ++n;
         return n;
      };
      master( "pcTrkDW1" );
      master( "pcTrkDW2" );
      master( "pcTrkDW3" );
      JTick( trk, 3 );
      nlohmann::json d = nlohmann::json::object();
      // (1) overflow: 1000 queued events after the death.
      {
         const int before = closedByDeath( "pcTrkDW1" );
         trk.OnImageDeleted( J6MainView( "pcTrkDW1" ), JourneyWallNow() );
         const View other = J6MainView( "pcTrkRenamed" );
         for ( int i = 0; i < 1000; ++i )
            trk.OnImageUpdated( other, JourneyWallNow() );
         JTick( trk, 2 );
         d["overflow"] = { { "closedByDeath", closedByDeath( "pcTrkDW1" ) - before }, { "reSeen", trk.ImageOfView( "pcTrkDW1" ) != 0 } };
         st.ok["deathOverflow"] = closedByDeath( "pcTrkDW1" ) - before == 1 && trk.ImageOfView( "pcTrkDW1" ) != 0;
      }
      // (2) recording off, then on.
      {
         const int before = closedByDeath( "pcTrkDW2" );
         trk.SetEnabled( false );
         trk.OnImageDeleted( J6MainView( "pcTrkDW2" ), JourneyWallNow() );
         trk.SetEnabled( true );
         JTick( trk, 2 );
         d["whileOff"] = { { "closedByDeath", closedByDeath( "pcTrkDW2" ) - before }, { "reSeen", trk.ImageOfView( "pcTrkDW2" ) != 0 } };
         st.ok["deathWhileOff"] = closedByDeath( "pcTrkDW2" ) - before == 1 && trk.ImageOfView( "pcTrkDW2" ) != 0;
      }
      // (3) a Created (and an Updated) of the NEW window at the address, queued after the death: they belong
      //     to the new window -- its first sighting is the Created time, and no gap lands on the old image.
      {
         const int64 oldImg = trk.ImageOfView( "pcTrkDW3" ), oldJ = trk.JourneyOfView( "pcTrkDW3" );
         const size_t gaps0 = store.Gaps( oldJ ).size();
         trk.OnImageDeleted( J6MainView( "pcTrkDW3" ), JourneyWallNow() );
         const double t0 = JourneyWallNow() - 5;   // a distinctive time
         trk.OnImageCreated( J6MainView( "pcTrkDW3" ), t0 );
         trk.OnImageUpdated( J6MainView( "pcTrkDW3" ), JourneyWallNow() );
         trk.SetHistoryReaderForSelfTest( []( const IsoString& id, int from )
         {
            if ( id == "pcTrkDW3" )
            {
               HistorySnapshot s;
               s.busy = true;   // the candidate waits, undecided, so its first sighting can be read
               return s;
            }
            return ReadViewHistory( id, from );
         } );
         JTick( trk );
         const double seen = trk.FirstSeenForSelfTest( "pcTrkDW3" );
         trk.SetHistoryReaderForSelfTest( HistoryReadFn() );
         JTick( trk, 2 );
         d["createdAfter"] = { { "firstSeen", seen }, { "t0", t0 }, { "gapsOnOld", store.Gaps( oldJ ).size() - gaps0 },
                               { "oldImage", oldImg } };
         st.ok["createdAfterDeath"] = std::fabs( seen - t0 ) < 1e-3 && store.Gaps( oldJ ).size() == gaps0;
      }
      // (4) start_journey right after a death: the death is consumed once, the new journey keeps recording.
      {
         JEvalJs( "(function(){ new ImageWindow( 22, 22, 1, 32, true, false, \"pcTrkDX\" ); })()" );
         st.made.push_back( "pcTrkDX" );
         trk.OnImageDeleted( J6MainView( "pcTrkDX" ), JourneyWallNow() );
         String err;
         const int64 jx = trk.StartJourneyFor( J6MainView( "pcTrkDX" ), err, JourneyWallNow() );
         JTick( trk, 2 );
         JourneyRow jr;
         const bool got = jx != 0 && store.GetJourney( jx, jr );
         d["startAfter"] = { { "journey", jx }, { "error", U8( err ) }, { "status", got ? jr.status : std::string() },
                             { "tracked", trk.JourneyOfView( "pcTrkDX" ) } };
         st.ok["startAfterDeath"] = got && jr.status == "recording" && trk.JourneyOfView( "pcTrkDX" ) == jx;
      }
      st.d["deaths"] = d;
      for ( const char* id : { "pcTrkDW1", "pcTrkDW2", "pcTrkDW3", "pcTrkDX" } )
         J6Close( st, id );
      JTick( trk );
   }
   else if ( step == "gate" )
   {
      // Review I4: while PixInsight executes a process or script (console abort enabled), a tick
      // reads and writes nothing (deferred, not waited on); the step made at top level is
      // recorded by the first idle tick.
      const int before = ActiveSteps( store, mimg );
      int deferred = 0, afterAbort = -1;
      JWaitIdle();
      {
         Console c;
         c.EnableAbort();
         const int d0 = trk.Deferrals();
         trk.Tick( JourneyWallNow(), true );
         deferred += trk.Deferrals() - d0;
         c.DisableAbort();
         afterAbort = ActiveSteps( store, mimg );
      }
      // (Re-review R4: a lock on ANOTHER view no longer defers the recorder -- check gateStallLock.)
      JTick( trk );
      st.d["gate"] = { { "before", before }, { "afterAbort", afterAbort },
                       { "after", ActiveSteps( store, mimg ) }, { "deferred", deferred } };
      st.ok["gate"] = afterAbort == before && deferred >= 1 && ActiveSteps( store, mimg ) == before + 1;
   }
   else if ( step == "unrelatedMake" )
   {
      // Review I3 setup: pcTrkU (made at top level) is seen well away from any recorded step, with no
      // link evidence, until it is ignored.
      JPump( 3500 );   // > PICopilotJourneyTimingSlackSeconds after the last recorded step
      st.made.push_back( "pcTrkU" );
      JTick( trk, 6 );
      st.d["noFalseTimingSetup"] = trk.ImageOfView( "pcTrkU" );
   }
   else if ( step == "noFalseTiming" )
   {
      // Review I3: right after a recorded step on the master, the ignored pcTrkU is changed and a saved
      // file is opened. Neither APPEARED from that step: timing (c) must link neither.
      const std::string fileId = payload.at( "fileId" ).get<std::string>();
      st.made.push_back( fileId );
      const int before = ActiveSteps( store, mimg );
      JTick( trk, 5 );
      std::vector<std::string> dec;
      for ( const std::string& x : trk.RecentDecisionsForSelfTest() )
         if ( x.find( "pcTrkU" ) != std::string::npos || x.find( fileId ) != std::string::npos )
            dec.push_back( x );
      st.d["noFalseTiming"] = { { "masterStep", ActiveSteps( store, mimg ) - before }, { "u", trk.ImageOfView( "pcTrkU" ) },
                                { "file", trk.ImageOfView( IsoString( fileId.c_str() ) ) }, { "fileId", fileId },
                                { "decisions", dec } };
      st.ok["noFalseTiming"] = st.d["noFalseTimingSetup"] == 0 && ActiveSteps( store, mimg ) == before + 1
                            && trk.ImageOfView( "pcTrkU" ) == 0 && trk.ImageOfView( IsoString( fileId.c_str() ) ) == 0;
   }
   else if ( step == "dupMaster" )
   {
      // Review I2: the same history-less (WBPP) master opened twice is two images, never one row shared
      // by two windows. The reader returns an empty history for both, like a WBPP master file.
      trk.SetHistoryReaderForSelfTest( []( const IsoString& id, int from )
      {
         if ( !id.StartsWith( "pcTrkDup" ) )
            return ReadViewHistory( id, from );
         HistorySnapshot s;
         s.ok = true;
         return s;
      } );
      JEvalJs( "(function(){ var w = new ImageWindow( 44, 44, 1, 32, true, false, \"pcTrkDup1\" ); })()" );
      st.made.push_back( "pcTrkDup1" );
      JSetKeywords( "pcTrkDup1", WbppMasterKeywords() );
      JTick( trk, 2 );
      JEvalJs( "(function(){ var w = new ImageWindow( 44, 44, 1, 32, true, false, \"pcTrkDup2\" ); })()" );
      st.made.push_back( "pcTrkDup2" );
      JSetKeywords( "pcTrkDup2", WbppMasterKeywords() );
      JTick( trk, 2 );
      trk.SetHistoryReaderForSelfTest( HistoryReadFn() );
      const int64 a = trk.ImageOfView( "pcTrkDup1" ), b = trk.ImageOfView( "pcTrkDup2" );
      ImageRow ra, rb;
      const bool ga = a != 0 && store.GetImage( a, ra ), gb = b != 0 && store.GetImage( b, rb );
      st.d["dupMaster"] = { { "a", a }, { "b", b }, { "aView", ra.viewId }, { "bView", rb.viewId } };
      st.ok["dupMaster"] = a != 0 && b != 0 && a != b && ga && gb && ra.viewId == "pcTrkDup1" && rb.viewId == "pcTrkDup2";
      J6Close( st, "pcTrkDup1" );   // made here (in-process); no JS wrapper holds them
      J6Close( st, "pcTrkDup2" );
      JTick( trk );
   }
   else if ( step == "joinFault" )
   {
      // Review I5: a failure in the middle of a join (the hook throws inside the transaction, like a lock
      // or a full disk) leaves no journey, image or step rows; after it clears exactly one join lands.
      int faults = 0;
      std::string where = "master";
      trk.SetJoinFaultForSelfTest( [&faults, &where]( const char* w )
      {
         if ( where == w )
         {
            ++faults;
            throw Error( String( "injected mid-join failure (" ) + w + ")" );
         }
      } );
      const int journeys0 = JourneyCount( store );
      JEvalJs( "(function(){ var w = new ImageWindow( 36, 36, 1, 32, true, false, \"pcTrkTxM\" ); })()" );
      JSetKeywords( "pcTrkTxM", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'TrkTx'" } } ) );
      JTick( trk, 3 );
      const int journeysFailed = JourneyCount( store ), rowsFailedM = ImageRowsOf( store, "pcTrkTxM" );
      const int faultsM = faults;
      where = "linked";
      JTick( trk );   // no fault on "master" now
      const int journeysOk = JourneyCount( store ), rowsOkM = ImageRowsOf( store, "pcTrkTxM" );
      // Linked: a Copilot-created window of the master's journey.
      faults = 0;
      trk.NoteCopilotStep( "pcTrkRenamed", "PixelMath", "tx test", { "pcTrkTxL" }, false, JourneyWallNow() );
      JEvalJs( "(function(){ var w = new ImageWindow( 36, 36, 1, 32, true, false, \"pcTrkTxL\" ); })()" );
      JTick( trk, 3 );
      const int rowsFailedL = ImageRowsOf( store, "pcTrkTxL" ), faultsL = faults;
      trk.SetJoinFaultForSelfTest( nullptr );
      JTick( trk );
      const int rowsOkL = ImageRowsOf( store, "pcTrkTxL" );
      st.d["joinAtomic"] = { { "faultsMaster", faultsM }, { "journeys0", journeys0 }, { "journeysFailed", journeysFailed },
                             { "rowsFailedM", rowsFailedM }, { "journeysOk", journeysOk }, { "rowsOkM", rowsOkM },
                             { "faultsLinked", faultsL }, { "rowsFailedL", rowsFailedL }, { "rowsOkL", rowsOkL },
                             { "linkedJourney", trk.JourneyOfView( "pcTrkTxL" ) } };
      st.ok["joinAtomic"] = faultsM >= 2 && journeysFailed == journeys0 && rowsFailedM == 0
                         && journeysOk == journeys0 + 1 && rowsOkM == 1 && trk.ImageOfView( "pcTrkTxM" ) != 0
                         && faultsL >= 2 && rowsFailedL == 0 && rowsOkL == 1 && trk.JourneyOfView( "pcTrkTxL" ) == jid;
      J6Close( st, "pcTrkTxM" );
      J6Close( st, "pcTrkTxL" );
      JTick( trk );
   }
   else if ( step == "startJourney" )
   {
      // Review I6: start_journey honours "recording off" (nothing read or stored), and survives a window
      // closed since the last tick (its dead View is never shifted), leaving no orphan journey.
      JEvalJs( "(function(){ new ImageWindow( 30, 30, 1, 32, true, false, \"pcTrkStart\" );"
               " new ImageWindow( 30, 30, 1, 32, true, false, \"pcTrkGone1\" );"
               " new ImageWindow( 30, 30, 1, 32, true, false, \"pcTrkGone2\" ); })()" );
      st.made.push_back( "pcTrkStart" );
      JTick( trk );   // all three become candidates
      const int journeys0 = JourneyCount( store );
      trk.SetEnabled( false );
      String offError;
      const int64 offJ = trk.StartJourneyFor( J6MainView( "pcTrkStart" ), offError, JourneyWallNow() );
      const int journeysOff = JourneyCount( store );
      trk.SetEnabled( true );
      J6Close( st, "pcTrkGone1" );   // closed since the last tick: a dead handle in the tracker's lists (m-3r)
      String onError;
      int64 onJ = 0;
      std::string threw;
      try
      {
         onJ = trk.StartJourneyFor( J6MainView( "pcTrkStart" ), onError, JourneyWallNow() );
      }
      catch ( const pcl::Exception& x ) { threw = U8( x.Message() ); }
      catch ( ... )                     { threw = "unknown exception"; }
      J6Close( st, "pcTrkGone2" );
      st.d["startJourney"] = { { "offJourney", offJ }, { "offError", U8( offError ) }, { "journeysOff", journeysOff - journeys0 },
                               { "onJourney", onJ }, { "onError", U8( onError ) }, { "threw", threw },
                               { "journeysOn", JourneyCount( store ) - journeys0 } };
      st.ok["startJourney"] = offJ == 0 && offError.Contains( "off" ) && journeysOff == journeys0
                           && onJ != 0 && onError.IsEmpty() && threw.empty() && JourneyCount( store ) == journeys0 + 1
                           && trk.JourneyOfView( "pcTrkStart" ) == onJ;
      JTick( trk );
   }
   else if ( step == "end" )
   {
      trk.SetScanUsesModifyCountForSelfTest( PICopilotJourneyScanUsesModifyCount && PICopilotJourneyNotificationsWork );
      st.ok["scanModes"] = st.scanModes && st.d.contains( "scanModes" ) && st.d["scanModes"].size() == 2;

      // (t) Retention trigger: once per local date (Ruling 9).
      std::string last;
      const int n1 = RunRetentionIfDue( store, 30, "2026-09-25", last, nullptr );
      const std::string afterFirst = last;
      const int n2 = RunRetentionIfDue( store, 30, "2026-09-25", last, nullptr );
      st.d["retention"] = { n1, afterFirst, n2, LocalDateToday() };
      st.ok["retention"] = n1 >= 0 && afterFirst == "2026-09-25" && n2 == -1 && LocalDateToday().size() == 10;

      // Review I1: a prune that fails (a journey folder cannot be removed) is recorded as run for that
      // date BEFORE it throws, so it is retried on the next date -- never on every tick.
      {
         const int64 old = store.CreateJourney( "old unkept", "Old", "2000-01-01T00:00:00.000Z" );
         {
            RawDb raw( store.DbPath() );
            if ( !raw.Exec( ( "UPDATE journey SET updated='2000-01-01T00:00:00.000Z' WHERE id=" + std::to_string( old ) ).c_str() ) )
               throw Error( "retention: backdating failed" );
         }
         const String thumbs = store.JourneyDir( old ) + "/thumbs";
         if ( !EnsurePrivateDirectory( store.JourneyDir( old ) ).IsEmpty() || !EnsurePrivateDirectory( thumbs ).IsEmpty() )
            throw Error( "retention: cannot make " + thumbs );
         File::WriteTextFile( thumbs + "/x.jpg", IsoString( "x" ) );
         ::chmod( U8( thumbs ).c_str(), 0500 );   // x.jpg cannot be unlinked
         std::string lastRun = "2026-09-26", msg;
         const bool threw = JThrowsWith( [&]() { RunRetentionIfDue( store, 30, "2026-09-27", lastRun, nullptr ); }, U8( thumbs ).c_str(), msg );
         const std::string afterFail = lastRun;
         bool threwAgain = false;
         int again = 0;
         for ( int i = 0; i < 3; ++i )   // the following "ticks" of the same date
            try { again += RunRetentionIfDue( store, 30, "2026-09-27", lastRun, nullptr ) == -1 ? 1 : 0; }
            catch ( ... ) { threwAgain = true; }
         ::chmod( U8( thumbs ).c_str(), 0700 );
         StringList removed;
         const int next = RunRetentionIfDue( store, 30, "2026-09-28", lastRun, &removed );
         JourneyRow gone;
         st.d["retentionOnce"] = { { "threw", threw }, { "msg", msg }, { "lastRunAfterFail", afterFail },
                                   { "sameDateNotDue", again }, { "threwAgain", threwAgain }, { "nextDatePruned", next } };
         st.ok["retentionOnce"] = threw && afterFail == "2026-09-27" && again == 3 && !threwAgain && next >= 1
                               && !store.GetJourney( old, gone ) && !File::DirectoryExists( store.JourneyDir( old ) );
      }

      // Review I5: the transaction API -- commit persists, an exception rolls back, nesting is refused.
      {
         const int n0 = JourneyCount( store );
         {
            JourneyStore::Transaction tx( store );
            store.CreateJourney( "tx commit", "Tx", NowIso() );
            tx.Commit();
         }
         const int afterCommit = JourneyCount( store );
         bool rolledBack = false;
         try
         {
            JourneyStore::Transaction tx( store );
            store.CreateJourney( "tx rollback", "Tx", NowIso() );
            throw Error( "abandon" );
         }
         catch ( const pcl::Exception& ) { rolledBack = JourneyCount( store ) == afterCommit; }
         std::string nestedMsg;
         bool nestedRefused = false, outerIntact = false;
         {
            JourneyStore::Transaction outer( store );
            nestedRefused = JThrowsWith( [&]() { JourneyStore::Transaction inner( store ); }, "already open", nestedMsg );
            store.CreateJourney( "tx outer", "Tx", NowIso() );
            outer.Commit();
            outerIntact = !store.InTransaction();
         }
         st.d["transaction"] = { { "committed", afterCommit - n0 }, { "rolledBack", rolledBack }, { "nested", nestedMsg },
                                 { "final", JourneyCount( store ) - n0 } };
         st.ok["transaction"] = afterCommit == n0 + 1 && rolledBack && nestedRefused && outerIntact && JourneyCount( store ) == n0 + 2;
      }

      // Re-review I-B: a journey whose image is recorded by a LIVE process is never pruned (the owner is read
      // before the cascading DELETE); a dead owner's journey is.
      {
         const int64 oj = store.CreateJourney( "owned old", "OwnedOld", NowIso() );
         const int64 oi = store.AddImage( oj, "pcTrkOwnedOld", "", "fp-owned-old", true, NowIso() );
         store.SetImageOwner( oi, JourneyOwnerOf() );
         {
            RawDb raw( store.DbPath() );
            if ( !raw.Exec( ( "UPDATE journey SET updated='" + IsoDaysAgo( 40 ) + "' WHERE id=" + std::to_string( oj ) ).c_str() ) )
               throw Error( "ownerPrune: backdating failed" );
         }
         StringList removed;
         const int n1 = store.PruneUnkept( IsoDaysAgo( 30 ), &removed );
         JourneyRow r1;
         const bool present1 = store.GetJourney( oj, r1 );
         store.SetImageOwner( oi, "999999999:1" );
         const int n2 = store.PruneUnkept( IsoDaysAgo( 30 ), &removed );
         JourneyRow r2;
         const bool present2 = store.GetJourney( oj, r2 );
         st.d["ownerPrune"] = { { "liveOwnerPruned", n1 }, { "presentWithLiveOwner", present1 }, { "deadOwnerPruned", n2 },
                                { "presentWithDeadOwner", present2 } };
         st.ok["ownerPrune"] = present1 && !present2 && n2 >= 1;
      }

      // Re-review m3: ResolveGaps removes only read-failure gaps; any other gap stays.
      {
         store.AddGap( { jid, mimg, 1, "closed before its last steps were recorded" } );
         store.AddGap( { jid, mimg, 1, std::string( PICopilotJourneyReadGapPrefix ) + "test" } );
         const int resolved = store.ResolveGaps( mimg, 1000000 );
         int left = 0;
         for ( const GapRow& g : store.Gaps( jid ) )
            if ( g.imageId == mimg && g.reason.find( "closed before" ) != std::string::npos )   // (the chaos may link others)
               ++left;
         nlohmann::json all = nlohmann::json::array();
         for ( const GapRow& g : store.Gaps( jid ) )
         {
            ImageRow ir;
            store.GetImage( g.imageId, ir );
            all.push_back( { g.imageId, ir.viewId, g.afterSeq, g.reason } );
         }
         st.d["gapKinds"] = { { "resolved", resolved }, { "closedLeft", left }, { "gaps", all } };
         st.ok["gapKinds"] = resolved == 1 && left == 1;
      }

      // Coordinator addition: every UPDATE/DELETE-by-id mutator fails loudly on a row that does not exist.
      {
         const int64 none = 987654321;
         std::string m1, m2, m3, m4, m5, m6, m7;
         const bool journey = JThrowsWith( [&]() { store.RenameJourney( none, "x" ); }, "no such row", m1 )
                           && JThrowsWith( [&]() { store.TouchJourney( none, NowIso() ); }, "no such row", m2 )
                           && JThrowsWith( [&]() { store.SetJourneyStatus( none, "ended" ); }, "no such row", m3 )
                           && JThrowsWith( [&]() { store.MarkKept( none, 0, NowIso() ); }, "no such row", m4 );
         const bool image = JThrowsWith( [&]() { store.SetImageView( none, "v", "" ); }, "no such row", m5 )
                         && JThrowsWith( [&]() { store.SetImageOwner( none, "x" ); }, "no such row", m5 );
         const bool stepF = JThrowsWith( [&]() { store.SetStepState( none, "active" ); }, "no such row", m6 )
                         && JThrowsWith( [&]() { store.SetStepReason( none, "r", false ); }, "no such row", m7 );
         // ... and still succeed on a row that exists.
         bool existing = true;
         try { store.TouchJourney( jid, NowIso() ); } catch ( ... ) { existing = false; }
         st.d["mutatorsLoud"] = { m1, m2, m3, m4, m5, m6, m7, existing };
         st.ok["mutatorsLoud"] = journey && image && stepF && existing;
      }

      // Task 5 re-review: ~JourneyStore off the root thread neither throws nor closes (a destructor
      // cannot throw; the connection is left open). Destroyed on a std::thread, then the file is
      // still a healthy database for a fresh root-thread connection.
      {
         JTempDir dtorRoot( "picopilot-trk-dtor-" );
         String de;
         std::unique_ptr<JourneyStore> other = JourneyStore::Open( dtorRoot.Path(), de );
         if ( !other )
            throw Error( "dtor store: " + de );
         other->CreateJourney( "dtor", "dtor", NowIso() );
         bool threw = false;
         std::thread t( [&other, &threw]() { try { other.reset(); } catch ( ... ) { threw = true; } } );
         t.join();
         String re;
         std::unique_ptr<JourneyStore> again = JourneyStore::Open( dtorRoot.Path(), re );
         const size_t n = again ? again->ListJourneys( false, "", 5 ).size() : 0;
         st.d["offRootDtor"] = { { "threw", threw }, { "reopenError", U8( re ) }, { "journeys", n } };
         st.ok["offRootDtor"] = !threw && !other && again && n == 1;
      }

      // Location data (SITELAT on the II master) never reached the library.
      store.Checkpoint();
      st.ok["redact"] = FileBytes( store.DbPath() ).find( "40 11 12" ) == std::string::npos
                     && FileBytes( store.DbPath() + "-wal" ).find( "40 11 12" ) == std::string::npos;
   }
   else
      throw Error( String( "unknown step " ) + step.c_str() );
}

// j6: {step, ...} -- one step of Section J6's tracker between top-level steps.
// A failing step is recorded (and fails J6), never thrown into the harness.
nlohmann::json PhaseJourneyTracker( const nlohmann::json& payload )
{
   J6State& st = J6();
   const std::string step = payload.at( "step" ).get<std::string>();
   st.steps.push_back( step );
   try
   {
      J6Step( st, step, payload );
   }
   catch ( const pcl::Exception& x ) { st.errors << String( step.c_str() ) + ": " + x.Message(); }
   catch ( const std::exception& x ) { st.errors << String( step.c_str() ) + ": " + String( x.what() ); }
   catch ( ... )                     { st.errors << String( step.c_str() ) + ": unknown exception"; }
   // Diagnostics: every "closed with unrecorded changes" gap the tracker queued, with the step it happened in.
   if ( st.trk )
      for ( const std::string& x : st.trk->RecentDecisionsForSelfTest() )
         if ( x.rfind( "gap: ", 0 ) == 0 && st.gapLogSeen.insert( x ).second )
            st.d["gapLog"].push_back( step + ": " + x );
   return { { "step", step } };
}

// ---- Section J7 (JourneyExport, Task 8) fixture: phase j7.exp ----
// pcExpM's journey is recorded here, step by step, from REAL top-level history
// (test/selftest.js runs the three steps; in-process steps are never recorded,
// Task 1). The handler does what the journey tracker does for a keyword master
// -- base history, acquisition, starting stats + thumbnail, then one row +
// stats + thumbnail per new step -- straight through the Task 3-6 units, so
// Section J7 depends on nothing but the store it exports from.
struct J7State
{
   std::unique_ptr<JTempDir>     root;    // the test library (removed by Section J7)
   std::unique_ptr<JourneyStore> store;
   int64                         jid = 0;
   int64                         mimg = 0;
   int                           known = 0;      // history entries already stored (base + recorded)
   int                           recorded = 0;   // steps recorded after the image joined
   std::string                   error;          // the first failure, verbatim
   nlohmann::json                detail = nlohmann::json::array();
};

// Runs one call on a PCL worker thread (Thread::IsRootThread() is false there;
// a std::thread is invisible to PCL and counts as root) and captures what it threw.
class J7CallThread : public Thread
{
public:

   explicit J7CallThread( std::function<void()> fn ) : m_fn( std::move( fn ) ) {}

   void Run() override
   {
      try { m_fn(); }
      catch ( const pcl::Exception& x ) { threw = true; error = x.Message(); }
      catch ( const std::exception& x ) { threw = true; error = String( x.what() ); }
      catch ( ... )                     { threw = true; error = "unknown exception"; }
   }

   bool   threw = false;
   String error;

private:

   std::function<void()> m_fn;
};

J7State& J7()
{
   static J7State s;
   return s;
}

void J7Stats( J7State& st, int64 stepId, const String& thumbName )
{
   const View v = ImageWindow::WindowById( "pcExpM" ).MainView();
   const StepStatsResult r = ComputeStepStats( v, st.store->JourneyDir( st.jid ) + "/thumbs/" + thumbName );
   if ( !r.ok )
      throw Error( "j7.exp stats: " + r.error );
   if ( r.thumbnailPath.IsEmpty() )
      throw Error( "j7.exp thumbnail: " + r.thumbnailError );
   st.store->AddStats( st.mimg, stepId, r.channels );
}

nlohmann::json PhaseJourneyExport( const nlohmann::json& payload )
{
   J7State& st = J7();
   const std::string step = payload.at( "step" ).get<std::string>();
   try
   {
      if ( step == "make" )
      {
         JMakeNoiseMaster( "pcExpM", 31 );
         const FITSKeywordArray kw = Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'ExpM42'" }, { "FILTER", "'Ha'" },
                                           { "EXPTIME", "300" }, { "NCOMBINE", "20" }, { "SITELAT", "'+40 11 12'" } } );
         ImageWindow::WindowById( "pcExpM" ).SetKeywords( kw );
         st.detail.push_back( { { "step", step } } );
      }
      else if ( step == "join" )
      {
         // A separate phase: the window's creation entry (process "PICopilot", the executeGlobal
         // that made it) lands in its history only after that phase has returned. Measured:
         // read inside "make" the history was empty, and the first top-level step then found 2.
         const FITSKeywordArray kw = ImageWindow::WindowById( "pcExpM" ).Keywords();
         st.root.reset( new JTempDir( "picopilot-exp-" ) );
         String oe;
         st.store = JourneyStore::Open( st.root->Path(), oe );
         if ( !st.store )
            throw Error( "store: " + oe );
         const HistorySnapshot base = ReadViewHistory( "pcExpM", 0 );
         if ( !base.ok )
            throw Error( "base history: " + (base.busy ? String( "busy" ) : base.error) );
         std::vector<std::string> ids;
         for ( const HistoryStep& h : base.steps )
            ids.push_back( h.processId );
         const MasterEvidence me = DetectMaster( ids, kw );
         if ( !me.isMaster )
            throw Error( "pcExpM is not detected as a master" );
         const std::string now = NowIso();
         const std::string target = DeriveTarget( kw, String(), "pcExpM" );
         const AcquisitionFacts a = ExtractAcquisition( kw, base.steps, String(), "pcExpM" );
         st.jid = st.store->CreateJourney( DeriveJourneyName( target, a.filter, 1, now ), target, now );
         st.mimg = st.store->AddImage( st.jid, "pcExpM", "", MasterFingerprint( 96, 64, 1, 32, true, {}, kw ), true, now );
         st.store->SetAcquisition( st.mimg, a );
         for ( const HistoryStep& h : base.steps )
         {
            StepRow r = MakeStepRow( h, st.mimg, "active", "user", "", base.ActiveCount() );
            r.params["base"] = true;   // pre-join history (the tracker's rule)
            st.store->AddStep( r );
         }
         st.known = base.TotalCount();
         J7Stats( st, 0, String().Format( "start-%lld.jpg", static_cast<long long>( st.mimg ) ) );
         st.detail.push_back( { { "step", step }, { "base", ids }, { "target", target } } );
      }
      else if ( step == "record" )
      {
         if ( !st.store )
            throw Error( "record before make" );
         const HistorySnapshot s = ReadViewHistory( "pcExpM", st.known );
         if ( !s.ok )
            throw Error( "history: " + (s.busy ? String( "busy" ) : s.error) );
         if ( s.TotalCount() != st.known + 1 || s.steps.size() != 1 )
         {
            String got;
            for ( const HistoryStep& h : s.steps )
               got += (got.IsEmpty() ? "" : ",") + FromU8( h.processId );
            throw Error( String().Format( "expected exactly one new step, history has %d entries (known %d, initial %d): ",
                                          s.TotalCount(), st.known, s.initialLength ) + got );
         }
         const int64 sid = st.store->AddStep( MakeStepRow( s.steps[0], st.mimg, "active", "user", "", s.ActiveCount() ) );
         st.known = s.TotalCount();
         ++st.recorded;
         J7Stats( st, sid, String().Format( "%lld.jpg", static_cast<long long>( sid ) ) );
         st.detail.push_back( { { "step", step }, { "processId", s.steps[0].processId }, { "seq", s.steps[0].combinedIndex + 1 } } );
      }
      else
         throw Error( String( "j7.exp: unknown step " ) + step.c_str() );
   }
   catch ( const pcl::Exception& x )
   {
      if ( st.error.empty() )
         st.error = step + ": " + U8( x.Message() );
      throw;
   }
   return { { "step", step } };
}

// ---- Section J8 (keeper write-up, Task 9) helpers ----

struct JStepSpec
{
   const char* expression;   // a PixelMath expression, applied to the pixels
   const char* actor;        // "user" | "copilot"
   const char* reason;       // "" = none stated
};

struct JBuilt
{
   int64 jid = 0;
   int64 img = 0;   // the master image (the journey's end image)
};

// Records the view's statistics (+ thumbnail) for `stepId` (0 = starting stats).
void JRecordStats( JourneyStore& store, const JBuilt& b, const char* id, int64 stepId, const String& thumbName )
{
   const StepStatsResult r = ComputeStepStats( ImageWindow::WindowById( IsoString( id ) ).MainView(),
                                               store.JourneyDir( b.jid ) + "/thumbs/" + thumbName );
   if ( !r.ok )
      throw Error( String( "JRecordStats " ) + id + ": " + r.error );
   if ( r.thumbnailPath.IsEmpty() )
      throw Error( String( "JRecordStats " ) + id + " thumbnail: " + r.thumbnailError );
   store.AddStats( b.img, stepId, r.channels );
}

// A recorded journey on `store`: a 96x64 noise master `id` with keywords `kw`,
// then each step applied to its pixels (ApplyProcess) and stored the way the
// journey tracker stores it: a step row from the step's XPSM, stats and a
// thumbnail. Task 7's JourneyTracker is not on this branch's base, so the rows
// are made straight from the Task 3-6 units (as J7's fixture does); what the
// write-up reads -- the store -- is the same either way.
JBuilt JRecordJourney( JourneyStore& store, const char* id, const FITSKeywordArray& kw, unsigned seed,
                       std::initializer_list<JStepSpec> steps )
{
   JMakeNoiseMaster( id, seed );
   ImageWindow::WindowById( IsoString( id ) ).SetKeywords( kw );
   const std::string now = NowIso();
   const std::string target = DeriveTarget( kw, String(), id );
   const AcquisitionFacts a = ExtractAcquisition( kw, {}, String(), id );
   JBuilt b;
   b.jid = store.CreateJourney( DeriveJourneyName( target, a.filter, 1, now ), target, now );
   b.img = store.AddImage( b.jid, id, "", MasterFingerprint( 96, 64, 1, 32, true, {}, kw ), true, now );
   store.SetAcquisition( b.img, a );
   JRecordStats( store, b, id, 0, String().Format( "start-%lld.jpg", static_cast<long long>( b.img ) ) );
   int seq = 0;
   for ( const JStepSpec& s : steps )
   {
      std::string x = kXpsmPixelMath;
      const std::string from = "<parameter id=\"expression\">$T*2</parameter>";
      x.replace( x.find( from ), from.size(), std::string( "<parameter id=\"expression\">" ) + s.expression + "</parameter>" );
      HistoryStep h;
      String e;
      if ( !ParseXpsmStep( x, h, e ) )
         throw Error( "JRecordJourney: " + e );
      const ApplyProcessResult ar = ApplyProcess( "PixelMath", h.parameters, h.tableParameters,
                                                  ImageWindow::WindowById( IsoString( id ) ).MainView() );
      if ( !ar.ok )
         throw Error( "JRecordJourney apply: " + ar.error );
      h.combinedIndex = seq;
      const int64 sid = store.AddStep( MakeStepRow( h, b.img, "active", s.actor, s.reason, seq + 1 ) );
      JRecordStats( store, b, id, sid, String().Format( "%lld.jpg", static_cast<long long>( sid ) ) );
      ++seq;
   }
   return b;
}

// The plan's J8 journey: master + 2 user PixelMath steps (no reason) + 1 Copilot step.
JBuilt JBuildJourney( JourneyStore& store, const char* id, const char* object, unsigned seed )
{
   const std::string obj = std::string( "'" ) + object + "'";
   return JRecordJourney( store, id,
                          Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", obj.c_str() }, { "FILTER", "'Ha'" },
                                { "INSTRUME", "'ASI2400MC'" }, { "EXPTIME", "300" }, { "NCOMBINE", "20" },
                                { "DATE-OBS", "'2026-09-20T03:04:05'" } } ),
                          seed, { { "$T*1.3", "user", "" }, { "$T+0.01", "user", "" }, { "$T*1.1", "copilot", "stretch gently" } } );
}

// RFC 3986 percent-encoding of a query value (the loopback's ?reasons=).
std::string JPctEncode( const std::string& s )
{
   static const char* hex = "0123456789ABCDEF";
   std::string o;
   for ( unsigned char c : s )
      if ( std::isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~' )
         o += char( c );
      else
      {
         o += '%';
         o += hex[c >> 4];
         o += hex[c & 15];
      }
   return o;
}

// Polls a KeeperExporter until idle (root thread; the worker only POSTs).
bool JWaitKeeper( KeeperExporter& k, StringList& notes, int seconds )
{
   const jclock::time_point t0 = jclock::now();
   while ( k.Busy() && MsSince( t0 ) < seconds*1000.0 )
   {
      k.Poll( notes );
      JPump( 100 );
   }
   k.Poll( notes );
   return !k.Busy();
}

// ---- Section J10 (journey tools, Task 10) helpers ----
//
// A process run inside the self-test's executeGlobal() is never recorded in
// History (harness fact, global constraints), so J10's tracker reads the
// History of its fixture windows from here: the steps a real top-level run
// would have left. Every other window's History is read for real.
struct JFakeHistory
{
   std::map<std::string, std::vector<HistoryStep>> steps;
   mutable int reads = 0;   // every read through this reader (fixture windows and real ones)

   HistorySnapshot Read( const IsoString& viewId, int from ) const
   {
      ++reads;
      const auto it = steps.find( std::string( viewId.c_str() ) );
      if ( it == steps.end() )
         return ReadViewHistory( viewId, from );
      HistorySnapshot s;
      s.ok = true;
      s.initialLength = 0;
      s.length = s.historyIndex = int( it->second.size() );
      s.from = std::max( 0, std::min( from, s.length ) );
      for ( const HistoryStep& h : it->second )
         if ( h.combinedIndex >= s.from )
            s.steps.push_back( h );
      return s;
   }
};

// A PixelMath History step with this expression, as PixInsight writes it
// (kXpsmPixelMath with the expression and the start time replaced).
HistoryStep JPixelMathStep( const std::string& expression, int combinedIndex, const std::string& started )
{
   std::string x = kXpsmPixelMath;
   const std::string from = "<parameter id=\"expression\">$T*2</parameter>";
   x.replace( x.find( from ), from.size(), "<parameter id=\"expression\">" + expression + "</parameter>" );
   const std::string t0 = "2026-09-25T20:47:50.344Z";
   x.replace( x.find( t0 ), t0.size(), started );
   HistoryStep h;
   String e;
   if ( !ParseXpsmStep( x, h, e ) )
      throw Error( "JPixelMathStep: " + e );
   h.combinedIndex = combinedIndex;
   return h;
}

// The History matching an image's recorded rows (JRecordJourney's steps).
std::vector<HistoryStep> JHistoryOfImage( JourneyStore& store, int64 imageId )
{
   std::vector<HistoryStep> hs;
   for ( const StepRow& r : store.Steps( imageId, false ) )
   {
      HistoryStep h;
      String e;
      if ( !ParseXpsmStep( r.params.value( "xpsm", std::string() ), h, e ) )
         throw Error( "JHistoryOfImage: " + e );
      h.identity = r.params.value( "identity", std::string() );
      h.combinedIndex = r.seq - 1;
      hs.push_back( h );
   }
   return hs;
}

// The real-tool-path successful apply_process results on `viewId`, in order: {median of channel 0 after it}.
double JApplyMedian( const ToolOutcome& o )
{
   const nlohmann::json s = nlohmann::json::parse( o.content.at( 0 ).at( "text" ).get<std::string>() );
   return s.at( "newContext" ).at( "channelStats" ).at( 0 ).at( "median" ).get<double>();
}

// A realistic kept journey whose replay material is far over one tool result
// (review I2): a DynamicBackgroundExtraction step with a 400-sample table
// (manual) and `curves` CurvesTransformation steps with 400-point K curves.
// Rows only (no window): replay_journey reads the store.
int64 JAddBigKeeper( JourneyStore& store, int curves )
{
   const std::string now = NowIso();
   const int64 jid = store.CreateJourney( "JtBig Ha 2026-09-27", "JtBig", now );
   const int64 img = store.AddImage( jid, "pcJtBigM", "", "big-fp", true, now );
   auto table = []( int rows, int cols, unsigned seed )
   {
      nlohmann::json t = nlohmann::json::array();
      uint32_t x = 2463534242u ^ seed;
      for ( int r = 0; r < rows; ++r )
      {
         nlohmann::json row = nlohmann::json::array();
         for ( int c = 0; c < cols; ++c )
         {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            row.push_back( double( x % 100000000u )/1e8 );
         }
         t.push_back( row );
      }
      return t;
   };
   int seq = 0;
   auto add = [&]( const char* pid, const nlohmann::json& tables )
   {
      StepRow r;
      r.imageId = img;
      r.seq = ++seq;
      r.processId = pid;
      r.state = "active";
      r.historyIndex = seq;
      r.params = { { "parameters", nlohmann::json::object() }, { "tableParameters", tables }, { "xpsm", "" },
                   { "identity", std::string( pid ) + "@big#" + std::to_string( seq ) }, { "mask", nullptr },
                   { "replayable", true }, { "parseNote", "" } };
      store.AddStep( r );
   };
   add( "DynamicBackgroundExtraction", { { "data", table( 400, 10, 7 ) } } );
   for ( int i = 0; i < curves; ++i )
      add( "CurvesTransformation", { { "K", table( 400, 2, 100 + i ) } } );
   store.MarkKept( jid, img, NowIso() );
   return jid;
}

} // namespace

bool RunPendingSelfTestPhase()
{
   const char* phasePath = std::getenv( "PICOPILOT_SELFTEST_PHASE" );
   if ( phasePath == nullptr || *phasePath == '\0' || !File::Exists( String( phasePath ) ) )
      return false;
   nlohmann::json& store = SelfTestPhaseStore();
   std::string id = "?";
   try
   {
      const nlohmann::json f = nlohmann::json::parse( File::ReadTextFile( String( phasePath ) ).c_str() );
      id = f.at( "phase" ).get<std::string>();
      const nlohmann::json payload = f.value( "payload", nlohmann::json::object() );
      if ( id == "harness.error" )
         store["errors"].push_back( payload );
      else
      {
         const auto h = SelfTestPhaseHandlers().find( id );
         if ( h == SelfTestPhaseHandlers().end() )
            store["errors"].push_back( { { "phase", id }, { "error", "no handler for this phase id" } } );
         else
            store[id].push_back( h->second( payload ) );
      }
   }
   catch ( const pcl::Exception& x ) { store["errors"].push_back( { { "phase", id }, { "error", U8( x.Message() ) } } ); }
   catch ( const std::exception& x ) { store["errors"].push_back( { { "phase", id }, { "error", x.what() } } ); }
   catch ( ... )                     { store["errors"].push_back( { { "phase", id }, { "error", "unknown exception" } } ); }
   return true;
}

bool RunJourneySelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section J0: notification spike + platform measurements (Task 1) ----
   SelfTestSectionMark( "J0 notification spike + platform measurements" );
   {
      nlohmann::json info = nlohmann::json::object();
      String error;
      bool preOk = false, notifyDecided = false, timerOk = false, nestedOk = false, mcOk = false,
           identityOk = false, timerApplyOk = false, xpsmOk = false, historyCostOk = false, reopenOk = false, statsCostOk = false, iiOk = false;
      std::vector<std::string> made;
      try
      {
         // (1) The pre-phase ran (selftest.js) and did every action.
         const char* prePath = std::getenv( "PICOPILOT_SELFTEST_PRE" );
         if ( prePath != nullptr && File::Exists( String( prePath ) ) )
         {
            const nlohmann::json pre = nlohmann::json::parse( File::ReadTextFile( String( prePath ) ).c_str() );
            info["pre"] = pre;
            preOk = !pre.contains( "error" ) && pre.value( "steps", nlohmann::json::array() ).size() == 5;
         }
         else
            info["pre"] = "missing: PICOPILOT_SELFTEST_PRE not set or not written";

         // (2) What reached the NEVER-OPENED interface during the pre-phase.
         const nlohmann::json probe = JourneySpikeProbeReport();
         int created = 0, createdNew = 0, updated = 0;
         for ( const nlohmann::json& e : probe.at( "events" ) )
         {
            const std::string kind = e.at( 0 ), id = e.at( 1 );
            if ( kind == "created" && id == "pcJourneyPre" ) ++created;
            if ( kind == "created" && id == "pcJourneyPreNew" ) ++createdNew;
            if ( kind == "updated" && id == "pcJourneyPre" ) ++updated;
         }
         info["closedCreatedCount"] = created;
         info["closedCreatedNewCount"] = createdNew;
         info["closedUpdatedCount"] = updated;
         info["closedCreated"] = created >= 1 && createdNew >= 1;
         info["closedUpdated"] = updated >= 4;     // pm1, pm2, undo, redo
         int focused = 0, activeSamples = 0;
         for ( const nlohmann::json& e : probe.at( "events" ) )
            if ( e.at( 0 ) == "focused" && e.at( 1 ) == "pcJourneyPre" ) ++focused;
         for ( const nlohmann::json& smp : probe.at( "nestedEvalSamples" ) )
            if ( smp.size() > 3 && smp.at( 3 ) == "pcJourneyPre" ) ++activeSamples;
         info["closedFocused"] = focused;
         info["activeWindowSamplesNamingPre"] = activeSamples;   // ActiveWindow() headlessly follows show()
         info["probeTicks"] = probe.at( "ticks" );
         info["timerFromOnLoad"] = probe.at( "timerCreated" ).get<bool>() && probe.at( "ticks" ).get<int>() >= 5;
         info["timerError"] = probe.at( "timerError" );
         info["nestedEval"] = { { "ok", probe.at( "nestedEvalOk" ) }, { "fail", probe.at( "nestedEvalFail" ) },
                                { "lastError", probe.at( "nestedEvalLastError" ) },
                                { "samples", probe.at( "nestedEvalSamples" ) } };
         timerOk = info["timerFromOnLoad"].get<bool>();
         nestedOk = probe.at( "nestedEvalOk" ).get<int>() >= 1 && probe.at( "nestedEvalFail" ).get<int>() == 0;
         const bool notifications = info["closedCreated"].get<bool>() && info["closedUpdated"].get<bool>();
         // BLOCKED rule (Ruling 22): nothing could record with the panel closed.
         notifyDecided = notifications || timerOk;
         info["d6"] = notifications ? "notifications first + backstop scan" : "scan carries capture";

         // (2b) Does the timer's nested EvaluateScript (it returns pcJourneyPre's
         //      historyIndex) clobber the result of an OUTER EvaluateScript that
         //      pumps events? The outer returns a sentinel string after ~1.5 s.
         {
            // Paused since the end of the pre-phase (phase probe.nestedEval);
            // live only for this measurement.
            JourneySpikeProbeSetNestedEval( true );
            const int ticks0 = JourneySpikeProbeReport().at( "ticks" ).get<int>();
            const String r = JEvalJs( "(function(){ var t0 = Date.now(); while ( Date.now() - t0 < 1500 ) { processEvents(); msleep( 20 ); }"
                                      " return \"outer-sentinel\"; })()" );
            const int ticks = JourneySpikeProbeReport().at( "ticks" ).get<int>() - ticks0;
            info["nestedEvalOuter"] = { { "returned", U8( r ) }, { "ticksDuring", ticks },
                                        { "clobbered", r != "outer-sentinel" } };
            JourneySpikeProbeSetNestedEval( false );
         }

         // (3) Launched-but-hidden interface (informational; the pre-phase is the gate).
         {
            bool dynamic = false;
            unsigned flags = 0;
            ThePICopilotInterface->Launch( *ThePICopilotProcess, nullptr, dynamic, flags );
            ThePICopilotInterface->Hide();
            JourneySpikeProbeClearEvents();
            JEvalJs( "(function(){ var w = new ImageWindow( 16, 16, 1, 32, true, false, \"pcSpikeHidden\" );"
                     " var p = new PixelMath; p.expression = \"0.3\"; p.executeOn( w.mainView ); })()" );
            JPump( 1000 );
            int hc = 0, hu = 0;
            const nlohmann::json report = JourneySpikeProbeReport();   // bound first: a range-for over a member
            for ( const nlohmann::json& e : report.at( "events" ) )   // of a temporary iterates a destroyed object
            {
               if ( e.at( 1 ) == "pcSpikeHidden" && e.at( 0 ) == "created" ) ++hc;
               if ( e.at( 1 ) == "pcSpikeHidden" && e.at( 0 ) == "updated" ) ++hu;
            }
            info["hiddenCreated"] = hc;
            info["hiddenUpdated"] = hu;
            made.push_back( "pcSpikeHidden" );
         }

         // Every history-bearing fixture below was made at TOP LEVEL by
         // test/selftest.js's J0 fixture phases: a step executed while this
         // process runs is never recorded in History (measured in Task 1), so
         // the in-process code only READS history.
         const nlohmann::json& phases = SelfTestPhaseStore();
         for ( const char* id : { "pcSpikeMC", "pcSpikeMCShown", "pcSpikeLong", "pcSpikeReopened",
                                  "pcSpikeIdSrc", "pcSpikeIdNew", "pcSpikeRGB" } )
            made.push_back( id );

         // (4) ModifyCount across step / undo / redo, read by phase j0.mc
         //     between top-level steps, on a never-shown window (pcSpikeMC, also
         //     used by (6)) and a shown one (pcSpikeMCShown).
         {
            nlohmann::json seq = nlohmann::json::array(), seqShown = nlohmann::json::array(), labels = nlohmann::json::array();
            if ( phases.contains( "j0.mc" ) )
               for ( const nlohmann::json& r : phases.at( "j0.mc" ) )
               {
                  labels.push_back( r.at( "label" ) );
                  seq.push_back( r.at( "counts" ).at( "pcSpikeMC" ) );
                  seqShown.push_back( r.at( "counts" ).at( "pcSpikeMCShown" ) );
               }
            auto tracks = []( const nlohmann::json& m )
            {
               return m.size() == 4 && m[1] != m[0] && m[2] != m[1] && m[3] != m[2];
            };
            info["modifyCount"] = { { "labels", labels }, { "sequence", seq }, { "sequenceShown", seqShown },
                                    { "tracksUndoHidden", tracks( seq ) }, { "tracksUndoShown", tracks( seqShown ) },
                                    { "tracksUndo", tracks( seq ) && tracks( seqShown ) } };
            mcOk = labels == nlohmann::json{ "start", "step", "undo", "redo" };   // the constant follows the value
         }

         // (4b) Production ApplyProcess from a module Timer tick (the panel's
         //      execution context) on a top-level window: recorded in History?
         //      Phases j0.timerApply.arm / .check (review fix #1).
         made.push_back( "pcSpikeTimerApply" );
         if ( phases.contains( "j0.timerApply.check" ) && phases.at( "j0.timerApply.check" ).size() == 1 )
         {
            const nlohmann::json& ta = phases.at( "j0.timerApply.check" ).at( 0 );
            info["timerApply"] = ta;
            const nlohmann::json& h = ta.at( "history" );
            const nlohmann::json& t = ta.at( "tick" );
            const bool ran = t.is_object() && t.value( "ok", false );
            const bool recorded = ran && h.at( "lengthAfter" ).get<int>() == h.at( "lengthBefore" ).get<int>() + 1
                               && h.at( "lastProcessId" ) == "PixelMath" && h.at( "lastHasExpression" ).get<bool>();
            const bool mcAdvanced = ran && t.at( "modifyCountAfter" ) != t.at( "modifyCountBefore" );
            info["timerApplyRecordsHistory"] = recorded;
            info["timerApplyModifyCountAdvanced"] = mcAdvanced;
            // A product fact the journey depends on (Copilot steps are undoable
            // and trackable): the gate requires it.
            timerApplyOk = recorded && mcAdvanced;
         }

         // (5) Created-window identity (PixelMath createNewImage, ChannelExtraction)
         //     and Ruling 28 (P14) keyword inheritance, measured at top level by
         //     phase j0.identity.
         if ( phases.contains( "j0.identity" ) && phases.at( "j0.identity" ).size() == 1 )
         {
            info["createdIdentity"] = phases.at( "j0.identity" ).at( 0 );
            info["keywordsInheritedPixelMath"] = info["createdIdentity"]["keywordsInheritedPixelMath"];
            info["keywordsInheritedChannelExtraction"] = info["createdIdentity"]["keywordsInheritedChannelExtraction"];
            for ( const nlohmann::json& p : info["createdIdentity"]["channelExtraction"]["windows"] )
               made.push_back( p.at( "id" ) );
            identityOk = !info["createdIdentity"].contains( "error" );   // measured either way
         }

         // (6) .xpsm built the way JourneyExport will, parsed back by pcl::XMLDocument.
         {
            JTempDir dir( "picopilot-spike-" );
            const String text = JEvalJs(
               "(function(){ var v = View.viewById( \"pcSpikeMC\" );"
               " return '<?xml version=\"1.0\" encoding=\"UTF-8\"?>\\n<xpsm version=\"1.0\" xmlns=\"http://www.pixinsight.com/xpsm\""
               " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://www.pixinsight.com/xpsm"
               " http://pixinsight.com/xpsm/xpsm-1.0.xsd\">\\n' + v.processing.toSource( \"XPSM 1.0\" )"
               " + '\\n<icon id=\"pcSpike\" instance=\"ProcessContainer_instance\" xpos=\"8\" ypos=\"8\" workspace=\"Workspace01\"/>\\n</xpsm>\\n'; })()" );
            const String path = dir.Path() + "/spike.xpsm";
            File::WriteTextFile( path, IsoString( U8( text ).c_str() ) );
            XMLDocument doc;
            doc.SetParserOption( XMLParserOption::IgnoreComments );
            doc.Parse( FromU8( std::string( File::ReadTextFile( path ).c_str() ) ) );
            const XMLElement* root = doc.RootElement();
            int containers = 0, instances = 0, icons = 0;
            if ( root != nullptr )
               for ( const XMLElement& e : root->ChildElements() )
               {
                  if ( e.Name() == "instance" && e.AttributeValue( "class" ) == "ProcessContainer" )
                  {
                     ++containers;
                     for ( const XMLElement& i : e.ChildElements() )
                        if ( i.Name() == "instance" )
                           ++instances;
                  }
                  if ( e.Name() == "icon" )
                     ++icons;
               }
            info["xpsm"] = { { "root", root != nullptr ? U8( root->Name() ) : std::string() },
                             { "containers", containers }, { "instances", instances }, { "icons", icons } };
            xpsmOk = root != nullptr && root->Name() == "xpsm" && containers == 1 && instances >= 1 && icons == 1;
         }

         // (7) 500-step top-level history (pcSpikeLong): full per-step read, tail read, C++ parse of every step.
         {
            const char* readJs =
               "(function( from ){ var v = View.viewById( \"pcSpikeLong\" ); var ip = v.initialProcessing, p = v.processing;"
               " var r = { initialLength: ip.length, length: p.length, historyIndex: v.historyIndex, steps: [] };"
               " for ( var c = from; c < ip.length + p.length; ++c ) { var pc = c < ip.length ? ip : p, i = c < ip.length ? c : c - ip.length;"
               "   r.steps.push( pc.at( i ).toSource( \"XPSM 1.0\" ) ); }"
               " return JSON.stringify( r ); })";
            jclock::time_point t0 = jclock::now();
            const String all = JEvalJs( String( readJs ) + "( 0 )" );
            const double readAllMs = MsSince( t0 );
            t0 = jclock::now();
            const String tail = JEvalJs( String( readJs ) + "( 499 )" );
            const double readTailMs = MsSince( t0 );
            const nlohmann::json allJ = nlohmann::json::parse( U8( all ) );
            t0 = jclock::now();
            int parsed = 0;
            for ( const nlohmann::json& s : allJ.at( "steps" ) )
            {
               XMLDocument d;
               d.Parse( FromU8( s.get<std::string>() ) );
               if ( d.RootElement() != nullptr && d.RootElement()->Name() == "instance" )
                  ++parsed;
            }
            const double parseAllMs = MsSince( t0 );
            info["history500"] = { { "readAllMs", readAllMs }, { "readTailMs", readTailMs }, { "parseAllMs", parseAllMs },
                                   { "chars", all.Length() }, { "steps", allJ.at( "steps" ).size() }, { "parsed", parsed },
                                   { "length", allJ.at( "length" ) } };
            historyCostOk = allJ.at( "length" ) == 500 && parsed >= 500 && allJ.at( "steps" ).size() >= 500;
         }

         // (7b) Save + reopen count (Ruling 27, pre-flight P15), measured at top
         //      level by phase j0.reopen: a 5-step history saved as XISF and
         //      reopened; the extra entry, if any, found by its normalized XPSM
         //      against every saved entry (initialProcessing + processing).
         if ( phases.contains( "j0.reopen" ) && phases.at( "j0.reopen" ).size() == 1 )
         {
            const nlohmann::json& ro = phases.at( "j0.reopen" ).at( 0 );
            info["reopen"] = ro;
            if ( !ro.contains( "error" ) )
            {
               info["reopenSavedLength"] = ro.at( "savedLength" );
               info["reopenInitialLength"] = ro.at( "initialLength" );
               info["reopenExtraProcessId"] = ro.at( "extraProcessId" );
               info["reopenExtraLeads"] = ro.at( "extraLeads" );
               const int delta = ro.at( "initialLength" ).get<int>() - ro.at( "savedLength" ).get<int>();
               // Decidable iff delta is 0, or +1 with exactly one unmatched entry (Ruling 27).
               // lengthBeforeRename: the harness's own `view.id = ...` rename is
               // itself recorded (an ImageIdentifier step, measured in Task 1).
               reopenOk = ro.at( "savedProcessingLength" ).get<int>() == 5 && ro.at( "lengthBeforeRename" ).get<int>() == 0
                       && ((delta == 0 && ro.at( "unmatched" ).get<int>() == 0)
                        || (delta == 1 && ro.at( "unmatched" ).get<int>() == 1));
            }
         }

         // (8) 60 MP RGB float: block-average cost by row stride, and the whole preview path.
         {
            JWindow big( "pcSpike60", 9504, 6336, 3, 0.1 );
            View v = big.MainView();
            nlohmann::json byStride = nlohmann::json::object();
            for ( int stride : { 1, 2, 4 } )
               byStride[std::to_string( stride )] = TimeBlockAverage( v, stride );
            const jclock::time_point t0 = jclock::now();
            const ViewPreviewResult p = RenderViewPreview( v );
            info["stats60"] = { { "blockMsByStride", byStride }, { "previewMs", MsSince( t0 ) },
                                { "previewOk", p.ok }, { "blockFactor", p.blockFactor } };
            statsCostOk = p.ok;
         }

         // (9) ImageIntegration result: history and keywords (Ruling 1), and with
         //     rejection maps on, the auxiliary windows' histories (Ruling 29, P35).
         {
            JTempDir dir( "picopilot-spike-ii-" );
            nlohmann::json rows = nlohmann::json::array();
            for ( int i = 0; i < 3; ++i )
            {
               Image img( 64, 64, ColorSpace::Gray );
               JFillNoise( img, 0.1, 0.01, unsigned( i + 1 ) );
               FITSKeywordArray kw;
               kw << FITSHeaderKeyword( "IMAGETYP", "'Light Frame'", "" ) << FITSHeaderKeyword( "OBJECT", "'SpikeM31'", "" )
                  << FITSHeaderKeyword( "FILTER", "'Ha'", "" ) << FITSHeaderKeyword( "INSTRUME", "'SpikeCam'", "" )
                  << FITSHeaderKeyword( "EXPTIME", "300", "" ) << FITSHeaderKeyword( "GAIN", "100", "" )
                  << FITSHeaderKeyword( "CCD-TEMP", "-10", "" ) << FITSHeaderKeyword( "DATE-OBS", "'2026-09-20T03:04:05'", "" )
                  << FITSHeaderKeyword( "SITELAT", "'+40 11 12'", "" );
               const String path = dir.Path() + String().Format( "/light_%02d.fits", i + 1 );
               JWriteFits( path, img, kw );
               rows.push_back( { true, U8( path ), "", "" } );
            }
            const GlobalRunResult g = RunGlobalProcess( "ImageIntegration",
                                                        { { "weightMode", "DontCare" }, { "generateRejectionMaps", true } },
                                                        { { "images", rows } } );
            info["ii"] = { { "ok", g.ok }, { "error", U8( g.error ) }, { "created", g.createdWindows } };
            for ( const std::string& id : g.createdWindows )
               made.push_back( id );
            if ( g.ok && !g.createdWindows.empty() )
            {
               const std::string id = g.createdWindows.front();
               const String h = JEvalJs( "(function(){ var v = View.viewById( \"" + String( id.c_str() ) + "\" );"
                  " var ids = []; for ( var i = 0; i < v.initialProcessing.length; ++i ) ids.push( v.initialProcessing.at( i ).processId() );"
                  " var pids = []; for ( var i = 0; i < v.processing.length; ++i ) pids.push( v.processing.at( i ).processId() );"
                  " return JSON.stringify( { initialIds: ids, processingIds: pids } ); })()" );
               info["ii"]["history"] = nlohmann::json::parse( U8( h ) );
               nlohmann::json names = nlohmann::json::array(), hist = nlohmann::json::array();
               for ( const FITSHeaderKeyword& k : ImageWindow::WindowById( IsoString( id.c_str() ) ).Keywords() )
               {
                  names.push_back( std::string( k.name.c_str() ) );
                  if ( k.name.Trimmed() == "HISTORY" && hist.size() < 5 )
                     hist.push_back( U8( String( k.comment.c_str() ) ) );
               }
               info["ii"]["keywordNames"] = names;
               info["ii"]["historyHead"] = hist;
               // Every created window: its first history step and the read-only
               // integrationImageId that step's XPSM carries (raw, before Task 3's
               // read-only drop). The result is createdWindows.front() (Ruling 29).
               nlohmann::json created = nlohmann::json::array();
               bool idInHistory = true;
               for ( size_t i = 0; i < g.createdWindows.size(); ++i )
               {
                  const std::string cid = g.createdWindows[i];
                  const String c = JEvalJs( "(function(){ var v = View.viewById( \"" + String( cid.c_str() ) + "\" );"
                     " var ip = v.initialProcessing; if ( ip.length == 0 ) return JSON.stringify( { initialLength: 0, first: \"\", iid: \"\" } );"
                     " var x = ip.at( 0 ).toSource( \"XPSM 1.0\" );"
                     " var m = /<parameter id=\"integrationImageId\"(?: value=\"([^\"]*)\"\\s*\\/>|>([^<]*)<)/.exec( x );"
                     " return JSON.stringify( { initialLength: ip.length, first: ip.at( 0 ).processId(),"
                     "   iid: m ? (m[1] !== undefined ? m[1] : m[2]) : \"\" } ); })()" );
                  const nlohmann::json cj = nlohmann::json::parse( U8( c ) );
                  const std::string iid = cj.at( "iid" );
                  created.push_back( { { "id", cid }, { "initialLength", cj.at( "initialLength" ) },
                                       { "firstProcessId", cj.at( "first" ) }, { "integrationImageId", iid } } );
                  // The result names itself; every auxiliary output names something else.
                  idInHistory = idInHistory && !iid.empty() && ((i == 0) == (iid == cid));
               }
               info["iiCreated"] = created;
               info["iiIdInHistory"] = idInHistory && g.createdWindows.size() >= 2;
               iiOk = true;
            }
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      for ( const std::string& id : made )
         JForceClose( id );
      // pcJourneyPre / pcJourneyPreNew stay open for section J6 (Task 7).
      const nlohmann::json phaseErrors = SelfTestPhaseStore().value( "errors", nlohmann::json::array() );
      info["phaseErrors"] = phaseErrors;
      const bool ok = phaseErrors.empty() && preOk && notifyDecided && nestedOk && mcOk && timerApplyOk && identityOk && xpsmOk && historyCostOk
                   && reopenOk && statsCostOk && iiOk;
      out["journeySpikeInfo"] = info;
      out["journeySpikeError"] = U8( error );
      out["journeySpikeOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section JH: an applied process lands in History, or fails loudly (Task T-hist) ----
   SelfTestSectionMark( "JH an applied process lands in History" );
   {
      bool detectInProcessOk = false, toolErrorOk = false, noFalseAlarmOk = false, countedOk = false,
           classifierOk = false, cyclesOk = false, previewInProcessOk = false, heldReplyOk = false, kindsOk = false,
           unverifiedOk = false, previewKindsOk = false;
      nlohmann::json info = nlohmann::json::object();
      String error;
      const bool seamWas = InProcessAppliesExpectedForSelfTest();
      try
      {
         // (a) DETECT, deterministic: inside this executeGlobal() NO process is
         //     recorded in History (harness fact), i.e. exactly the hazard. With
         //     the self-test seam off, a completed apply must be a loud error
         //     that says the image changed outside History -- never ok.
         {
            ImageWindow w( 32, 32, 1, 32, true, false, true, "pcHistInProc" );
            const View v = w.MainView();
            SetInProcessAppliesExpectedForSelfTest( true );
            const ApplyProcessResult fill = ApplyProcess( "PixelMath", { { "expression", "0.8" } }, nlohmann::json::object(), v );
            SetInProcessAppliesExpectedForSelfTest( false );
            const uint64_t mc0 = w.ModifyCount();
            const ApplyProcessResult a = ApplyProcess( "PixelMath", { { "expression", "$T*0.5" } }, nlohmann::json::object(), v );
            double px = -1;
            {
               ImageVariant iv = v.Image();
               px = iv.IsFloatSample() && iv.BitsPerSample() == 32 ? double( static_cast<const Image&>( *iv ).Pixel( 0, 0 ) ) : -1;
            }
            const std::string err = U8( a.error );
            info["inProcess"] = { { "fillOk", fill.ok }, { "ok", a.ok }, { "unrecordedChange", a.unrecordedChange },
                                  { "error", err }, { "pixel", px }, { "modifyCount", { mc0, uint64_t( w.ModifyCount() ) } },
                                  { "parametersSet", a.parametersSet } };
            detectInProcessOk = fill.ok && !a.ok && a.unrecordedChange
                             && err.find( "did NOT record it in the image's History" ) != std::string::npos
                             && err.find( "Edit > Undo cannot revert it" ) != std::string::npos
                             && err.find( "Tell the user" ) != std::string::npos
                             && std::fabs( px - 0.4 ) < 1e-6 && w.ModifyCount() == mc0
                             && a.parametersSet.value( "expression", std::string() ) == "$T*0.5";

            // The apply_process tool: is_error true, and the turn still knows
            // an image changed (turn-end notes).
            ToolContext ctx;
            ctx.mode = AgentMode::Copilot;
            ctx.turnViewId = v.FullId();
            const ToolOutcome o = ExecuteTool( ToolCall{ "toolu_hist1", "apply_process",
                                                         { { "process_id", "PixelMath" },
                                                           { "parameters", { { "expression", "$T*0.5" } } } } }, ctx );
            const nlohmann::json block = ToolResultBlock( "toolu_hist1", o );
            const std::string text = o.content.empty() ? std::string() : o.content.at( 0 ).value( "text", std::string() );
            info["tool"] = { { "isError", o.isError }, { "mutated", o.mutated }, { "blockIsError", block.value( "is_error", false ) },
                             { "text", text }, { "logLine", U8( o.logLine ) } };
            toolErrorOk = o.isError && o.mutated && block.value( "is_error", false ) == true
                       && text.find( "did NOT record it" ) != std::string::npos
                       && U8( o.logLine ).find( "did NOT record it" ) != std::string::npos;

            // No false alarm: an instance that does not update the target's
            // history (PixelMath createNewImage: IsHistoryUpdater false) is ok.
            // (The new window is found by diffing the open windows; a named
            // newImageId is covered by the agent self-test, stringRulesOk.)
            std::set<std::string> before;
            for ( const ImageWindow& x : ImageWindow::AllWindows() )
               before.insert( std::string( x.MainView().Id().c_str() ) );
            const ApplyProcessResult n = ApplyProcess( "PixelMath", { { "expression", "$T" }, { "createNewImage", true } },
                                                       nlohmann::json::object(), v );
            std::vector<std::string> created;
            for ( const ImageWindow& x : ImageWindow::AllWindows() )
               if ( before.count( std::string( x.MainView().Id().c_str() ) ) == 0 )
                  created.push_back( std::string( x.MainView().Id().c_str() ) );
            info["createNewImage"] = { { "ok", n.ok }, { "error", U8( n.error ) }, { "created", created } };
            noFalseAlarmOk = n.ok && !n.unrecordedChange && created.size() == 1;
            // A preview in-process (seam off): nothing is recorded inside this
            // executeGlobal(), so a preview apply must be the loud error too
            // (or, should PixInsight record it, a verified preview step) --
            // never an unverified ok. The main image is never touched.
            const View pv = w.CreatePreview( Rect( 0, 0, 16, 16 ), "pcHistPv" );
            const ApplyProcessResult pa = ApplyProcess( "PixelMath", { { "expression", "0.1" } }, nlohmann::json::object(), pv );
            double mainPx = -1;
            {
               ImageVariant iv = v.Image();
               mainPx = double( static_cast<const Image&>( *iv ).Pixel( 0, 0 ) );
            }
            info["previewInProcess"] = { { "ok", pa.ok }, { "error", U8( pa.error ) }, { "undo", U8( pa.undo ) },
                                         { "unrecordedChange", pa.unrecordedChange }, { "mainPixel", mainPx } };
            const bool pvVerified = pa.ok && U8( pa.undo ).find( "Recorded as the preview" ) != std::string::npos;
            const bool pvLoud = !pa.ok && pa.unrecordedChange && U8( pa.error ).find( "did NOT record it" ) != std::string::npos;
            previewInProcessOk = (pvVerified || pvLoud) && std::fabs( mainPx - 0.2 ) < 1e-6;   // 0.2: two $T*0.5 above

            // Review round 2: a History read that is busy (EvalGuard held) or
            // failed can decide nothing -- the result is a DISTINCT error,
            // never ok, and the chat-log line shows it.
            {
               EvalDepthGuard busyScript;   // as if another EvaluateScript were running
               const ApplyProcessResult u = ApplyProcess( "PixelMath", { { "expression", "$T" } }, nlohmann::json::object(), v );
               const ToolOutcome uo = ExecuteTool( ToolCall{ "toolu_hist2", "apply_process",
                                                             { { "process_id", "PixelMath" },
                                                               { "parameters", { { "expression", "$T" } } } } }, ctx );
               const nlohmann::json ub = ToolResultBlock( "toolu_hist2", uo );
               const std::string ue = U8( u.error );
               info["unverified"] = { { "ok", u.ok }, { "unverifiedChange", u.unverifiedChange },
                                      { "unrecordedChange", u.unrecordedChange }, { "error", ue },
                                      { "toolIsError", ub.value( "is_error", false ) }, { "toolMutated", uo.mutated },
                                      { "logLine", U8( uo.logLine ) } };
               unverifiedOk = !u.ok && u.unverifiedChange && !u.unrecordedChange
                           && ue.find( "could NOT verify" ) != std::string::npos
                           && ue.find( "another script evaluation was running" ) != std::string::npos
                           && ue.find( "Tell the user" ) != std::string::npos
                           && uo.isError && uo.mutated && ub.value( "is_error", false ) == true
                           && U8( uo.logLine ).find( "could NOT verify" ) != std::string::npos;
            }
            noFalseAlarmOk = noFalseAlarmOk && U8( n.undo ).find( "adds no History step" ) != std::string::npos;
            SetInProcessAppliesExpectedForSelfTest( seamWas );
            for ( const std::string& id : created )
               JForceClose( id );
            JForceClose( "pcHistInProc" );
         }
         // The seam counted the earlier sections' in-process applies instead
         // of failing them (proof the check ran there too).
         info["inProcessUnrecordedCounted"] = InProcessUnrecordedAppliesForSelfTest();
         countedOk = InProcessUnrecordedAppliesForSelfTest() > 0;

         // (b) Which replies wait for idle (pure).
         {
            auto tu = []( const char* name ) { return nlohmann::json{ { "type", "tool_use" }, { "id", "x" }, { "name", name },
                                                                      { "input", nlohmann::json::object() } }; };
            const nlohmann::json text = { { "type", "text" }, { "text", "apply_process" } };
            classifierOk = ResponseCallsImageChangingTool( nlohmann::json::array( { text, tu( "apply_process" ) } ) )
                        && ResponseCallsImageChangingTool( nlohmann::json::array( { tu( "run_global_process" ) } ) )
                        && ResponseCallsImageChangingTool( nlohmann::json::array( { tu( "describe_process" ), tu( "run_pjsr" ) } ) )
                        && !ResponseCallsImageChangingTool( nlohmann::json::array( { tu( "get_view_context" ), tu( "list_processes" ),
                                                                                     tu( "describe_process" ), text } ) )
                        && !ResponseCallsImageChangingTool( nlohmann::json::array() )
                        && !ResponseCallsImageChangingTool( nlohmann::json() )
                        && !ResponseCallsImageChangingTool( nlohmann::json::array( { { { "type", "tool_use" }, { "name", 7 } } } ) );
         }

         // (b2) The panel's held-reply decision (pure; e_Poll_Timer is plumbing).
         {
            using A = HeldReplyAction;
            const double n = PICopilotBusyWaitNoteSeconds;
            heldReplyOk = DecideHeldReply( false, true, true, true, 0.1, false ) == A::Wait
                       && DecideHeldReply( false, true, true, true, n, false ) == A::WaitAndNote
                       && DecideHeldReply( false, true, true, true, n + 5, true ) == A::Wait
                       && DecideHeldReply( false, true, true, false, n + 5, true ) == A::RunAfterNote
                       && DecideHeldReply( false, true, true, false, 0.1, false ) == A::Run
                       && DecideHeldReply( true, true, true, true, n + 5, true ) == A::Run      // Stop ends the wait
                       && DecideHeldReply( false, true, false, true, n + 5, false ) == A::Run   // text / read-only tools
                       && DecideHeldReply( false, false, true, true, n + 5, false ) == A::Run;  // failed reply
            info["heldReplyOk"] = heldReplyOk;
         }

         // (c) The forced-10 ms repro (phases hist.*, test/selftest.js): each
         //     cycle applied from a module Timer tick to a top-level window
         //     (0.8 -> 0.4), either right after PixInsight returned from a
         //     process execution ("tail") or while another process ran on a
         //     4000x4000 view ("during"); ungated = straight away (DETECT),
         //     gated = the tool loop's PREVENT gate first.
         const nlohmann::json& phases = SelfTestPhaseStore();
         const nlohmann::json cycles = phases.value( "hist.check", nlohmann::json::array() );
         nlohmann::json rows = nlohmann::json::array();
         std::map<std::string, int> n, unrecorded;
         bool allConsistent = true, gatedAllRecorded = true, gatedAllDeferred = true;
         for ( const nlohmann::json& c : cycles )
         {
            const nlohmann::json& top = c.at( "top" );
            const nlohmann::json& t = c.at( "tick" );
            const std::string key = top.at( "kind" ).get<std::string>() + (top.at( "gated" ).get<bool>() ? "/gated" : "/ungated");
            ++n[key];
            const bool applied = t.is_object() && t.contains( "ok" );
            const bool pixelsChanged = std::fabs( top.at( "pxBefore" ).get<double>() - 0.8 ) < 1e-6
                                    && std::fabs( top.at( "pxAfter" ).get<double>() - 0.4 ) < 1e-6;
            const bool recorded = top.at( "lengthAfter" ).get<int>() == top.at( "lengthBefore" ).get<int>() + 1
                               && top.at( "lastProcessId" ) == "PixelMath" && top.at( "lastHasExpression" ).get<bool>();
            const bool tickOk = applied && t.at( "ok" ).get<bool>();
            const bool mcAdvanced = applied && t.value( "modifyCountAfter", uint64_t( 0 ) ) > t.value( "modifyCountBefore", uint64_t( 0 ) );
            const std::string err = applied ? t.value( "error", std::string() ) : std::string();
            // The contract: ok <=> recorded; not recorded => the loud error.
            const bool consistent = applied && pixelsChanged
                && (recorded ? (tickOk && mcAdvanced && err.empty())
                             : (!tickOk && t.value( "unrecordedChange", false ) && !mcAdvanced
                                && err.find( "did NOT record it" ) != std::string::npos));
            if ( !recorded )
               ++unrecorded[key];
            allConsistent = allConsistent && consistent;
            if ( top.at( "gated" ).get<bool>() )
            {
               gatedAllRecorded = gatedAllRecorded && recorded && tickOk;
               gatedAllDeferred = gatedAllDeferred && applied && t.value( "deferredTicks", 0 ) >= 1;
            }
            rows.push_back( { { "kind", key }, { "recorded", recorded }, { "consistent", consistent }, { "tick", t },
                              { "top", top } } );
         }
         info["cycles"] = rows;
         info["counts"] = n;
         info["unrecorded"] = unrecorded;
         // Non-vacuous: the ungated repro must actually hit the window (measured
         // 16/16 "tail" and 13/13 "during" before this fix), and every gated
         // apply must have waited at least one tick and landed.
         // (d) Process kinds (review round 1).
         {
            const nlohmann::json kc = phases.value( "hist.kind.check", nlohmann::json::array() );
            nlohmann::json krows = nlohmann::json::array();
            bool allGood = kc.size() == 11;
            int ungatedUnrecorded = 0;
            for ( const nlohmann::json& c : kc )
            {
               const nlohmann::json& top = c.at( "top" );
               const nlohmann::json& t = c.at( "tick" );
               const std::string pid = top.at( "process" );
               const bool gated = top.at( "gated" ).get<bool>();
               const bool applied = t.is_object() && t.contains( "ok" );
               const bool tickOk = applied && t.at( "ok" ).get<bool>();
               const int dLen = top.at( "lengthAfter" ).get<int>() - top.at( "lengthBefore" ).get<int>();
               const int dIdx = top.at( "historyIndexAfter" ).get<int>() - top.at( "historyIndexBefore" ).get<int>();
               const std::string undo = applied ? t.value( "undo", std::string() ) : std::string();
               const std::string err = applied ? t.value( "error", std::string() ) : std::string();
               const long long dMc = applied ? (long long)t.value( "modifyCountAfter", uint64_t( 0 ) )
                                             - (long long)t.value( "modifyCountBefore", uint64_t( 0 ) ) : -1;
               const bool recorded = dLen == 1 && dIdx == 1 && top.at( "lastProcessId" ) == pid;
               bool good;
               if ( pid == "ScreenTransferFunction" )
                  good = tickOk && dLen == 0 && undo.find( "adds no History step" ) != std::string::npos;
               else if ( gated )
                  good = tickOk && recorded && err.empty() && undo.rfind( "Recorded in ", 0 ) == 0
                      && (dMc > 0 ? undo.find( "modification count advanced" ) != std::string::npos
                                  : undo.find( "verified in the History list" ) != std::string::npos);
               else
               {
                  // ok <=> recorded; unrecorded => the loud error.
                  good = applied && (recorded ? tickOk
                                              : !tickOk && t.value( "unrecordedChange", false ) && dLen == 0
                                                && err.find( "did NOT record it" ) != std::string::npos);
                  if ( !recorded )
                     ++ungatedUnrecorded;
               }
               allGood = allGood && good;
               krows.push_back( { { "process", pid }, { "gated", gated }, { "ok", tickOk }, { "historyDelta", dLen },
                                  { "modifyCountDelta", dMc }, { "undo", undo }, { "error", err.substr( 0, 120 ) },
                                  { "good", good } } );
            }
            info["kinds"] = krows;
            info["kindsUngatedUnrecorded"] = ungatedUnrecorded;
            kindsOk = allGood && ungatedUnrecorded >= 1;
         }

         // (e) Previews (review round 2), from the module Timer at top level:
         //     measured truth -- a preview holds ONE History step (a new one
         //     replaces it), and the main image (pixels inside AND outside the
         //     preview rectangle, History) never changes.
         {
            const nlohmann::json pc = phases.value( "hist.preview.check", nlohmann::json::array() );
            nlohmann::json prow = nlohmann::json::array();
            bool allGood = pc.size() == 4;
            int ungatedUnrecorded = 0;
            for ( const nlohmann::json& c : pc )
            {
               const nlohmann::json& b = c.at( "top" ).at( "before" );
               const nlohmann::json& a = c.at( "top" ).at( "after" );
               const bool gated = c.at( "top" ).at( "gated" ).get<bool>();
               const nlohmann::json& t = c.at( "tick" );
               const bool applied = t.is_object() && t.contains( "ok" );
               const bool tickOk = applied && t.at( "ok" ).get<bool>();
               const std::string undo = applied ? t.value( "undo", std::string() ) : std::string();
               const std::string err = applied ? t.value( "error", std::string() ) : std::string();
               const bool mainUntouched = a.at( "mainIn" ) == b.at( "mainIn" ) && a.at( "mainOut" ) == b.at( "mainOut" )
                                       && a.at( "mainLen" ) == b.at( "mainLen" ) && a.at( "mainHi" ) == b.at( "mainHi" )
                                       && t.value( "modifyCountAfter", uint64_t( 1 ) ) == t.value( "modifyCountBefore", uint64_t( 0 ) );
               // Measured: a preview step starts from the MAIN image's pixels,
               // not from an earlier preview step (0.6 -> $T*0.5 gives 0.4).
               const bool pvChanged = std::fabs( a.at( "pvPx" ).get<double>() - 0.5*b.at( "mainIn" ).get<double>() ) < 1e-6;
               const bool recorded = a.at( "pvLen" ).get<int>() == 1 && a.at( "pvHi" ).get<int>() == 1
                                  && a.at( "pvLast" ) != b.at( "pvLast" )
                                  && a.at( "pvLast" ).get<std::string>().rfind( "PixelMath:$T*0.5@", 0 ) == 0;
               bool good = applied && mainUntouched && pvChanged;
               if ( gated )
                  good = good && recorded && tickOk && undo.find( "Recorded as the preview" ) != std::string::npos;
               else
               {
                  good = good && (recorded ? tickOk : !tickOk && t.value( "unrecordedChange", false )
                                                     && err.find( "did NOT record it" ) != std::string::npos);
                  if ( !recorded )
                     ++ungatedUnrecorded;
               }
               allGood = allGood && good;
               prow.push_back( { { "gated", gated }, { "priorStep", c.at( "top" ).at( "priorStep" ) }, { "recorded", recorded },
                                 { "ok", tickOk }, { "mainUntouched", mainUntouched }, { "undo", undo },
                                 { "error", err.substr( 0, 120 ) }, { "before", b }, { "after", a }, { "good", good } } );
            }
            info["previews"] = prow;
            previewKindsOk = allGood && ungatedUnrecorded >= 1;
         }

         cyclesOk = n["tail/ungated"] == 5 && n["tail/gated"] == 5 && n["during/ungated"] == 3 && n["during/gated"] == 3
                 && allConsistent && unrecorded["tail/ungated"] >= 1 && unrecorded["during/ungated"] >= 1
                 && gatedAllRecorded && gatedAllDeferred;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      SetInProcessAppliesExpectedForSelfTest( seamWas );
      JForceClose( "pcHistInProc" );
      info["detectInProcessOk"] = detectInProcessOk;
      info["toolErrorOk"] = toolErrorOk;
      info["noFalseAlarmOk"] = noFalseAlarmOk;
      info["countedOk"] = countedOk;
      info["classifierOk"] = classifierOk;
      info["cyclesOk"] = cyclesOk;
      info["previewInProcessOk"] = previewInProcessOk;
      info["kindsOk"] = kindsOk;
      info["unverifiedOk"] = unverifiedOk;
      info["previewKindsOk"] = previewKindsOk;
      const bool ok = error.IsEmpty() && detectInProcessOk && toolErrorOk && noFalseAlarmOk && countedOk && classifierOk
                   && cyclesOk && previewInProcessOk && heldReplyOk && kindsOk && unverifiedOk && previewKindsOk;
      out["histLandedInfo"] = info;
      out["histLandedError"] = U8( error );
      out["histLandedOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J1: vendored SQLite (Task 2) -------------------------------
   SelfTestSectionMark( "J1 vendored SQLite" );
   {
      bool ok = false;
      nlohmann::json info = nlohmann::json::object();
      sqlite3* db = nullptr;
      try
      {
         info["version"] = sqlite3_libversion();
         info["sourceId"] = std::string( sqlite3_sourceid() ).substr( 0, 19 );
         info["threadsafe"] = sqlite3_threadsafe();
         const int rc = sqlite3_open_v2( ":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr );
         char* err = nullptr;
         int fk = -1;
         if ( rc == SQLITE_OK
           && sqlite3_exec( db, "CREATE TABLE t(a TEXT); INSERT INTO t VALUES ('caf\xC3\xA9 \xF0\x9F\x93\xB7');", nullptr, nullptr, &err ) == SQLITE_OK )
         {
            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2( db, "SELECT a FROM t", -1, &st, nullptr );
            if ( sqlite3_step( st ) == SQLITE_ROW )
               info["roundTrip"] = reinterpret_cast<const char*>( sqlite3_column_text( st, 0 ) );
            sqlite3_finalize( st );
            sqlite3_prepare_v2( db, "PRAGMA foreign_keys", -1, &st, nullptr );
            if ( sqlite3_step( st ) == SQLITE_ROW )
               fk = sqlite3_column_int( st, 0 );
            sqlite3_finalize( st );
         }
         if ( err != nullptr )
         {
            info["error"] = err;
            sqlite3_free( err );
         }
         info["foreignKeysDefault"] = fk;
         ok = std::string( sqlite3_libversion() ) == "3.53.4" && sqlite3_threadsafe() == 1
           && info.value( "roundTrip", std::string() ) == "caf\xC3\xA9 \xF0\x9F\x93\xB7" && fk == 1;
      }
      catch ( const std::exception& x ) { info["exception"] = x.what(); }
      if ( db != nullptr )
         sqlite3_close( db );
      out["sqliteVendorInfo"] = info;
      out["sqliteVendorOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J2: HistoryReader (Task 3) ---------------------------------
   SelfTestSectionMark( "J2 HistoryReader" );
   // Pure parser/diff checks run here; every check of a LIVE history reads the
   // pcHrA fixture that test/selftest.js built at top level, through the j2.hr
   // phases (J2State), because a step executed in-process is never recorded.
   {
      nlohmann::json d = nlohmann::json::object();
      bool parseOk = false, typesOk = false, identityOk = false, notReplayableOk = false, badXmlOk = false,
           costOk = false, integrationIdOk = false, busyOk = false, phasesOk = false, strictParseOk = false;
      String error;
      std::vector<std::string> made = { "pcHrA", "pcHrRenamed", "pcHrMask", "pcHrLong", "pcHrUtf" };
      const J2State& st = J2();
      try
      {
         // (a) PixelMath: typed scalars, read-only table dropped, time parsed.
         HistoryStep pm;
         String e;
         parseOk = ParseXpsmStep( kXpsmPixelMath, pm, e );
         d["pm"] = { { "ok", parseOk }, { "error", U8( e ) }, { "parameters", pm.parameters }, { "tables", pm.tableParameters },
                     { "started", pm.started }, { "durationS", pm.durationS }, { "identity", pm.identity } };
         typesOk = parseOk && pm.processId == "PixelMath" && pm.parameters.at( "expression" ) == "$T*2"
                && pm.parameters.at( "useSingleExpression" ) == true && pm.parameters.at( "rescaleLower" ).is_number()
                && pm.parameters.at( "rescaleLower" ).get<double>() == 0.0
                && pm.parameters.at( "newImageColorSpace" ) == "SameAsTarget"
                && pm.parameters.at( "newImageWidth" ).is_number_integer()
                && !pm.parameters.contains( "outputData" ) && !pm.tableParameters.contains( "outputData" )
                && pm.started == "2026-09-25T20:47:50.344Z" && std::fabs( pm.durationS - 0.006297325 ) < 1e-12
                && pm.replayable && pm.parseNote.empty() && pm.xpsm == kXpsmPixelMath;

         // (b) HistogramTransformation table in column order; identity independent of id=/enabled=.
         HistoryStep ht1, ht2;
         const bool h1 = ParseXpsmStep( HtXpsm( "id=\"HistogramTransformation_instance\"" ), ht1, e );
         const bool h2 = ParseXpsmStep( HtXpsm( "enabled=\"true\"" ), ht2, e );
         d["ht"] = { { "tables", ht1.tableParameters }, { "identity1", ht1.identity }, { "identity2", ht2.identity } };
         typesOk = typesOk && h1 && h2 && ht1.tableParameters.at( "H" ).size() == 5
                && ht1.tableParameters.at( "H" ).at( 3 ) == nlohmann::json::array( { 0.0, 0.25, 1.0, 0.0, 1.0 } );
         identityOk = h1 && h2 && ht1.identity == ht2.identity
                   && ht1.identity.rfind( "HistogramTransformation@2026-09-25T20:47:50.353Z#", 0 ) == 0
                   && ht1.identity.size() == std::string( "HistogramTransformation@2026-09-25T20:47:50.353Z#" ).size() + 16
                   && ht1.identity != pm.identity
                   // FNV-1a-64 reference vectors ("" and "a").
                   && Fnv1a64Hex( "" ) == "cbf29ce484222325" && Fnv1a64Hex( "a" ) == "af63dc4c8601ec8c";

         // (c) Not replayable: unknown process, Script step, unknown parameter.
         HistoryStep unk, scr, extra;
         const bool u = ParseXpsmStep( "<instance class=\"NoSuchProcessPc\" version=\"256\" id=\"x\"><parameter id=\"a\" value=\"1\"/></instance>", unk, e );
         const bool s = ParseXpsmStep( "<instance class=\"Script\" version=\"256\" id=\"Script_instance\">"
                                       "<parameter id=\"filePath\">/home/u/scripts/x.js</parameter>"
                                       "<parameter id=\"md5sum\">9dbce6</parameter><table id=\"parameters\" rows=\"0\"/>"
                                       "<parameter id=\"information\"></parameter></instance>", scr, e );
         const bool x = ParseXpsmStep( std::string( kXpsmPixelMath ).replace( std::string( kXpsmPixelMath ).find( "</instance>" ), 11,
                                       "<parameter id=\"noSuchParameterPc\" value=\"1\"/></instance>" ), extra, e );
         d["notReplayable"] = { { "unknown", unk.parseNote }, { "script", scr.parseNote }, { "extra", extra.parseNote } };
         notReplayableOk = u && s && x && !unk.replayable && !scr.replayable && !extra.replayable
                        && unk.parseNote.find( "not installed" ) != std::string::npos
                        && scr.parseNote.find( "script" ) != std::string::npos
                        && extra.parseNote.find( "noSuchParameterPc" ) != std::string::npos
                        && unk.parameters.at( "a" ) == "1";

         // (d) Malformed XML and a non-instance root fail with a message, never throw.
         HistoryStep bad;
         String e1, e2;
         badXmlOk = !ParseXpsmStep( "<instance class=\"PixelMath\"><parameter", bad, e1 ) && !e1.IsEmpty()
                 && !ParseXpsmStep( "<icon id=\"x\"/>", bad, e2 ) && e2.Contains( "not an XPSM instance" );
         d["badXml"] = { { "e1", U8( e1 ) }, { "e2", U8( e2 ) } };

         // (d2) Ruling 29: the read-only integrationImageId is captured raw BEFORE
         //      read-only parameters are dropped; other steps leave it empty.
         HistoryStep ii;
         const bool iiParsed = ParseXpsmStep( "<instance class=\"ImageIntegration\" version=\"256\" id=\"ImageIntegration_instance\">"
                                              "<parameter id=\"integrationImageId\">integration</parameter>"
                                              "<parameter id=\"lowRejectionMapImageId\">rejection_low</parameter></instance>", ii, e );
         d["integrationImageId"] = { { "ii", ii.integrationImageId }, { "pm", pm.integrationImageId }, { "parameters", ii.parameters } };
         integrationIdOk = iiParsed && ii.integrationImageId == "integration" && !ii.parameters.contains( "integrationImageId" )
                        && pm.integrationImageId.empty();

         // (d3) Review fixes: a boolean that is not "true"/"false" is a precise
         //      not-replayable note (never a silent false); an unreadable <time
         //      span> fails the step with a precise error; numbers parse the
         //      same under a comma-decimal LC_NUMERIC (the C library's strtod
         //      would stop at the '.').
         {
            HistoryStep b, sp;
            String eb, es;
            std::string x1 = kXpsmPixelMath;
            x1.replace( x1.find( "\"useSingleExpression\" value=\"true\"" ), 34, "\"useSingleExpression\" value=\"1\"   " );
            const bool bParsed = ParseXpsmStep( x1, b, eb );
            std::string x2 = kXpsmPixelMath;
            x2.replace( x2.find( "span=\"0.006297325\"" ), 18, "span=\"0.0062x7325\"" );
            const bool spParsed = ParseXpsmStep( x2, sp, es );
            const std::string oldLocale = std::setlocale( LC_NUMERIC, nullptr ) ? std::setlocale( LC_NUMERIC, nullptr ) : "C";
            const char* comma = nullptr;
            for ( const char* name : { "de_DE.UTF-8", "de_DE.utf8", "de_DE" } )
               if ( (comma = std::setlocale( LC_NUMERIC, name )) != nullptr )
                  break;
            const std::string commaName = comma != nullptr ? comma : "";
            const std::string commaPoint = comma != nullptr ? std::localeconv()->decimal_point : "";
            HistoryStep htc, pmc;
            String ec;
            const bool htcParsed = ParseXpsmStep( HtXpsm( "id=\"HistogramTransformation_instance\"" ), htc, ec );
            const bool pmcParsed = ParseXpsmStep( kXpsmPixelMath, pmc, ec );
            std::setlocale( LC_NUMERIC, oldLocale.c_str() );
            HistoryStep htRef;
            ParseXpsmStep( HtXpsm( "id=\"HistogramTransformation_instance\"" ), htRef, ec );
            d["strictParse"] = { { "boolNote", b.parseNote }, { "boolReplayable", b.replayable }, { "spanParsed", spParsed },
                                 { "spanError", U8( es ) }, { "locale", commaName }, { "localePoint", commaPoint },
                                 { "localeRestored", std::string( std::setlocale( LC_NUMERIC, nullptr ) ) },
                                 { "commaHtReplayable", htc.replayable }, { "commaHtNote", htc.parseNote },
                                 { "commaPmDuration", pmc.durationS } };
            strictParseOk = bParsed && !b.replayable && b.parseNote.find( "useSingleExpression" ) != std::string::npos
                         && !b.parameters.contains( "useSingleExpression" )
                         && !spParsed && es.Contains( "span" ) && es.Contains( "0.0062x7325" )
                         && comma != nullptr && commaPoint == ","      // the check must really run under a comma locale
                         && htcParsed && pmcParsed && htc.replayable && htc.identity == htRef.identity
                         && std::fabs( pmc.durationS - 0.006297325 ) < 1e-12
                         && std::string( std::setlocale( LC_NUMERIC, nullptr ) ) == oldLocale;
         }

         // (e)-(i) Live history, read by the j2.hr phases between top-level steps.
         const std::vector<std::string> wantSteps = { "live0", "undo", "redo", "branch", "mask", "rename", "reopen", "utf8" };
         d["phases"] = { { "steps", st.steps }, { "detail", st.detail } };
         phasesOk = st.steps == wantSteps;
         if ( !st.reopenedId.empty() )
            made.push_back( st.reopenedId );

         // (j) Cost on 500 steps (spec risk 3): full read + parse, and the tail
         //     read the tracker normally does (pcHrLong, built at top level).
         jclock::time_point t0 = jclock::now();
         const HistorySnapshot full = ReadViewHistory( "pcHrLong", 0 );
         const double fullMs = MsSince( t0 );
         t0 = jclock::now();
         const HistorySnapshot tail = ReadViewHistory( "pcHrLong", full.TotalCount() - 1 );
         const double tailMs = MsSince( t0 );
         d["cost"] = { { "fullMs", fullMs }, { "tailMs", tailMs }, { "steps", full.steps.size() }, { "full", SnapJson( full ) } };
         costOk = full.ok && tail.ok && full.length == 500 && full.steps.size() >= 500 && tail.steps.size() == 1
               && tailMs <= 150 && fullMs <= 3000;

         // (k) Controller ruling: EvaluateScript is never entered re-entrantly.
         //     While another EvaluateScript caller holds the guard, a read reports
         //     busy (retry next tick) -- not an error, and without evaluating.
         {
            const int depth0 = EvaluateScriptDepth();
            HistorySnapshot held;
            int depthHeld = -1;
            {
               EvalDepthGuard outer;
               depthHeld = EvaluateScriptDepth();
               held = ReadViewHistory( "pcHrLong", 0 );
            }
            const HistorySnapshot after = ReadViewHistory( "pcHrLong", full.TotalCount() - 1 );
            d["busy"] = { { "depth0", depth0 }, { "depthHeld", depthHeld }, { "held", SnapJson( held ) },
                          { "after", SnapJson( after ) }, { "depthAfter", EvaluateScriptDepth() } };
            // Review fix: a busy or failed snapshot diffs to NOTHING (no state
            // change) -- never "supersede every row".
            const std::vector<KnownStep> knownLong = KnownFrom( full.steps, 1000, full.ActiveCount() );
            const HistoryDiff dBusy = DiffHistory( knownLong, held );
            const HistorySnapshot failed = ReadViewHistory( "pcHrNoSuchViewPc", 0 );
            const HistoryDiff dFailed = DiffHistory( knownLong, failed );
            auto empty = []( const HistoryDiff& x )
            {
               return !x.needFullRead && x.toActive.empty() && x.toUndone.empty() && x.toSuperseded.empty()
                   && x.appended.empty() && x.appendedState.empty();
            };
            d["busy"]["diffBusyEmpty"] = empty( dBusy );
            d["busy"]["diffFailedEmpty"] = empty( dFailed );
            d["busy"]["failed"] = SnapJson( failed );
            busyOk = empty( dBusy ) && empty( dFailed ) && !failed.ok && !failed.busy && failed.error.Contains( "no view" )
                  && knownLong.size() >= 500
                  && depth0 == 0 && depthHeld == 1 && held.busy && !held.ok && held.error.IsEmpty() && held.steps.empty()
                  && after.ok && !after.busy && EvaluateScriptDepth() == 0;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      for ( const std::string& id : made )
         JForceClose( id );
      const bool ok = parseOk && typesOk && identityOk && notReplayableOk && badXmlOk && integrationIdOk
                   && strictParseOk && phasesOk && st.utf8Ok && st.liveReadOk && st.undoRedoOk && st.branchOk && st.maskOk && st.renameOk && st.reopenOk
                   && costOk && busyOk;
      d["verdicts"] = { { "parse", parseOk }, { "types", typesOk }, { "identity", identityOk }, { "notReplayable", notReplayableOk },
                        { "badXml", badXmlOk }, { "integrationId", integrationIdOk }, { "strictParse", strictParseOk }, { "phases", phasesOk },
                        { "utf8", st.utf8Ok },
                        { "liveRead", st.liveReadOk }, { "undoRedo", st.undoRedoOk }, { "branch", st.branchOk },
                        { "mask", st.maskOk }, { "rename", st.renameOk }, { "reopen", st.reopenOk },
                        { "cost", costOk }, { "busy", busyOk } };
      out["historyReaderDetail"] = d;
      out["historyReaderError"] = U8( error );
      out["historyReaderOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J3: StepStats (Task 4) -------------------------------------
   SelfTestSectionMark( "J3 StepStats" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool noiseOk = false, rescaleOk = false, rgbOk = false, u16Ok = false, thumbOk = false, busyOk = false,
           previewOk = false, budgetOk = false, safeWriteOk = false;
      String error;
      try
      {
         // (a) The estimator on white Gaussian noise of known sigma (pure).
         {
            Image img( 1024, 1024, ColorSpace::Gray );
            JFillNoise( img, 0.2, 0.01, 7 );
            const double s = LaplacianNoiseSigma( img, 0 );
            d["noise1024"] = s;
            noiseOk = std::fabs( s - 0.01 )/0.01 < 0.05;
         }
         // (b) 4096^2 -> k = 2: noise is rescaled to full resolution by sqrt(samples per block).
         {
            JWindow w( "pcSsBig", 4096, 4096, 1, 0 );
            View v = w.MainView();
            {
               AutoViewLock lock( v );
               ImageVariant iv = v.Image();
               JFillNoise( static_cast<Image&>( *iv ), 0.2, 0.02, 11 );
            }
            const StepStatsResult r = ComputeStepStats( v, String() );
            d["big"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "k", r.blockFactor }, { "n", r.samplesPerBlock },
                         { "median", r.ok ? r.channels[0].median : -1 }, { "noise", r.ok ? r.channels[0].noise : -1 } };
            rescaleOk = r.ok && r.blockFactor == 2 && r.channels.size() == 1
                     && r.samplesPerBlock == 2*((2 + PICopilotJourneyStatsRowStride - 1)/PICopilotJourneyStatsRowStride)
                     && std::fabs( r.channels[0].median - 0.2 ) < 0.001
                     && std::fabs( r.channels[0].noise - 0.02 )/0.02 < 0.07 && r.thumbnailPath.IsEmpty();
         }
         // (c) RGB + alpha: three nominal channels, per-channel levels.
         {
            ImageWindow w( 256, 256, 4, 32, true, true, true, "pcSsRgb" );
            View v = w.MainView();
            {
               AutoViewLock lock( v );
               ImageVariant iv = v.Image();
               Image& img = static_cast<Image&>( *iv );
               for ( int c = 0; c < 4; ++c )
                  img.Fill( float( c < 3 ? 0.1*(c + 1) : 1.0 ), Rect( 0 ), c, c );
            }
            const StepStatsResult r = ComputeStepStats( v, String() );
            d["rgb"] = { { "ok", r.ok }, { "channels", r.channels.size() } };
            rgbOk = r.ok && r.channels.size() == 3 && std::fabs( r.channels[2].median - 0.3 ) < 1e-6
                 && std::fabs( r.channels[0].mean - 0.1 ) < 1e-6 && r.channels[1].max <= 0.2 + 1e-6;
            w.ForceClose();
         }
         // (d) 16-bit integer data is normalized to [0,1].
         {
            ImageWindow w( 128, 128, 1, 16, false, false, true, "pcSsU16" );
            View v = w.MainView();
            {
               AutoViewLock lock( v );
               ImageVariant iv = v.Image();
               static_cast<UInt16Image&>( *iv ).Fill( uint16( 32768 ) );
            }
            const StepStatsResult r = ComputeStepStats( v, String() );
            u16Ok = r.ok && std::fabs( r.channels[0].median - 32768.0/65535 ) < 1e-4;
            d["u16"] = r.ok ? r.channels[0].median : -1.0;
            w.ForceClose();
         }
         // (e) Thumbnail: a real JPEG, long edge 256, in a directory that did not exist yet.
         {
            JTempDir dir( "picopilot-ss-" );
            JWindow w( "pcSsThumb", 1200, 800, 3, 0.25 );
            const String path = dir.Path() + "/thumbs/17.jpg";
            const StepStatsResult r = ComputeStepStats( w.MainView(), path );
            bool jpeg = false;
            int bw = 0, bh = 0;
            if ( File::Exists( path ) )
            {
               const ByteArray b = File::ReadFile( path );
               jpeg = b.Length() > 4 && b[0] == 0xFF && b[1] == 0xD8;
               Bitmap bmp( path );
               bw = bmp.Width();
               bh = bmp.Height();
            }
            d["thumb"] = { { "path", U8( r.thumbnailPath ) }, { "error", U8( r.thumbnailError ) }, { "w", bw }, { "h", bh } };
            thumbOk = r.ok && r.thumbnailPath == path && r.thumbnailError.IsEmpty() && jpeg
                   && std::max( bw, bh ) == PICopilotJourneyThumbEdge && bh > 0;
         }
         // (f) Busy view: refused at once, never waited on.
         {
            JWindow w( "pcSsBusy", 64, 64, 1, 0.5 );
            View v = w.MainView();
            const jclock::time_point t0 = jclock::now();
            StepStatsResult r;
            {
               AutoViewLock lock( v );
               r = ComputeStepStats( v, String() );
            }
            const double ms = MsSince( t0 );
            d["busy"] = { { "error", U8( r.error ) }, { "ms", ms } };
            busyOk = !r.ok && r.error.Contains( "busy" ) && ms < 100;
         }
         // (g) The preview still works on the extracted block average.
         {
            JWindow w( "pcSsPrev", 4096, 2000, 3, 0.2 );
            const ViewPreviewResult p = RenderViewPreview( w.MainView() );
            previewOk = p.ok && p.blockFactor == 2 && std::max( p.width, p.height ) <= PICopilotPreviewMaxEdge;
            d["preview"] = { { "ok", p.ok }, { "k", p.blockFactor }, { "error", U8( p.error ) } };
         }
         // (h) 60 MP RGB float with a thumbnail within the step budget (spec §9).
         {
            JTempDir dir( "picopilot-ss60-" );
            JWindow w( "pcSs60", 9504, 6336, 3, 0.1 );
            const StepStatsResult r = ComputeStepStats( w.MainView(), dir.Path() + "/t.jpg" );
            d["mp60"] = { { "ok", r.ok }, { "ms", r.elapsedMs }, { "budgetMs", PICopilotJourneyStepBudgetMs }, { "stride", r.rowStride } };
            budgetOk = r.ok && r.thumbnailError.IsEmpty() && r.elapsedMs <= PICopilotJourneyStepBudgetMs;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      // (i) Fix round 1 (CWE-59): the thumbnail and SafeFileWrite never write
      // through a symlink, and a failure leaves no partial file and no temp.
      try
      {
         nlohmann::json sw = nlohmann::json::object();
         JTempDir dir( "picopilot-sw-" );
         const std::string root = U8( dir.Path() );
         auto entries = [&]( const std::string& d )
         {
            std::vector<std::string> names;
            if ( DIR* h = ::opendir( d.c_str() ) )
            {
               while ( const dirent* e = ::readdir( h ) )
                  if ( std::string( e->d_name ) != "." && std::string( e->d_name ) != ".." )
                     names.push_back( e->d_name );
               ::closedir( h );
            }
            std::sort( names.begin(), names.end() );
            return names;
         };
         auto isLink = []( const std::string& p ) { struct stat st; return ::lstat( p.c_str(), &st ) == 0 && S_ISLNK( st.st_mode ); };
         auto exists = []( const std::string& p ) { struct stat st; return ::lstat( p.c_str(), &st ) == 0; };
         const ByteArray victimBytes( 6, uint8( 'V' ) );
         const String victim = dir.Path() + "/victim.jpg";
         File::WriteFile( victim, victimBytes );
         File::CreateDirectory( dir.Path() + "/thumbs" );

         // (i1) Thumbnail target pre-placed as a symlink -> refused, not followed.
         bool linkThumbOk = false;
         {
            const std::string link = root + "/thumbs/5.jpg";
            (void)::symlink( U8( victim ).c_str(), link.c_str() );
            JWindow w( "pcSsLink", 300, 200, 1, 0.3 );
            const StepStatsResult r = ComputeStepStats( w.MainView(), FromU8( link ) );
            const bool victimSame = File::ReadFile( victim ) == victimBytes;
            sw["linkThumb"] = { { "ok", r.ok }, { "error", U8( r.thumbnailError ) }, { "path", U8( r.thumbnailPath ) },
                                { "victimUnchanged", victimSame }, { "stillLink", isLink( link ) } };
            linkThumbOk = r.ok && r.thumbnailPath.IsEmpty() && r.thumbnailError.Contains( "symbolic link" )
                       && victimSame && isLink( link ) && entries( root + "/thumbs" ) == std::vector<std::string>{ "5.jpg" };
            ::unlink( link.c_str() );
         }
         // (i2) The byte API refuses a symlink target too.
         bool linkBytesOk = false;
         {
            const std::string link = root + "/recipe.json";
            (void)::symlink( U8( victim ).c_str(), link.c_str() );
            const String why = SafeWriteTextFile( FromU8( link ), "{\"x\":1}", SafeFileMode::Shared );
            linkBytesOk = why.Contains( "symbolic link" ) && File::ReadFile( victim ) == victimBytes && isLink( link );
            sw["linkBytes"] = U8( why );
            ::unlink( link.c_str() );
         }
         // (i3) Forced failure after the temp is written: no target, no temp.
         bool injectedOk = false;
         {
            SetSafeFileWriteFailBeforeRenameForSelfTest( true );
            const String why = SafeWriteTextFile( dir.Path() + "/thumbs/f.md", "partial?", SafeFileMode::Shared );
            SetSafeFileWriteFailBeforeRenameForSelfTest( false );
            injectedOk = why.Contains( "injected failure" ) && entries( root + "/thumbs" ).empty();
            sw["injected"] = { { "error", U8( why ) }, { "left", entries( root + "/thumbs" ) } };
         }
         // (i4) Thumbnail with the same forced failure: stats still ok, no file, no temp.
         bool thumbFailOk = false;
         {
            JWindow w( "pcSsFail", 300, 200, 1, 0.3 );
            SetSafeFileWriteFailBeforeRenameForSelfTest( true );
            const StepStatsResult r = ComputeStepStats( w.MainView(), dir.Path() + "/thumbs/6.jpg" );
            SetSafeFileWriteFailBeforeRenameForSelfTest( false );
            thumbFailOk = r.ok && !r.thumbnailError.IsEmpty() && r.thumbnailPath.IsEmpty() && entries( root + "/thumbs" ).empty();
            sw["thumbFail"] = { { "ok", r.ok }, { "error", U8( r.thumbnailError ) }, { "left", entries( root + "/thumbs" ) } };
         }
         // (i5) A renderer that throws mid-write, and a failed content check: no target, private dir gone.
         bool renderFailOk = false;
         {
            String given;
            const String why = SafeRenderFile( dir.Path() + "/thumbs/7.jpg", SafeFileMode::Shared,
               [&given]( const String& temp ) { given = temp; File::WriteFile( temp, ByteArray( 10, uint8( 0xFF ) ) ); throw Error( "render blew up" ); } );
            const String whyCheck = SafeWriteFile( dir.Path() + "/thumbs/8.jpg", ByteArray( 10, uint8( 'x' ) ), SafeFileMode::Shared, SafeCheckJpeg );
            const std::string privDir = U8( File::ExtractDirectory( given ) );
            renderFailOk = why.Contains( "render blew up" ) && !given.IsEmpty() && !exists( privDir )
                        && whyCheck.Contains( "not a JPEG" ) && entries( root + "/thumbs" ).empty();
            sw["renderFail"] = { { "error", U8( why ) }, { "checkError", U8( whyCheck ) }, { "privDirGone", !exists( privDir ) } };
         }
         // (i6) Normal writes: a valid JPEG thumbnail, then an existing regular file replaced atomically.
         bool normalOk = false;
         {
            JWindow w( "pcSsNorm", 300, 200, 3, 0.3 );
            const String path = dir.Path() + "/thumbs/9.jpg";
            const StepStatsResult r1 = ComputeStepStats( w.MainView(), path );
            const StepStatsResult r2 = ComputeStepStats( w.MainView(), path );   // over an existing file
            const ByteArray b = File::Exists( path ) ? File::ReadFile( path ) : ByteArray();
            const String t1 = SafeWriteTextFile( dir.Path() + "/j.md", "one", SafeFileMode::Shared );
            const String t2 = SafeWriteTextFile( dir.Path() + "/j.md", "two", SafeFileMode::Shared );
            const ByteArray jb = File::ReadFile( dir.Path() + "/j.md" );
            normalOk = r1.thumbnailError.IsEmpty() && r2.thumbnailError.IsEmpty() && r2.thumbnailPath == path
                    && SafeCheckJpeg( b ).IsEmpty() && t1.IsEmpty() && t2.IsEmpty()
                    && std::string( jb.Begin(), jb.End() ) == "two"
                    && entries( root + "/thumbs" ) == std::vector<std::string>{ "9.jpg" };
            sw["normal"] = { { "e1", U8( r1.thumbnailError ) }, { "e2", U8( r2.thumbnailError ) }, { "bytes", b.Length() },
                             { "t1", U8( t1 ) }, { "t2", U8( t2 ) }, { "left", entries( root + "/thumbs" ) } };
         }
         // (i7) Task 5 (controller fix of the umask read-back race): the final
         // mode is fixed by file class -- Shared 0644, Private 0600, minus the
         // umask, which the KERNEL applies at open(); nothing reads or changes
         // the process umask -- and is never inherited from a replaced file.
         // A detector thread creates files while 1500 writes run: with the old
         // umask(0)/umask(m) read-back some of its files came out 0666.
         bool modesOk = false;
         {
            const mode_t um = JProcUmask();
            const int wantShared = int( 0644 & ~um ), wantPrivate = int( 0600 & ~um );
            const String ps = dir.Path() + "/shared.json", pp = dir.Path() + "/private.json";
            const String e1 = SafeWriteTextFile( ps, "{}", SafeFileMode::Shared );
            const String e2 = SafeWriteTextFile( pp, "{}", SafeFileMode::Private );
            const int m1 = JModeOf( ps ), m2 = JModeOf( pp );
            // Cross-replace: the class of the NEW write decides, not the old file.
            const String e3 = SafeWriteTextFile( pp, "{\"v\":2}", SafeFileMode::Shared );
            const String e4 = SafeWriteTextFile( ps, "{\"v\":2}", SafeFileMode::Private );
            const int m3 = JModeOf( pp ), m4 = JModeOf( ps );
            JWindow w( "pcSsMode", 300, 200, 1, 0.3 );
            const String tp = dir.Path() + "/thumbs/mode.jpg";
            const StepStatsResult r = ComputeStepStats( w.MainView(), tp );
            const int mt = JModeOf( tp );

            const std::string raceDir = root + "/race";
            ::mkdir( raceDir.c_str(), 0700 );
            std::atomic<bool> stop( false );
            std::atomic<int> made( 0 ), wrong( 0 );
            const int wantObserved = int( 0666 & ~um );
            std::thread observer( [&]()
            {
               for ( unsigned i = 0; !stop.load(); ++i )
               {
                  const std::string p = raceDir + "/o" + std::to_string( i%4 );
                  const int fd = ::open( p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666 );
                  if ( fd < 0 )
                     continue;
                  struct stat st;
                  if ( ::fstat( fd, &st ) == 0 && int( st.st_mode & 0777 ) != wantObserved )
                     ++wrong;
                  ++made;
                  ::close( fd );
                  ::unlink( p.c_str() );
               }
            } );
            int writeErrors = 0;
            const jclock::time_point t0 = jclock::now();
            for ( int i = 0; i < 1500; ++i )
               if ( !SafeWriteTextFile( FromU8( raceDir + "/w.txt" ), "x", SafeFileMode::Shared ).IsEmpty() )
                  ++writeErrors;
            stop = true;
            observer.join();
            const double raceMs = MsSince( t0 );
            const int umAfter = int( JProcUmask() );
            sw["modes"] = { { "umask", int( um ) }, { "shared", m1 }, { "private", m2 }, { "privateToShared", m3 },
                            { "sharedToPrivate", m4 }, { "thumb", mt }, { "thumbError", U8( r.thumbnailError ) },
                            { "errors", { U8( e1 ), U8( e2 ), U8( e3 ), U8( e4 ) } },
                            { "race", { { "writes", 1500 }, { "writeErrors", writeErrors }, { "observed", made.load() },
                                        { "wrongMode", wrong.load() }, { "ms", raceMs }, { "umaskAfter", umAfter } } } };
            modesOk = e1.IsEmpty() && e2.IsEmpty() && e3.IsEmpty() && e4.IsEmpty()
                   && m1 == wantShared && m2 == wantPrivate && m3 == wantShared && m4 == wantPrivate
                   && r.ok && r.thumbnailError.IsEmpty() && mt == wantShared
                   && writeErrors == 0 && made.load() > 0 && wrong.load() == 0 && umAfter == int( um );
         }
         sw["verdicts"] = { { "linkThumb", linkThumbOk }, { "linkBytes", linkBytesOk }, { "injected", injectedOk },
                            { "thumbFail", thumbFailOk }, { "renderFail", renderFailOk }, { "normal", normalOk },
                            { "modes", modesOk } };
         d["safeWrite"] = sw;
         safeWriteOk = linkThumbOk && linkBytesOk && injectedOk && thumbFailOk && renderFailOk && normalOk && modesOk;
      }
      catch ( const pcl::Exception& x ) { error += " | safeWrite: " + x.Message(); }
      catch ( const std::exception& x ) { error += " | safeWrite: " + String( x.what() ); }
      SetSafeFileWriteFailBeforeRenameForSelfTest( false );
      const bool ok = noiseOk && rescaleOk && rgbOk && u16Ok && thumbOk && busyOk && previewOk && budgetOk && safeWriteOk;
      out["stepStatsDetail"] = d;
      out["stepStatsError"] = U8( error );
      out["stepStatsOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J4: JourneyStore (Task 5) ----------------------------------
   SelfTestSectionMark( "J4 JourneyStore" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool schemaOk = false, roundTripOk = false, utf8Ok = false, stateOk = false, redactOk = false,
           retentionOk = false, damagedOk = false, newerOk = false, foreignOk = false, lockedOk = false, isoOk = false,
           fileModesOk = false, rootLinkOk = false, dbLinkOk = false, openRootOk = false, treeOk = false, openLockedOk = false,
           offRootOk = false, directRedactOk = false;
      String error;
      try
      {
         JTempDir root( "picopilot-store-" );
         String openError;
         std::unique_ptr<JourneyStore> st = JourneyStore::Open( root.Path(), openError );
         if ( !st )
            throw Error( "Open failed: " + openError );

         // (a) Schema v1: exactly the spec §5 tables/columns (+ stats.image_id, Ruling 2).
         {
            RawDb raw( st->DbPath() );
            const std::map<std::string, std::vector<std::string>> want = {
               { "journey", { "id", "created", "updated", "name", "target", "kept", "kept_at", "end_image_id", "status" } },
               { "image", { "id", "journey_id", "view_id", "file_path", "fingerprint", "is_master", "created", "owner" } },   // owner: Task 7 re-review m6
               { "acquisition", { "image_id", "target", "filter", "camera", "gain", "offset", "sensor_temp", "sub_exposure",
                                  "sub_count", "total_integration_s", "session_date" } },
               { "step", { "id", "image_id", "seq", "process_id", "params_json", "started", "duration_s", "actor", "reason",
                           "reason_inferred", "state", "history_index" } },
               { "stats", { "step_id", "image_id", "channel", "median", "mad", "mean", "min", "max", "noise" } },
               { "link", { "from_image_id", "to_image_id", "via_step_id", "evidence" } },
               { "gap", { "journey_id", "image_id", "after_step_seq", "reason" } } };
            bool all = raw.Column( "PRAGMA user_version" ) == std::vector<std::string>( { "1" } );
            std::vector<std::string> tables = raw.Column( "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name" );
            d["tables"] = tables;
            all = all && tables == std::vector<std::string>( { "acquisition", "gap", "image", "journey", "link", "sqlite_sequence", "stats", "step" } );   // sqlite_sequence: AUTOINCREMENT ids (Task 7 re-review: a removed journey id is never reused)
            for ( const auto& t : want )
            {
               const std::vector<std::string> cols = raw.Column( ( "SELECT name FROM pragma_table_info('" + t.first + "')" ).c_str() );
               d["columns"][t.first] = cols;
               all = all && cols == t.second;
            }
            schemaOk = all && raw.Column( "PRAGMA journal_mode" ) == std::vector<std::string>( { "wal" } );
         }

         // (b) Round trip of every row kind.
         const std::string now = NowIso();
         const int64 jid = st->CreateJourney( "M42 Ha 2026-09-25", "M42", now );
         const int64 img = st->AddImage( jid, "M42_Ha", "/data/M42_Ha.xisf", "64x64x1:f32:abc", true, now );
         AcquisitionFacts a;
         a.target = "M42"; a.filter = "Ha"; a.camera = "ASI2400MC"; a.subExposureS = 300.0; a.subCount = 20;
         a.totalIntegrationS = 6000.0; a.sessionDate = "2026-09-20";
         st->SetAcquisition( img, a );
         HistoryStep h;
         String pe;
         ParseXpsmStep( kXpsmPixelMath, h, pe );
         h.combinedIndex = 0;
         std::vector<int64> sids;
         for ( int i = 0; i < 3; ++i )
         {
            h.combinedIndex = i;
            sids.push_back( st->AddStep( MakeStepRow( h, img, "active", i == 1 ? "copilot" : "user", i == 1 ? "lift the background" : "", i + 1 ) ) );
         }
         st->AddStats( img, 0, { { 0, 0.1, 0.01, 0.11, 0.0, 1.0, 0.003 } } );
         st->AddStats( img, sids[2], { { 0, 0.2, 0.02, 0.21, 0.0, 1.0, 0.004 } } );
         const int64 img2 = st->AddImage( jid, "M42_Ha_stars", "", "64x64x1:f32:def", false, now );
         st->AddLink( { img, img2, sids[2], "timing" } );
         st->AddGap( { jid, img, 2, "history read failed: test" } );
         const std::vector<StepRow> steps = st->Steps( img, false );
         AcquisitionFacts back;
         const bool hasAcq = st->Acquisition( img, back );
         const std::vector<ChannelStats> s0 = st->Stats( img, 0 ), s2 = st->Stats( img, sids[2] );
         const std::vector<LinkRow> links = st->Links( jid );
         const std::vector<GapRow> gaps = st->Gaps( jid );
         JourneyRow jr;
         const bool gotJ = st->GetJourney( jid, jr );
         d["roundTrip"] = { { "steps", steps.size() }, { "actor1", steps.size() > 1 ? steps[1].actor : "" },
                            { "links", links.size() }, { "gaps", gaps.size() } };
         roundTripOk = gotJ && jr.name == "M42 Ha 2026-09-25" && jr.status == "recording" && !jr.kept
                    && steps.size() == 3 && steps[1].actor == "copilot" && steps[1].reason == "lift the background"
                    && steps[0].params.at( "parameters" ).at( "expression" ) == "$T*2"
                    && steps[0].params.at( "identity" ) == h.identity && steps[2].seq == 3
                    && hasAcq && back.filter == "Ha" && back.subCount == 20 && !back.gain.has_value()
                    && back.totalIntegrationS.value_or( -1 ) == 6000.0
                    && s0.size() == 1 && std::fabs( s0[0].median - 0.1 ) < 1e-12 && s2.size() == 1 && s2[0].noise == 0.004
                    && links.size() == 1 && links[0].evidence == "timing" && links[0].viaStepId == sids[2]
                    && gaps.size() == 1 && gaps[0].afterSeq == 2 && st->Images( jid ).size() == 2
                    && st->StepCount( jid, true ) == 3;

         // (c) UTF-8 (non-BMP) round trip.
         const std::string uname = "C\xC3\xB4ne \xF0\x9F\x94\xAD";
         st->RenameJourney( jid, uname );
         JourneyRow ju;
         st->GetJourney( jid, ju );
         utf8Ok = ju.name == uname;

         // (d) States: superseded rows are hidden unless asked for.
         st->SetStepState( sids[2], "superseded" );
         stateOk = st->Steps( img, false ).size() == 2 && st->Steps( img, true ).size() == 3;

         // (e) Location redaction (Ruling 20): nothing identifying reaches the file.
         {
            HistoryStep loc;
            loc.processId = "FITSHeader";
            loc.parameters = { { "obsLatitude", 47.123456 }, { "note", "keep me" } };
            loc.tableParameters = { { "keywords", { { "SITELAT", "'+47 07 24.4'", "site" }, { "OBJECT", "'M42'", "" } } } };
            loc.xpsm = "<instance class=\"FITSHeader\"><parameter id=\"x\">+47 07 24.4</parameter></instance>";
            loc.identity = "FITSHeader@#0";
            loc.combinedIndex = 5;
            const int64 lid = st->AddStep( MakeStepRow( loc, img, "active", "user", "", 6 ) );
            st->Checkpoint();
            const std::string bytes = FileBytes( st->DbPath() ) + FileBytes( st->DbPath() + "-wal" );
            StepRow r;
            st->GetStep( lid, r );
            d["redacted"] = r.params;
            redactOk = bytes.find( "47 07 24" ) == std::string::npos && bytes.find( "47.123456" ) == std::string::npos
                    && r.params.at( "parameters" ).at( "obsLatitude" ) == "[redacted]"
                    && r.params.at( "parameters" ).at( "note" ) == "keep me"
                    && r.params.at( "tableParameters" ).at( "keywords" ).at( 0 ).at( 1 ) == "[redacted]"
                    && r.params.at( "tableParameters" ).at( "keywords" ).at( 1 ).at( 1 ) == "'M42'"
                    && r.params.at( "replayable" ) == false && r.params.at( "xpsm" ) == ""
                    && r.params.at( "parseNote" ) == "contained observing-site data; not stored";
         }

         // (f) Retention: only the old unkept journey goes (row, cascade, folder).
         {
            const int64 oldU = st->CreateJourney( "old", "X", IsoDaysAgo( 40 ) );
            const int64 newU = st->CreateJourney( "new", "X", IsoDaysAgo( 5 ) );
            const int64 oldK = st->CreateJourney( "keeper", "X", IsoDaysAgo( 400 ) );
            const int64 oi = st->AddImage( oldU, "o", "", "fp-o", true, IsoDaysAgo( 40 ) );
            st->AddStep( MakeStepRow( h, oi, "active", "user", "", 1 ) );
            st->MarkKept( oldK, 0, IsoDaysAgo( 399 ) );
            for ( int64 id : { oldU, newU, oldK } )
            {
               File::CreateDirectory( st->JourneyDir( id ) + "/thumbs" );
               File::WriteTextFile( st->JourneyDir( id ) + "/thumbs/1.jpg", "x" );
            }
            // MarkKept touched 'updated'; age the keeper again so only 'kept' protects it.
            RawDb( st->DbPath() ).Exec( ( "UPDATE journey SET updated='" + IsoDaysAgo( 400 ) + "' WHERE id=" + std::to_string( oldK ) ).c_str() );
            StringList removed;
            const int n = st->PruneUnkept( IsoDaysAgo( 30 ), &removed );
            JourneyRow tmp;
            RawDb raw( st->DbPath() );
            d["retention"] = { { "pruned", n }, { "orphanSteps", raw.Column( ( "SELECT count(*) FROM step WHERE image_id=" + std::to_string( oi ) ).c_str() ) } };
            retentionOk = n == 1 && !st->GetJourney( oldU, tmp ) && st->GetJourney( newU, tmp ) && st->GetJourney( oldK, tmp )
                       && !File::DirectoryExists( st->JourneyDir( oldU ) ) && File::DirectoryExists( st->JourneyDir( oldK ) )
                       && raw.Column( ( "SELECT count(*) FROM step WHERE image_id=" + std::to_string( oi ) ).c_str() )
                          == std::vector<std::string>( { "0" } );
         }

         // (j) Locked by another connection: a quick, named failure; works after release.
         {
            RawDb other( st->DbPath() );
            other.Exec( "BEGIN EXCLUSIVE" );
            const jclock::time_point t0 = jclock::now();
            String lockMsg;
            try { st->CreateJourney( "while locked", "X", NowIso() ); }
            catch ( const pcl::Exception& x ) { lockMsg = x.Message(); }
            const double ms = MsSince( t0 );
            other.Exec( "COMMIT" );
            int64 after = 0;
            try { after = st->CreateJourney( "after unlock", "X", NowIso() ); } catch ( ... ) {}
            d["locked"] = { { "message", U8( lockMsg ) }, { "ms", ms }, { "after", after } };
            lockedOk = lockMsg.Contains( "locked" ) && lockMsg.Contains( st->DbPath() ) && ms < 1000 && after > 0;
         }

         // (r) Fix round 1, I1: every JourneyStore call is ROOT THREAD ONLY
         // (global constraints). Off the root thread a write, a read, a raw
         // Exec (Checkpoint) and Open all throw a named error, and nothing
         // reaches the file; the root thread still works afterwards.
         {
            RawDb raw( st->DbPath() );
            const char* const kCount = "SELECT count(*) FROM journey";
            const std::vector<std::string> before = raw.Column( kCount );
            JourneyStore* sp = st.get();
            const String rootPath = root.Path();
            JCallThread tw( [sp]() { sp->CreateJourney( "off-root write", "X", NowIso() ); } );
            JCallThread tr( [sp]() { (void)sp->Steps( 1, true ); } );
            JCallThread tx( [sp]() { sp->Checkpoint(); } );
            String openErr;
            bool offOpened = false;
            JCallThread to( [rootPath, &openErr, &offOpened]()
            {
               String e;
               offOpened = JourneyStore::Open( rootPath, e ) != nullptr;
               openErr = e;
            } );
            bool finished = true;
            for ( JCallThread* t : { &tw, &tr, &tx, &to } )
            {
               t->Start();
               finished = t->Wait( 10000 ) && finished;
            }
            const std::vector<std::string> after = raw.Column( kCount );
            int64 rootAfter = 0;
            try { rootAfter = st->CreateJourney( "root after off-root", "X", NowIso() ); } catch ( ... ) {}
            auto named = [&]( const String& m ) { return m.Contains( "root thread" ) && m.Contains( st->DbPath() ); };
            d["offRoot"] = { { "finished", finished }, { "write", U8( tw.error ) }, { "read", U8( tr.error ) },
                             { "exec", U8( tx.error ) }, { "open", U8( openErr ) }, { "openReturned", offOpened },
                             { "journeysBefore", before }, { "journeysAfter", after }, { "rootAfter", rootAfter } };
            offRootOk = finished && tw.threw && named( tw.error ) && tr.threw && named( tr.error )
                     && tx.threw && named( tx.error ) && !offOpened && named( openErr )
                     && before == after && rootAfter > 0;
         }

         // (s) Fix round 1, M2: AddStep redacts on its own -- a row that did
         // NOT come through MakeStepRow still lands with no location data.
         {
            StepRow raw;
            raw.imageId = img;
            raw.seq = 7;
            raw.processId = "FITSHeader";
            raw.params = { { "parameters", { { "siteLongitude", 11.987654 }, { "note", "kept" } } },
                           { "tableParameters", { { "keywords", { { "SITELONG", "'+11 59 15.5'", "c" } } } } },
                           { "xpsm", "<instance class=\"FITSHeader\"><parameter id=\"x\">+11 59 15.5</parameter></instance>" },
                           { "identity", "FITSHeader@#7" }, { "mask", nullptr }, { "replayable", true }, { "parseNote", "" } };
            const int64 rid = st->AddStep( raw );
            st->Checkpoint();
            const std::string bytes = FileBytes( st->DbPath() ) + FileBytes( st->DbPath() + "-wal" );
            StepRow back;
            const bool got = st->GetStep( rid, back );
            d["directRedacted"] = back.params;
            directRedactOk = got && bytes.find( "59 15" ) == std::string::npos && bytes.find( "11.987654" ) == std::string::npos
                          && back.params.at( "parameters" ).at( "siteLongitude" ) == "[redacted]"
                          && back.params.at( "parameters" ).at( "note" ) == "kept"
                          && back.params.at( "tableParameters" ).at( "keywords" ).at( 0 ).at( 1 ) == "[redacted]"
                          && back.params.at( "xpsm" ) == "" && back.params.at( "replayable" ) == false
                          && back.params.at( "parseNote" ) == "contained observing-site data; not stored";
         }
         st.reset();

         // (g) A damaged file is reported with its path and NEVER replaced.
         {
            JTempDir r2( "picopilot-store-bad-" );
            const String p = r2.Path() + "/journeys.sqlite3";
            File::WriteTextFile( p, IsoString( std::string( 4096, 'Z' ).c_str() ) );
            const std::string before = FileBytes( p );
            String e2;
            const bool opened = JourneyStore::Open( r2.Path(), e2 ) != nullptr;
            d["damaged"] = U8( e2 );
            damagedOk = !opened && e2.Contains( p ) && e2.Contains( "never replaces" ) && FileBytes( p ) == before;
         }
         // (h) Written by a newer version.
         {
            JTempDir r3( "picopilot-store-new-" );
            { RawDb raw( r3.Path() + "/journeys.sqlite3" ); raw.Exec( "CREATE TABLE journey(id INTEGER); PRAGMA user_version=2;" ); }
            String e3;
            newerOk = JourneyStore::Open( r3.Path(), e3 ) == nullptr && e3.Contains( "newer" );
            d["newer"] = U8( e3 );
         }
         // (i) Some other program's database in our file name.
         {
            JTempDir r4( "picopilot-store-foreign-" );
            { RawDb raw( r4.Path() + "/journeys.sqlite3" ); raw.Exec( "CREATE TABLE other(x TEXT);" ); }
            String e4;
            foreignOk = JourneyStore::Open( r4.Path(), e4 ) == nullptr && e4.Contains( "not a PI Copilot journey database" );
            d["foreign"] = U8( e4 );
         }
         // (k) Timestamp format.
         {
            const std::string t = NowIso();
            isoOk = t.size() == 24 && t[4] == '-' && t[10] == 'T' && t[19] == '.' && t[23] == 'Z' && IsoDaysAgo( 1 ) < t;
            d["now"] = t;
         }

         // ---- Controller addition 2: file/dir discipline of everything the store creates ----
         const mode_t um = JProcUmask();
         // (l) A missing library root is created 0700 component by component
         // (mkdir never follows a link); the DB file is created O_EXCL|O_NOFOLLOW
         // 0600 and SQLite gives its WAL the DB file's mode.
         {
            JTempDir r5( "picopilot-store-mode-" );
            const String lib = r5.Path() + "/PICopilot/journeys";
            String e5;
            std::unique_ptr<JourneyStore> s5 = JourneyStore::Open( lib, e5 );
            int dbMode = -1, walMode = -1;
            if ( s5 )
            {
               s5->CreateJourney( "m", "X", NowIso() );
               dbMode = JModeOf( s5->DbPath() );
               walMode = JModeOf( s5->DbPath() + "-wal" );
            }
            const bool opened = s5 != nullptr;
            s5.reset();
            const int rootMode = JModeOf( lib ), parentMode = JModeOf( r5.Path() + "/PICopilot" );
            d["fileModes"] = { { "opened", opened }, { "error", U8( e5 ) }, { "umask", int( um ) }, { "root", rootMode },
                               { "parent", parentMode }, { "db", dbMode }, { "wal", walMode } };
            fileModesOk = opened && e5.IsEmpty() && rootMode == int( 0700 & ~um ) && parentMode == int( 0700 & ~um )
                       && dbMode == int( 0600 & ~um ) && walMode == int( 0600 & ~um );
         }
         // (m) The library root is a symbolic link: refused, nothing created behind it.
         {
            JTempDir r6( "picopilot-store-rootlink-" );
            const String real = r6.Path() + "/real";
            File::CreateDirectory( real );
            const String lib = r6.Path() + "/journeys";
            (void)::symlink( U8( real ).c_str(), U8( lib ).c_str() );
            String e6;
            const bool opened = JourneyStore::Open( lib, e6 ) != nullptr;
            d["rootLink"] = U8( e6 );
            rootLinkOk = !opened && e6.Contains( "symbolic link" ) && e6.Contains( lib ) && JIsLink( lib )
                      && JModeOf( real + "/journeys.sqlite3" ) == -1;
         }
         // (n) journeys.sqlite3 is a symbolic link: refused, the link and its target untouched.
         {
            JTempDir r7( "picopilot-store-dblink-" );
            const String victim = r7.Path() + "/victim.bin";
            File::WriteTextFile( victim, "victim bytes" );
            const String p = r7.Path() + "/journeys.sqlite3";
            (void)::symlink( U8( victim ).c_str(), U8( p ).c_str() );
            String e7;
            const bool opened = JourneyStore::Open( r7.Path(), e7 ) != nullptr;
            d["dbLink"] = U8( e7 );
            dbLinkOk = !opened && e7.Contains( "symbolic link" ) && e7.Contains( p ) && e7.Contains( "never replaces" )
                    && JIsLink( p ) && FileBytes( victim ) == "victim bytes"
                    && JModeOf( victim + "-wal" ) == -1 && JModeOf( p + "-wal" ) == -1;
         }
         // (o) A root other users can write into: refused (they could plant the
         // DB, its WAL or a journey folder), nothing created.
         {
            JTempDir r8( "picopilot-store-open-" );
            ::chmod( U8( r8.Path() ).c_str(), 0777 );
            String e8;
            const bool opened = JourneyStore::Open( r8.Path(), e8 ) != nullptr;
            ::chmod( U8( r8.Path() ).c_str(), 0700 );
            d["openRoot"] = U8( e8 );
            openRootOk = !opened && e8.Contains( "writable by other users" ) && e8.Contains( r8.Path() )
                      && JModeOf( r8.Path() + "/journeys.sqlite3" ) == -1;
         }
         // (p) RemoveDirectoryTree removes links, never what they point to, and
         // a failure is loud (throws, naming the path).
         {
            JTempDir r9( "picopilot-store-tree-" );
            const String keep = r9.Path() + "/keep";
            File::CreateDirectory( keep );
            File::WriteTextFile( keep + "/k.txt", "k" );
            const String t = r9.Path() + "/tree";
            File::CreateDirectory( t + "/sub/deeper" );
            File::WriteTextFile( t + "/a.txt", "a" );
            File::WriteTextFile( t + "/sub/deeper/c.txt", "c" );
            (void)::symlink( U8( keep ).c_str(), U8( t + "/dirlink" ).c_str() );
            (void)::symlink( U8( keep + "/k.txt" ).c_str(), U8( t + "/sub/filelink" ).c_str() );
            const String top = r9.Path() + "/toplink";
            (void)::symlink( U8( keep ).c_str(), U8( top ).c_str() );
            String removeError;
            try
            {
               RemoveDirectoryTree( t );
               RemoveDirectoryTree( top );
            }
            catch ( const pcl::Exception& x ) { removeError = x.Message(); }
            const bool removedOk = removeError.IsEmpty() && JModeOf( t ) == -1 && JModeOf( top ) == -1
                                && FileBytes( keep + "/k.txt" ) == "k" && File::DirectoryExists( keep );
            // Loud failure: a read-only subdirectory cannot be emptied.
            const String ro = r9.Path() + "/ro";
            File::CreateDirectory( ro + "/locked" );
            File::WriteTextFile( ro + "/locked/f.txt", "f" );
            ::chmod( U8( ro + "/locked" ).c_str(), 0500 );
            String treeError;
            try { RemoveDirectoryTree( ro ); }
            catch ( const pcl::Exception& x ) { treeError = x.Message(); }
            ::chmod( U8( ro + "/locked" ).c_str(), 0700 );
            d["tree"] = { { "removed", removedOk }, { "removeError", U8( removeError ) },
                          { "victimKept", FileBytes( keep + "/k.txt" ) == "k" }, { "error", U8( treeError ) } };
            treeOk = removedOk && treeError.Contains( ro + "/locked" ) && File::Exists( ro + "/locked/f.txt" );
         }
         // (q) Locked by another program AT OPEN (a rollback-journal EXCLUSIVE
         // lock blocks even the quick_check read): reported as locked -- never
         // as "damaged" -- quickly, file untouched; opens normally once released.
         {
            JTempDir r10( "picopilot-store-openlock-" );
            {
               String e;
               std::unique_ptr<JourneyStore> s = JourneyStore::Open( r10.Path(), e );
               if ( !s )
                  throw Error( "(q) first Open failed: " + e );
               s->CreateJourney( "q", "X", NowIso() );
            }
            const String p = r10.Path() + "/journeys.sqlite3";
            RawDb other( p );
            const bool setup = other.Exec( "PRAGMA journal_mode=DELETE" ) && other.Exec( "BEGIN EXCLUSIVE" );
            const std::string before = FileBytes( p );
            String e10;
            const jclock::time_point t0 = jclock::now();
            const bool opened = JourneyStore::Open( r10.Path(), e10 ) != nullptr;
            const double ms = MsSince( t0 );
            const bool same = FileBytes( p ) == before;
            other.Exec( "COMMIT" );
            String e11;
            std::unique_ptr<JourneyStore> again = JourneyStore::Open( r10.Path(), e11 );
            JourneyRow jq;
            const bool readable = again && again->GetJourney( 1, jq ) && jq.name == "q";
            again.reset();
            d["openLocked"] = { { "setup", setup }, { "message", U8( e10 ) }, { "ms", ms }, { "fileUnchanged", same },
                                { "afterRelease", U8( e11 ) }, { "readable", readable } };
            openLockedOk = setup && !opened && e10.Contains( "locked" ) && !e10.Contains( "damaged" ) && e10.Contains( p )
                        && ms < 1000 && same && readable && e11.IsEmpty();
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      d["verdicts"] = { { "schema", schemaOk }, { "roundTrip", roundTripOk }, { "utf8", utf8Ok }, { "state", stateOk },
                        { "redact", redactOk }, { "retention", retentionOk }, { "damaged", damagedOk }, { "newer", newerOk },
                        { "foreign", foreignOk }, { "locked", lockedOk }, { "iso", isoOk }, { "fileModes", fileModesOk },
                        { "rootLink", rootLinkOk }, { "dbLink", dbLinkOk }, { "openRoot", openRootOk }, { "tree", treeOk },
                        { "openLocked", openLockedOk }, { "offRoot", offRootOk }, { "directRedact", directRedactOk } };
      const bool ok = schemaOk && roundTripOk && utf8Ok && stateOk && redactOk && retentionOk && damagedOk && newerOk
                   && foreignOk && lockedOk && isoOk && fileModesOk && rootLinkOk && dbLinkOk && openRootOk && treeOk
                   && openLockedOk && offRootOk && directRedactOk;
      out["journeyStoreDetail"] = d;
      out["journeyStoreError"] = U8( error );
      out["journeyStoreOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J5: MasterFacts (Task 6) -----------------------------------
   SelfTestSectionMark( "J5 MasterFacts" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool detectOk = false, wbppOk = false, sirilOk = false, iiTableOk = false, redactOk = false, namesOk = false,
           fingerprintOk = false, auxOk = false, hardenOk = false, round1Ok = false;
      String error;
      try
      {
         const FITSKeywordArray wbpp = WbppMasterKeywords();
         const MasterEvidence e1 = DetectMaster( {}, wbpp );
         const MasterEvidence e2 = DetectMaster( { "ImageIntegration", "PixelMath" }, FITSKeywordArray() );
         const MasterEvidence e3 = DetectMaster( { "Script", "DrizzleIntegration" }, FITSKeywordArray() );
         const MasterEvidence e4 = DetectMaster( {}, Kw( { { "HISTORY", "ImageIntegration.numberOfImages: 12" } } ) );
         const MasterEvidence e5 = DetectMaster( {}, Kw( { { "STACKCNT", "24" } } ) );
         const MasterEvidence n1 = DetectMaster( {}, Kw( { { "IMAGETYP", "'Light Frame'" }, { "EXPTIME", "120" } } ) );
         const MasterEvidence n2 = DetectMaster( {}, Kw( { { "IMAGETYP", "'Master Dark'" } } ) );
         const MasterEvidence n3 = DetectMaster( { "PixelMath", "ImageIntegration" }, FITSKeywordArray() );
         const MasterEvidence n4 = DetectMaster( {}, Kw( { { "NCOMBINE", "1" } } ) );
         d["detect"] = { e1.why, e2.why, e3.why, e4.why, e5.why, n1.isMaster, n2.isMaster, n3.isMaster, n4.isMaster };
         detectOk = e1.isMaster && e1.why == "keyword IMAGETYP='Master Light'"
                 && e2.isMaster && e2.why == "history begins with ImageIntegration"
                 && e3.isMaster && e3.why == "history begins with DrizzleIntegration"
                 && e4.isMaster && e4.why == "HISTORY ImageIntegration.numberOfImages"
                 && e5.isMaster && e5.why == "keyword STACKCNT=24"
                 && !n1.isMaster && !n2.isMaster && !n3.isMaster && !n4.isMaster;

         // Ruling 29 (P35): an integration run's auxiliary outputs (rejection /
         // slope maps) carry the same integration-first history as the result.
         // With PICopilotJourneyIntegrationIdInHistory (measured in Task 1 J0 (9))
         // the step's integrationImageId names the RESULT, so any other window
         // is auxiliary. Expectations follow the measured constant, so the test
         // is valid for either value.
         {
            HistoryStep res;
            res.processId = "ImageIntegration";
            res.integrationImageId = "integration";
            HistoryStep script;
            script.processId = "Script";
            HistoryStep pm;
            pm.processId = "PixelMath";
            HistoryStep noId;
            noId.processId = "ImageIntegration";   // integrationImageId not recorded
            const bool m = PICopilotJourneyIntegrationIdInHistory;
            const bool aResult  = IsIntegrationAuxiliary( "integration", { res } );
            const bool aLow     = IsIntegrationAuxiliary( "rejection_low", { res } );
            const bool aScript  = IsIntegrationAuxiliary( "slope", { script, res } );
            const bool aNotInt  = IsIntegrationAuxiliary( "Image07", { pm, res } );
            const bool aNoId    = IsIntegrationAuxiliary( "rejection_high", { noId } );
            const bool aEmpty   = IsIntegrationAuxiliary( "masterLight", {} );
            d["auxiliary"] = { { "measured", m }, { "result", aResult }, { "low", aLow }, { "script", aScript },
                               { "notIntegration", aNotInt }, { "noId", aNoId }, { "empty", aEmpty } };
            auxOk = !aResult && aLow == m && aScript == m && !aNotInt && !aNoId && !aEmpty;
         }

         // WBPP: facts from keywords, target from the WBPP path.
         const String wbppPath = "/mnt/qnap/astro_data/10_9/Autorun/Light/M16/master/"
                                 "masterLight_BIN-1_4944x3284_EXPOSURE-300.00s_FILTER-NoFilter_combined_RGB_drizzle_1x.xisf";
         const AcquisitionFacts a = ExtractAcquisition( wbpp, {}, wbppPath, "masterLight" );
         d["wbpp"] = { { "target", a.target }, { "filter", a.filter }, { "camera", a.camera }, { "sub", a.subExposureS.value_or( -1 ) },
                       { "count", a.subCount.value_or( -1 ) }, { "total", a.totalIntegrationS.value_or( -1 ) }, { "date", a.sessionDate } };
         wbppOk = a.target == "M16" && a.filter == "NoFilter" && a.camera == "ZWO ASI071MC Pro" && a.subExposureS == 300.0
               && a.subCount == 10 && a.totalIntegrationS == 3000.0 && a.sessionDate == "2022-10-09" && !a.gain.has_value();

         // Siril-style stack: STACKCNT + LIVETIME, OBJECT with a space.
         const AcquisitionFacts s = ExtractAcquisition( Kw( { { "OBJECT", "'NGC 7000'" }, { "FILTER", "'Ha'" }, { "STACKCNT", "24" },
                                                              { "LIVETIME", "7200" }, { "EXPTIME", "7200" }, { "GAIN", "100" },
                                                              { "OFFSET", "50" }, { "CCD-TEMP", "-10.0" } } ),
                                                        {}, "/data/ngc7000.fit", "ngc7000" );
         sirilOk = s.target == "NGC 7000" && s.subCount == 24 && s.totalIntegrationS == 7200.0 && s.subExposureS == 300.0
                && s.gain == 100.0 && s.offset == 50.0 && s.sensorTempC == -10.0;
         d["siril"] = { { "target", s.target }, { "sub", s.subExposureS.value_or( -1 ) } };

         // Sub count from an ImageIntegration step's images table (enabled rows only).
         HistoryStep ii;
         ii.processId = "ImageIntegration";
         ii.tableParameters = { { "images", { { true, "/a.fits", "", "" }, { false, "/b.fits", "", "" }, { true, "/c.fits", "", "" } } } };
         const AcquisitionFacts t = ExtractAcquisition( Kw( { { "EXPTIME", "60" } } ), { ii }, "", "integration" );
         iiTableOk = t.subCount == 2 && t.totalIntegrationS == 120.0 && t.target == "integration";

         // Location / observer values never reach any fact (D7).
         {
            const nlohmann::json all = { a.target, a.filter, a.camera, a.sessionDate };
            const std::string dump = all.dump() + s.target + t.target;
            redactOk = dump.find( "40 11 12" ) == std::string::npos && dump.find( "Jane" ) == std::string::npos
                    && KeywordText( wbpp, "SITELAT" ).empty() && KeywordText( wbpp, "OBSERVER" ).empty()
                    && KeywordText( wbpp, "FILTER" ) == "NoFilter";
         }

         namesOk = DeriveJourneyName( "M16", "NoFilter", 1, "2026-09-25T20:47:50.344Z" ) == "M16 NoFilter 2026-09-25"
                && DeriveJourneyName( "M16", "", 1, "2026-09-25T20:47:50.344Z" ) == "M16 2026-09-25"
                && DeriveJourneyName( "M16", "Ha", 3, "2026-09-25T20:47:50.344Z" ) == "M16 3 masters 2026-09-25"
                && StripKind( "Ha", 1 ) == "Ha master" && StripKind( "", 1 ) == "master" && StripKind( "Ha", 3 ) == "3 masters"
                && SafeFolderName( "C\xC3\xB4ne / M42:*" ) == "C_ne___M42__" && SafeFolderName( ".." ) == "journey"
                && SafeFolderName( ".hidden" ) == "_hidden" && SafeFolderName( std::string( 100, 'a' ) ).size() == 60
                && DeriveTarget( Kw( {} ), "/x/y/Pelican.xisf", "v" ) == "Pelican" && DeriveTarget( Kw( {} ), "", "Image07" ) == "Image07";

         // Fingerprint: stable across added processing, sensitive to geometry/keywords/base history.
         const std::string f0 = MasterFingerprint( 4944, 3284, 3, 32, true, {}, wbpp );
         FITSKeywordArray wbpp2 = wbpp;
         wbpp2 << FITSHeaderKeyword( "HISTORY", "", "PixelMath: something later" );   // not a stable keyword
         const std::string f1 = MasterFingerprint( 4944, 3284, 3, 32, true, {}, wbpp2 );
         const std::string f2 = MasterFingerprint( 4944, 3284, 1, 32, true, {}, wbpp );
         const std::string f3 = MasterFingerprint( 4944, 3284, 3, 32, true, { "ImageIntegration@t#0123456789abcdef" }, wbpp );
         d["fingerprint"] = { f0, f1, f2, f3 };
         fingerprintOk = f0 == f1 && f0 != f2 && f0 != f3 && f0.rfind( "4944x3284x3:f32:", 0 ) == 0 && f0.size() == 16 + 16;

         // Hardening (Task 6 addition): a keyword that is not a real number
         // is not a fact. "inf" is no frame count and no exposure; a count an
         // int cannot hold is unknown (not UB); an images table with no enabled
         // row gives no count (unknown, not 0 frames / 0 s).
         {
            const MasterEvidence hInf = DetectMaster( {}, Kw( { { "STACKCNT", "inf" } } ) );
            const AcquisitionFacts hExp = ExtractAcquisition( Kw( { { "EXPTIME", "inf" }, { "NCOMBINE", "4" } } ), {}, "", "v" );
            const AcquisitionFacts hBig = ExtractAcquisition( Kw( { { "NCOMBINE", "1e12" }, { "EXPTIME", "60" } } ), {}, "", "v" );
            HistoryStep off;
            off.processId = "ImageIntegration";
            off.tableParameters = { { "images", { { false, "/a.fits", "", "" }, { false, "/b.fits", "", "" } } } };
            const AcquisitionFacts hOff = ExtractAcquisition( Kw( { { "EXPTIME", "60" } } ), { off }, "", "v" );
            d["harden"] = { { "infMaster", hInf.isMaster }, { "infExposure", hExp.subExposureS.has_value() },
                            { "infCount", hExp.subCount.value_or( -1 ) }, { "bigCount", hBig.subCount.has_value() },
                            { "offCount", hOff.subCount.has_value() }, { "offTotal", hOff.totalIntegrationS.has_value() } };
            hardenOk = !hInf.isMaster && !hExp.subExposureS && !hExp.totalIntegrationS && hExp.subCount == 4
                    && !hBig.subCount && !hBig.totalIntegrationS && hBig.subExposureS == 60.0
                    && !hOff.subCount && !hOff.totalIntegrationS && hOff.subExposureS == 60.0;
         }

         // Review round 1 (Task 6).
         {
            // Important 1: master evidence and the frame count share one
            // definition of a count, so they can never disagree.
            const FITSKeywordArray frac = Kw( { { "NCOMBINE", "2.5" } } );
            const FITSKeywordArray whole = Kw( { { "NCOMBINE", "3" } } );
            const MasterEvidence mFrac = DetectMaster( {}, frac );
            const MasterEvidence mWhole = DetectMaster( {}, whole );
            const AcquisitionFacts aFrac = ExtractAcquisition( frac, {}, "", "v" );
            const AcquisitionFacts aWhole = ExtractAcquisition( whole, {}, "", "v" );
            const bool countAgreeOk = !mFrac.isMaster && !aFrac.subCount
                                   && mWhole.isMaster && mWhole.why == "keyword NCOMBINE=3" && aWhole.subCount == 3;

            // Minor 3: a present but non-ISO DATE-OBS (a Julian date) falls back to DATE-LOC.
            const AcquisitionFacts aJd = ExtractAcquisition( Kw( { { "DATE-OBS", "2459861.53343" },
                                                                   { "DATE-LOC", "'2022-10-08T20:48:08'" } } ), {}, "", "v" );
            const bool dateOk = aJd.sessionDate == "2022-10-08";

            // Minor 4: an unpaired UTF-8 continuation byte becomes its own '_';
            // a well-formed sequence is still one '_' per code point.
            const std::string sUnpaired = SafeFolderName( "a\xB4" "b" );
            const std::string sEuro = SafeFolderName( "x\xE2\x82\xAC" "y" );
            const std::string sTrunc = SafeFolderName( "p\xC3" "q" );   // lead byte cut short by ASCII
            const bool utf8Ok = sUnpaired == "a_b" && sEuro == "x_y" && sTrunc == "p_q"
                             && SafeFolderName( "C\xC3\xB4ne" ) == "C_ne";

            // Minor 6: OBJECT wins over the WBPP .../<target>/master/<file> path rule.
            const std::string tObj = DeriveTarget( Kw( { { "OBJECT", "'M8'" } } ), "/d/M16/master/masterLight.xisf", "v" );
            const bool objectWinsOk = tObj == "M8";

            d["round1"] = { { "fracMaster", mFrac.isMaster }, { "fracCount", aFrac.subCount.value_or( -1 ) },
                            { "wholeWhy", mWhole.why }, { "wholeCount", aWhole.subCount.value_or( -1 ) },
                            { "jdDate", aJd.sessionDate }, { "unpaired", sUnpaired }, { "euro", sEuro },
                            { "trunc", sTrunc }, { "objectTarget", tObj } };
            round1Ok = countAgreeOk && dateOk && utf8Ok && objectWinsOk;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      const bool ok = detectOk && wbppOk && sirilOk && iiTableOk && redactOk && namesOk && fingerprintOk && auxOk && hardenOk && round1Ok;
      out["masterFactsDetail"] = d;
      out["masterFactsError"] = U8( error );
      out["masterFactsOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J6: JourneyTracker + JourneyService (Task 7) ---------------
   // The tracker checks ran in the j6 fixture phases (J6State, see the helpers);
   // this section adds the production-service and settings checks, reports all
   // verdicts, and closes every window the checks made.
   SelfTestSectionMark( "J6 JourneyTracker + JourneyService" );
   {
      J6State& st = J6();
      nlohmann::json d = st.d;
      bool serviceOk = false, settingsOk = false;
      String error;
      try
      {
         // (a) The PRODUCTION service recorded the pre-phase with the panel never opened (flushed and
         //     paused by the j6 "service" step right after the pre-phase).
         {
            JourneyService& svc = JourneyService::Instance();
            const char* xdg = std::getenv( "XDG_DATA_HOME" );
            JourneyStore* s = svc.Store();
            int64 jid = 0, img = 0;
            nlohmann::json links = nlohmann::json::array();
            if ( s != nullptr )
               for ( const JourneyRow& j : s->ListJourneys( false, "PreM42", 5 ) )
               {
                  jid = j.id;
                  for ( const ImageRow& i : s->Images( jid ) )
                     if ( i.isMaster )
                        img = i.id;
                  for ( const LinkRow& l : s->Links( jid ) )
                     links.push_back( l.evidence );
               }
            d["service"] = { { "root", U8( JourneyService::LibraryRoot() ) }, { "storeError", U8( svc.StoreError() ) },
                             { "flushedAfterPrePhase", st.serviceFlushed }, { "journey", jid },
                             { "active", s != nullptr && img != 0 ? ActiveSteps( *s, img ) : -1 }, { "links", links } };
            serviceOk = svc.Started() && st.serviceFlushed && s != nullptr && xdg != nullptr
                     && JourneyService::LibraryRoot() == String( xdg ) + "/PICopilot/journeys"
                     && jid != 0 && img != 0 && ActiveSteps( *s, img ) == 2 && links.size() == 1 && links[0] == "timing";
            if ( s != nullptr )
            {
               // Diagnostics: every journey the production service recorded, and what it says of the new window.
               nlohmann::json all = nlohmann::json::array();
               for ( const JourneyRow& j : s->ListJourneys( false, "", 20 ) )
               {
                  nlohmann::json imgs = nlohmann::json::array(), ls = nlohmann::json::array();
                  for ( const ImageRow& i : s->Images( j.id ) )
                     imgs.push_back( { i.viewId, i.isMaster } );
                  for ( const LinkRow& l : s->Links( j.id ) )
                     ls.push_back( { l.fromImageId, l.toImageId, l.evidence } );
                  all.push_back( { { "id", j.id }, { "name", j.name }, { "images", imgs }, { "links", ls } } );
               }
               const JourneyStatus ns = svc.Tracker().StatusFor( "pcJourneyPreNew" );
               d["serviceAll"] = all;
               d["serviceNew"] = { { "state", int( ns.state ) }, { "why", ns.why }, { "reason", U8( ns.reason ) },
                                   { "journey", ns.journeyId } };
               d["serviceDecisions"] = svc.Tracker().RecentDecisionsForSelfTest();
            }
            bool redact = false;
            if ( s != nullptr )
            {
               s->Checkpoint();
               redact = FileBytes( s->DbPath() ).find( "40 11 12" ) == std::string::npos
                     && FileBytes( s->DbPath() + "-wal" ).find( "40 11 12" ) == std::string::npos;
            }
            d["serviceRedact"] = redact;
            st.ok["redact"] = st.ok.count( "redact" ) > 0 && st.ok["redact"] && redact;
         }

         // Settings data layer: defaults and clamps.
         {
            Settings::Remove( "PICopilot/RecordJourneys" );
            Settings::Remove( "PICopilot/JourneyExportFolder" );
            Settings::Remove( "PICopilot/JourneyRetentionDays" );
            const bool defaults = CopilotSettings::LoadRecordJourneys() && CopilotSettings::LoadJourneyExportFolder().IsEmpty()
                               && CopilotSettings::LoadJourneyRetentionDays() == 30;
            CopilotSettings::SaveJourneyRetentionDays( 0 );
            const int low = CopilotSettings::LoadJourneyRetentionDays();
            CopilotSettings::SaveJourneyRetentionDays( 99999 );
            const int high = CopilotSettings::LoadJourneyRetentionDays();
            CopilotSettings::SaveRecordJourneys( false );
            const bool off = !CopilotSettings::LoadRecordJourneys();
            CopilotSettings::SaveJourneyExportFolder( "  /tmp/x  " );
            const bool trimmed = CopilotSettings::LoadJourneyExportFolder() == "/tmp/x";
            Settings::Remove( "PICopilot/RecordJourneys" );
            Settings::Remove( "PICopilot/JourneyExportFolder" );
            Settings::Remove( "PICopilot/JourneyRetentionDays" );
            d["settings"] = { defaults, low, high, off, trimmed };
            settingsOk = defaults && low == 1 && high == 3650 && off && trimmed;
         }
         st.ok["settings"] = settingsOk;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      // Every fixture step ran (a harness error stops the block early; that is a J6 failure too).
      const bool complete = !st.steps.empty() && st.steps.front() == "service" && st.steps.back() == "end";
      // The fixture closed its windows from JS; this only catches any left behind by an early harness
      // error. pcJourneyPre/pcJourneyPreNew stay open until exit, as before Task 7.
      for ( const std::string& id : st.made )
         JForceClose( id );
      JourneyService::Instance().SetNotificationForwardForSelfTest( nullptr );
      st.trk.reset();
      st.store.reset();
      st.frames.reset();
      st.root.reset();

      nlohmann::json checks = { { "service", serviceOk } };
      bool ok = serviceOk && complete && error.IsEmpty() && st.errors.IsEmpty();
      for ( const char* c : kJ6Checks )
      {
         const bool v = st.ok.count( c ) > 0 && st.ok[c];
         checks[c] = v;
         ok = ok && v;
      }
      nlohmann::json errors = nlohmann::json::array();
      for ( const String& e : st.errors )
         errors.push_back( U8( e ) );
      d["steps"] = st.steps;
      d["stepErrors"] = errors;
      d["complete"] = complete;
      out["journeyTrackerDetail"] = d;
      out["journeyTrackerChecks"] = checks;
      out["journeyTrackerError"] = U8( error );
      out["journeyTrackerOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J7: JourneyExport (Task 8) ---------------------------------
   SelfTestSectionMark( "J7 JourneyExport" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool fixtureOk = false, summaryOk = false, recipeOk = false, validatorOk = false, privacyOk = false, manualOk = false,
           xpsmOk = false, replayOk = false, copyOk = false, missingRootOk = false, readOnlyOk = false,
           relativeOk = false, retryOk = false, independentOk = false, exportThumbsOk = false, pathParamOk = false,
           stripOk = false, modesOk = false, fileParamOk = false, enabledRewriteOk = false, rollbackOk = false,
           referenceOk = false, offRootOk = false, catalogFailOk = false;
      String error;
      std::vector<std::string> made = { "pcExpM" };
      // Owned here from now on: the store goes first, then its folder (reverse declaration order).
      std::unique_ptr<JTempDir> root = std::move( J7().root );
      std::unique_ptr<JourneyStore> store = std::move( J7().store );
      try
      {
         // The recorded journey (phase j7.exp): keyword master + PixelMath, HistogramTransformation, PixelMath (by hand).
         d["fixture"] = { { "error", J7().error }, { "recorded", J7().recorded }, { "phases", J7().detail } };
         fixtureOk = store != nullptr && J7().error.empty() && J7().recorded == 3;
         if ( !fixtureOk )
            throw Error( "the j7.exp fixture did not record the journey: " + FromU8( J7().error ) );
         const int64 jid = J7().jid;
         const int64 mimg = J7().mimg;

         // A stored Script step (manual; its path must not leave the machine) and a masked step, as the
         // tracker would store them, on a second, derived image of the same journey.
         const int64 img2 = store->AddImage( jid, "pcExpM_starless", "/home/u/data/starless.xisf", "fp-x", false, NowIso() );
         store->AddLink( { mimg, img2, 0, "timing" } );
         HistoryStep scr, masked;
         String pe;
         ParseXpsmStep( "<instance class=\"Script\" version=\"256\" id=\"Script_instance\">"
                        "<parameter id=\"filePath\">/home/u/scripts/FixStars.js</parameter><parameter id=\"md5sum\">ab</parameter>"
                        "<table id=\"parameters\" rows=\"0\"/><parameter id=\"information\"></parameter></instance>", scr, pe );
         scr.combinedIndex = 0;
         store->AddStep( MakeStepRow( scr, img2, "active", "user", "", 1 ) );
         ParseXpsmStep( kXpsmPixelMath, masked, pe );
         masked.combinedIndex = 1;
         masked.maskId = "pcExpMask";
         masked.maskInverted = true;
         store->AddStep( MakeStepRow( masked, img2, "active", "copilot", "protect the stars", 2 ) );
         store->AddGap( { jid, img2, 2, "history read of pcExpM_starless failed: test" } );

         // (a) Summary shown before keeping.
         const KeeperSummary ks = BuildKeeperSummary( *store, jid );
         const String html = KeeperSummaryHtml( ks );
         d["summary"] = { { "masters", ks.masters }, { "images", ks.images }, { "steps", ks.steps }, { "copilot", ks.copilotSteps },
                          { "masterLines", ks.masterLines }, { "links", ks.linkLines }, { "gaps", ks.gapLines } };
         summaryOk = ks.masters == 1 && ks.images == 2 && ks.steps == 5 && ks.copilotSteps == 1 && ks.linkLines.size() == 1
                  && ks.linkLines[0].find( "linked by timing" ) != std::string::npos && ks.gapLines.size() == 1
                  && ks.masterLines.size() == 1 && ks.masterLines[0] == "ExpM42 (Ha, 20 x 300 s)"
                  && html.Contains( "timing" ) && html.Contains( "ExpM42" ) && !ks.alreadyKept;

         // (b) recipe.json: shape, only active non-base steps, stats before/after, achieved medians.
         store->MarkKept( jid, mimg, NowIso() );
         const nlohmann::json recipe = BuildRecipe( *store, jid, "PI Copilot test" );
         std::string why;
         const bool valid = ValidateRecipe( recipe, why );
         d["recipeWhy"] = why;
         const nlohmann::json& steps = recipe.at( "steps" );
         d["recipeSteps"] = nlohmann::json::array();
         for ( const nlohmann::json& s : steps )
            d["recipeSteps"].push_back( { s.at( "processId" ), s.at( "seq" ), s.at( "manual" ) } );
         recipeOk = valid && recipe.at( "schema" ) == "picopilot-recipe" && recipe.at( "schemaVersion" ) == 1
                 && recipe.at( "images" ).size() == 2 && recipe.at( "images" ).at( 0 ).at( "isMaster" ) == true
                 && recipe.at( "images" ).at( 0 ).at( "acquisition" ).at( "subCount" ) == 20
                 && steps.size() == 5 && steps.at( 0 ).at( "processId" ) == "PixelMath"
                 && steps.at( 1 ).at( "tableParameters" ).at( "H" ).at( 3 ).at( 1 ) == 0.3
                 && steps.at( 0 ).at( "statsBefore" ).is_array() && steps.at( 2 ).at( "statsAfter" ).is_array()
                 && steps.at( 2 ).at( "achieved" ).at( "median" ).at( "to" ).size() == 1
                 && recipe.at( "links" ).at( 0 ).at( "evidence" ) == "timing" && recipe.at( "gaps" ).size() == 1
                 && recipe.at( "journey" ).at( "endImage" ) == "img" + std::to_string( mimg );

         // (c) The validator rejects what the schema rejects, naming the place.
         {
            auto bad = [&recipe]( const std::function<void( nlohmann::json& )>& mutate, const char* expect )
            {
               nlohmann::json r = recipe;
               mutate( r );
               std::string w;
               return !ValidateRecipe( r, w ) && w.find( expect ) != std::string::npos;
            };
            validatorOk = bad( []( nlohmann::json& r ) { r.erase( "schemaVersion" ); }, "schemaVersion" )
                       && bad( []( nlohmann::json& r ) { r["schemaVersion"] = 2; }, "schemaVersion" )
                       && bad( []( nlohmann::json& r ) { r["steps"][0]["actor"] = "robot"; }, "steps[0].actor" )
                       && bad( []( nlohmann::json& r ) { r["steps"][0]["image"] = "img999999"; }, "steps[0].image" )
                       && bad( []( nlohmann::json& r ) { r["links"][0]["to"] = "img999999"; }, "links[0].to" )
                       && bad( []( nlohmann::json& r ) { r["steps"][1]["seq"] = 0; }, "steps[1].seq" )
                       && bad( []( nlohmann::json& r ) { r["steps"][0]["statsAfter"] = "x"; }, "steps[0].statsAfter" )
                       && bad( []( nlohmann::json& r ) { r["images"][0]["key"] = "M42"; }, "images[0].key" )
                       // Type checks the schema declares (pre-flight P27).
                       && bad( []( nlohmann::json& r ) { r["journey"]["name"] = 5; }, "journey.name" )
                       && bad( []( nlohmann::json& r ) { r["images"][0]["acquisition"]["subCount"] = "20"; }, "images[0].acquisition.subCount" )
                       && bad( []( nlohmann::json& r ) { r["steps"][4]["mask"]["inverted"] = "yes"; }, "steps[4].mask.inverted" )
                       && bad( []( nlohmann::json& r ) { r["steps"][2]["achieved"]["median"]["to"] = nlohmann::json::array( { "x" } ); },
                               "steps[2].achieved.median.to" )
                       && bad( []( nlohmann::json& r ) { r["gaps"][0]["afterSeq"] = "2"; }, "gaps[0].afterSeq" )
                       && bad( []( nlohmann::json& r ) { r["gaps"][0]["reason"] = nullptr; }, "gaps[0].reason" )
                       // Wrong container types never reach a lookup (no exception, a named refusal).
                       && bad( []( nlohmann::json& r ) { r["journey"] = nlohmann::json::array(); }, "journey" )
                       && bad( []( nlohmann::json& r ) { r["steps"][0]["statsBefore"][0]["channel"] = -1; }, "steps[0].statsBefore[0].channel" )
                       && bad( []( nlohmann::json& r ) { r["journey"]["endImage"] = "img999999"; }, "journey.endImage" )
                       && nlohmann::json::parse( RecipeSchemaText() ).at( "properties" ).at( "schemaVersion" ).at( "const" ) == 1
                       && nlohmann::json::parse( RecipeSchemaText() ).at( "properties" ).at( "schema" ).at( "const" ) == PICopilotRecipeSchemaId;
         }

         // (d) Privacy: no directory, no location value, anywhere in the recipe.
         {
            const std::string dump = recipe.dump();
            privacyOk = dump.find( "/home/" ) == std::string::npos && dump.find( "40 11 12" ) == std::string::npos
                     && steps.at( 3 ).at( "parameters" ).at( "filePath" ) == "FixStars.js"
                     && recipe.at( "images" ).at( 1 ).at( "fileName" ) == "starless.xisf";
         }

         // (e) Manual steps (Ruling 16): the script and the masked step, with the reason.
         manualOk = steps.at( 3 ).at( "manual" ) == true && steps.at( 3 ).at( "manualWhy" ).get<std::string>().find( "FixStars.js" ) != std::string::npos
                 && steps.at( 3 ).at( "manualWhy" ).get<std::string>().find( "/home/" ) == std::string::npos
                 && steps.at( 4 ).at( "manual" ) == true && steps.at( 4 ).at( "manualWhy" ).get<std::string>().find( "pcExpMask" ) != std::string::npos
                 && steps.at( 0 ).at( "manual" ) == false && IsManualProcess( "DynamicBackgroundExtraction" )
                 && !IsManualProcess( "AutomaticBackgroundExtractor" );

         // (e2) PrivacyStripPaths turns only real absolute paths into file names: a PixelMath
         //      expression that starts with '~' (inversion) or a comment is left alone.
         {
            const nlohmann::json in = { { "a", "/home/u/x/y.fits" }, { "b", "~/data/z.xisf" }, { "c", "C:\\astro\\m42.fit" },
                                        { "d", "~$T/2" }, { "e", "/* stretch */ $T*2" }, { "f", "$T/2" },
                                        { "g", nlohmann::json::array( { "/opt/m.pb", 3, true } ) }, { "h", { { "i", "/a/b/c.js" } } } };
            const nlohmann::json o = PrivacyStripPaths( in );
            d["strip"] = o;
            stripOk = o.at( "a" ) == "y.fits" && o.at( "b" ) == "z.xisf" && o.at( "c" ) == "m42.fit" && o.at( "d" ) == "~$T/2"
                   && o.at( "e" ) == "/* stretch */ $T*2" && o.at( "f" ) == "$T/2" && o.at( "g" ).at( 0 ) == "m.pb"
                   && o.at( "g" ).at( 1 ) == 3 && o.at( "h" ).at( "i" ) == "c.js";
         }

         // (f) Keeper files: .xpsm (well-formed, containers per image, icons) + recipe.json + recipe.schema.json.
         const KeeperFilesResult kf = WriteKeeperFiles( *store, jid, "PI Copilot test" );
         JourneyRow jr;
         store->GetJourney( jid, jr );
         const String xpsmPath = kf.dir + "/" + ExportBaseName( jr ) + ".xpsm";
         int containers = 0, icons = 0;
         {
            XMLDocument doc;
            doc.Parse( FromU8( FileBytes( xpsmPath ) ) );
            if ( doc.RootElement() != nullptr )
               for ( const XMLElement& e : doc.RootElement()->ChildElements() )
               {
                  if ( e.Name() == "instance" && e.AttributeValue( "class" ) == "ProcessContainer" ) ++containers;
                  if ( e.Name() == "icon" ) ++icons;
               }
         }
         // The Script step is a comment naming only FixStars.js; no directory anywhere in the .xpsm (P5).
         const std::string xpsmText = FileBytes( xpsmPath );
         d["files"] = { { "dir", U8( kf.dir ) }, { "xpsmError", U8( kf.xpsmError ) }, { "recipeError", U8( kf.recipeError ) },
                        { "thumbsError", U8( kf.thumbsError ) }, { "containers", containers }, { "icons", icons } };
         xpsmOk = kf.xpsmOk && kf.recipeOk && containers == 2 && icons == 2
               && File::Exists( kf.dir + "/recipe.json" ) && File::Exists( kf.dir + "/recipe.schema.json" )
               && nlohmann::json::parse( FileBytes( kf.dir + "/recipe.json" ) ) == recipe
               && FileBytes( kf.dir + "/recipe.schema.json" ) == RecipeSchemaText()
               && xpsmText.find( "/home/" ) == std::string::npos && xpsmText.find( "class=\"Script\"" ) == std::string::npos
               && xpsmText.find( "FixStars.js" ) != std::string::npos;
         // Exported files are shared (0644 at most, umask applied); export/ and export/thumbs/ are ours (0700).
         {
            const int m = JModeOf( xpsmPath ), mr = JModeOf( kf.dir + "/recipe.json" ), md = JModeOf( kf.dir ),
                      mt = JModeOf( kf.dir + "/thumbs" );
            d["modes"] = { { "xpsm", m }, { "recipe", mr }, { "export", md }, { "thumbs", mt } };
            const int want = 0644 & ~int( JProcUmask() );
            modesOk = m == want && mr == want && md == 0700 && mt == 0700;
         }
         // Thumbnails travel inside export/, so the recipe's (and journey.md's) relative links resolve there (P25).
         {
            const nlohmann::json& t0 = recipe.at( "images" ).at( 0 ).at( "thumbnail" );
            const nlohmann::json& t2 = steps.at( 2 ).at( "thumbnail" );
            const String stepThumb = kf.dir + String().Format( "/thumbs/%lld.jpg", static_cast<long long>( steps.at( 2 ).at( "id" ).get<int64>() ) );
            d["thumbs"] = { { "t0", t0 }, { "t2", t2 } };
            exportThumbsOk = kf.thumbsOk && t0.is_string() && t2.is_string()
                          && File::Exists( kf.dir + "/" + FromU8( t0.get<std::string>() ) )
                          && FromU8( t2.get<std::string>() ) == stepThumb.Substring( kf.dir.Length() + 1 )
                          && File::Exists( stepThumb );
         }

         // (g) The .xpsm replays on a fresh copy of the master to the same pixels (Ruling 13).
         //     Every step must be a full ApplyProcess success (ok, i.e. also recorded in History).
         {
            JMakeNoiseMaster( "pcExpCopy", 31 );
            made.push_back( "pcExpCopy" );
            XMLDocument doc;
            doc.Parse( FromU8( xpsmText ) );
            int applied = 0;
            String replayError;
            const XMLElement* first = nullptr;
            if ( doc.RootElement() != nullptr )
               for ( const XMLElement& e : doc.RootElement()->ChildElements() )
                  if ( e.Name() == "instance" && e.AttributeValue( "class" ) == "ProcessContainer" )
                  {
                     first = &e;
                     break;
                  }
            if ( first == nullptr )
               replayError = "no ProcessContainer in the .xpsm";
            else
               for ( const XMLElement& inst : first->ChildElements() )
               {
                  HistoryStep hs;
                  String e;
                  if ( !ParseXpsmElement( inst, hs, e ) )
                  {
                     replayError = e;
                     break;
                  }
                  const ApplyProcessResult ar = ApplyProcess( IsoString( hs.processId.c_str() ), hs.parameters, hs.tableParameters,
                                                              ImageWindow::WindowById( "pcExpCopy" ).MainView() );
                  if ( !ar.ok )
                  {
                     replayError = ar.error;
                     break;
                  }
                  ++applied;
               }
            const double diff = JMaxAbsDiff( ImageWindow::WindowById( "pcExpM" ).MainView(),
                                             ImageWindow::WindowById( "pcExpCopy" ).MainView() );
            d["replay"] = { { "applied", applied },{ "maxAbsDiff", diff }, { "error", U8( replayError ) } };
            replayOk = applied == 3 && diff <= 1e-6 && replayError.IsEmpty();
         }

         // (h) Export copy into an existing folder.
         JTempDir exportRoot( "picopilot-exp-out-" );
         String copiedTo;
         const String c1 = CopyKeeperToExportFolder( *store, jid, exportRoot.Path(), copiedTo );
         d["copy"] = { { "error", U8( c1 ) }, { "to", U8( copiedTo ) } };
         copyOk = c1.IsEmpty() && copiedTo.StartsWith( exportRoot.Path() + "/ExpM42/" )
               && File::Exists( copiedTo + "/recipe.json" ) && File::Exists( copiedTo + "/" + ExportBaseName( jr ) + ".xpsm" )
               && File::DirectoryExists( copiedTo + "/thumbs" )
               && FileBytes( copiedTo + "/recipe.json" ) == FileBytes( kf.dir + "/recipe.json" )
               && File::Exists( copiedTo + "/" + FromU8( steps.at( 2 ).at( "thumbnail" ).get<std::string>() ) );

         // (i) Unmounted NAS: a missing root is reported and NOT created (Ruling 18, Review Focus 4).
         {
            String to;
            const String e = CopyKeeperToExportFolder( *store, jid, "/nonexistent-picopilot-nas/astro_data/keepers", to );
            missingRootOk = e.Contains( "/nonexistent-picopilot-nas/astro_data/keepers" ) && e.Contains( "does not exist" )
                         && !File::DirectoryExists( "/nonexistent-picopilot-nas" ) && to.IsEmpty();
            d["missingRoot"] = U8( e );
         }
         // (j) Read-only root: a named failure, the local keeper stands; (l) retry works once writable.
         {
            JTempDir ro( "picopilot-exp-ro-" );
            ::chmod( U8( ro.Path() ).c_str(), 0555 );
            String to;
            const String e = CopyKeeperToExportFolder( *store, jid, ro.Path(), to );
            readOnlyOk = !e.IsEmpty() && e.Contains( ro.Path() ) && File::Exists( kf.dir + "/recipe.json" ) && to.IsEmpty();
            ::chmod( U8( ro.Path() ).c_str(), 0755 );
            String to2;
            retryOk = CopyKeeperToExportFolder( *store, jid, ro.Path(), to2 ).IsEmpty() && File::Exists( to2 + "/recipe.json" );
            d["readOnly"] = U8( e );
         }
         // (k) A relative export folder is refused (never resolved against PI's working directory).
         {
            String to;
            relativeOk = CopyKeeperToExportFolder( *store, jid, "keepers", to ).Contains( "absolute" );
         }
         // (m) Independence: a failing .xpsm write does not stop recipe.json.
         {
            const String bad = kf.dir + "/" + ExportBaseName( jr ) + ".xpsm";
            File::Remove( bad );
            File::CreateDirectory( bad );   // a directory where the file must go: the write fails
            const KeeperFilesResult k2 = WriteKeeperFiles( *store, jid, "PI Copilot test" );
            File::RemoveDirectory( bad );
            independentOk = !k2.xpsmOk && k2.xpsmError.Contains( ".xpsm" ) && k2.recipeOk && k2.thumbsOk;
            d["independent"] = { { "xpsmError", U8( k2.xpsmError ) }, { "recipeOk", k2.recipeOk } };
         }
         // (n) A replayable step whose parameter names a file (here PixelMath's symbols) would put a
         //     directory into the .xpsm: it is manual, stays out of the icon set, and the recipe keeps
         //     only the file name (the .xpsm never holds a directory, P5).
         {
            std::string x = kXpsmPixelMath;
            const std::string from = "<parameter id=\"symbols\"></parameter>";
            x.replace( x.find( from ), from.size(), "<parameter id=\"symbols\">/home/u/secret/k.txt</parameter>" );
            HistoryStep ps;
            ParseXpsmStep( x, ps, pe );
            ps.combinedIndex = 0;
            const int64 j2 = store->CreateJourney( "path parameter", "PathT", NowIso() );
            const int64 i2 = store->AddImage( j2, "pcExpPath", "", "fp-p", true, NowIso() );
            store->AddStep( MakeStepRow( ps, i2, "active", "user", "", 1 ) );
            const std::string px = BuildJourneyXpsm( *store, j2 );
            const nlohmann::json pr = BuildRecipe( *store, j2, "PI Copilot test" );
            std::string pw;
            const nlohmann::json& s0 = pr.at( "steps" ).at( 0 );
            d["pathParam"] = { { "manualWhy", s0.at( "manualWhy" ) }, { "symbols", s0.at( "parameters" ).at( "symbols" ) } };
            pathParamOk = ps.replayable && px.find( "/home/" ) == std::string::npos && px.find( "k.txt" ) != std::string::npos
                       && px.find( "class=\"PixelMath\"" ) == std::string::npos
                       && ValidateRecipe( pr, pw ) && s0.at( "manual" ) == true && s0.at( "parameters" ).at( "symbols" ) == "k.txt"
                       && s0.at( "manualWhy" ).get<std::string>().find( "k.txt" ) != std::string::npos
                       && pr.dump().find( "/home/" ) == std::string::npos;
         }
         // (o) Fix round 1 (review Important + controller ruling): a FILE parameter -- known by its
         //     id (PCL has no file type: *file*, *path*, *directory*, *folder*, *Dir), or by its table
         //     column's id from the installed process's metadata -- whose value has ANY directory
         //     component (relative, or a //server share) is manual and reduced to its file name; a bare
         //     file name stays replayable and untouched; PixelMath expressions are never file values.
         // (p) Review Minor 1: the id -> enabled rewrite finds the real (quote-aware) end of the tag.
         {
            auto row = []( int64 image, int seq, const char* processId, const nlohmann::json& p, const nlohmann::json& t,
                           const std::string& xpsm )
            {
               StepRow r;
               r.imageId = image;
               r.seq = seq;
               r.processId = processId;
               r.params = { { "parameters", p }, { "tableParameters", t }, { "xpsm", xpsm }, { "identity", "" },
                            { "mask", nullptr }, { "replayable", true }, { "parseNote", "" } };
               return r;
            };
            const std::string icOpen = "<instance class=\"ImageCalibration\" version=\"256\" id=\"ImageCalibration_instance\">";
            const int64 j3 = store->CreateJourney( "file parameters", "FileT", NowIso() );
            const int64 i3 = store->AddImage( j3, "pcExpFile", "", "fp-f", true, NowIso() );
            const std::vector<int64> ids = {
               store->AddStep( row( i3, 1, "ImageCalibration", { { "outputDirectory", "//cal-server/share/out" }, { "masterBiasPath", "bias.xisf" } },
                                    nlohmann::json::object(),
                                    icOpen + "<parameter id=\"outputDirectory\">//cal-server/share/out</parameter></instance>" ) ),
               store->AddStep( row( i3, 2, "ImageCalibration", { { "masterBiasPath", "models/bias.xisf" } }, nlohmann::json::object(),
                                    icOpen + "<parameter id=\"masterBiasPath\">models/bias.xisf</parameter></instance>" ) ),
               store->AddStep( row( i3, 3, "ImageIntegration", nlohmann::json::object(),
                                    { { "images", nlohmann::json::array( { nlohmann::json::array( { true, "subs/a.xisf", "", "" } ) } ) } },
                                    "<instance class=\"ImageIntegration\" version=\"256\" id=\"ImageIntegration_instance\">"
                                    "<table id=\"images\" rows=\"1\"><tr><td id=\"enabled\" value=\"true\"/><td id=\"path\">subs/a.xisf</td>"
                                    "</tr></table></instance>" ) ),
               store->AddStep( row( i3, 4, "PixelMath", { { "expression", "//pm-comment/x" }, { "expression1", "models/foo.onnx" } },
                                    nlohmann::json::object(),
                                    "<instance class=\"PixelMath\" version=\"256\" id=\"PixelMath_instance\">"
                                    "<parameter id=\"expression\">//pm-comment/x</parameter>"
                                    "<parameter id=\"expression1\">models/foo.onnx</parameter></instance>" ) ),
               store->AddStep( row( i3, 5, "ImageCalibration", { { "masterBiasPath", "swin2sr_v11.onnx" } }, nlohmann::json::object(),
                                    icOpen + "<parameter id=\"masterBiasPath\">swin2sr_v11.onnx</parameter></instance>" ) ) };
            // (p) on its own journey: a literal '>' in an attribute value is legal XML, but PCL's own XML
            //     parser rejects it (measured: "Unmatched double or single quote"), so this .xpsm is checked
            //     as text; WriteKeeperFiles' parse check would refuse to write it, loudly.
            const int64 j4 = store->CreateJourney( "quoted gt", "GtT", NowIso() );
            const int64 i4 = store->AddImage( j4, "pcExpGt", "", "fp-g", true, NowIso() );
            store->AddStep( row( i4, 1, "PixelMath", { { "expression", "$T" } }, nlohmann::json::object(),
                                 "<instance class=\"PixelMath\" note=\"a>b\" version=\"256\" id=\"PixelMath_instance\">"
                                 "<parameter id=\"expression\">$T</parameter></instance>" ) );
            const std::string gx = BuildJourneyXpsm( *store, j4 );
            const nlohmann::json fr = BuildRecipe( *store, j3, "PI Copilot test" );
            const std::string fx = BuildJourneyXpsm( *store, j3 );
            std::string fw;
            const nlohmann::json& fs = fr.at( "steps" );
            nlohmann::json manual = nlohmann::json::array();
            for ( const nlohmann::json& s : fs )
               manual.push_back( s.at( "manual" ) );
            int inst = 0;
            String parseError;
            try
            {
               XMLDocument doc;
               doc.Parse( FromU8( fx ) );
               for ( const XMLElement& e : doc.RootElement()->ChildElements() )
                  if ( e.Name() == "instance" )
                     inst += int( e.ChildElements().Length() );
            }
            catch ( const pcl::Exception& x )
            {
               parseError = x.Message();   // recorded; inst stays 0 and fails the check
            }
            d["fileParamXpsmParse"] = U8( parseError );
            d["fileParam"] = { { "manual", manual }, { "p0", fs.at( 0 ).at( "parameters" ) }, { "p1", fs.at( 1 ).at( "parameters" ) },
                               { "t2", fs.at( 2 ).at( "tableParameters" ) }, { "p3", fs.at( 3 ).at( "parameters" ) },
                               { "p4", fs.at( 4 ).at( "parameters" ) }, { "instances", inst },
                               { "why0", fs.at( 0 ).at( "manualWhy" ) } };
            fileParamOk = ValidateRecipe( fr, fw ) && fs.size() == 5
                       && manual == nlohmann::json::array( { true, true, true, false, false } )
                       && fs.at( 0 ).at( "parameters" ).at( "outputDirectory" ) == "out"
                       && fs.at( 0 ).at( "parameters" ).at( "masterBiasPath" ) == "bias.xisf"
                       && fs.at( 1 ).at( "parameters" ).at( "masterBiasPath" ) == "bias.xisf"
                       && fs.at( 2 ).at( "tableParameters" ).at( "images" ).at( 0 ).at( 1 ) == "a.xisf"
                       && fs.at( 3 ).at( "parameters" ).at( "expression" ) == "//pm-comment/x"
                       && fs.at( 3 ).at( "parameters" ).at( "expression1" ) == "models/foo.onnx"
                       && fs.at( 4 ).at( "parameters" ).at( "masterBiasPath" ) == "swin2sr_v11.onnx"
                       && fs.at( 0 ).at( "manualWhy" ).get<std::string>().find( "cal-server" ) == std::string::npos
                       && fr.dump().find( "cal-server" ) == std::string::npos && fr.dump().find( "models/bias" ) == std::string::npos
                       && fr.dump().find( "subs/" ) == std::string::npos
                       && fx.find( "cal-server" ) == std::string::npos && fx.find( "models/bias" ) == std::string::npos
                       && fx.find( "subs/" ) == std::string::npos && fx.find( "swin2sr_v11.onnx" ) != std::string::npos
                       && fx.find( "//pm-comment/x" ) != std::string::npos && inst == 2 && parseError.IsEmpty()
                       && IsFileParameter( "ImageCalibration", "outputDirectory" ) && IsFileParameter( "Script", "filePath" )
                       && !IsFileParameter( "PixelMath", "expression" ) && !IsFileParameter( "PixelMath", "symbols" )
                       && !IsFileParameter( "Convolution", "direction" );
            enabledRewriteOk = fx.find( "id=\"PixelMath_instance\"" ) == std::string::npos
                            && fx.find( "id=\"ImageCalibration_instance\"" ) == std::string::npos
                            && gx.find( "note=\"a>b\" version=\"256\" enabled=\"true\"" ) != std::string::npos
                            && gx.find( "id=\"PixelMath_instance\"" ) == std::string::npos;
            (void)ids;
         }
         // (r) Fix round 2 (re-review round 1).
         {
            auto stepOf = []( const char* processId, const nlohmann::json& p, const nlohmann::json& t )
            {
               StepRow r;
               r.processId = processId;
               r.params = { { "parameters", p }, { "tableParameters", t }, { "xpsm", "" }, { "identity", "" },
                            { "mask", nullptr }, { "replayable", true }, { "parseNote", "" } };
               return r;
            };
            // (r1) The explicit list of file-holding ids the name heuristic misses is real: every entry is a
            //      String parameter / String table column of the INSTALLED process, and IsFileParameter knows it.
            nlohmann::json listed = nlohmann::json::array();
            bool listReal = !KnownFileParameterIds().empty();
            for ( const std::string& k : KnownFileParameterIds() )
            {
               const size_t slash = k.find( '/' ), dot = k.find( '.', slash );
               const std::string proc = k.substr( 0, slash );
               const std::string param = k.substr( slash + 1, dot == std::string::npos ? std::string::npos : dot - slash - 1 );
               bool isString = false;
               try
               {
                  const Process P( IsoString( proc.c_str() ) );
                  const ProcessParameter p( P, IsoString( param.c_str() ) );
                  if ( dot == std::string::npos )
                     isString = !p.IsNull() && p.IsString();
                  else if ( !p.IsNull() && p.IsTable() )
                     for ( const ProcessParameter& c : p.TableColumns() )
                        if ( std::string( c.Id().c_str() ) == k.substr( dot + 1 ) )
                           isString = c.IsString();
               }
               catch ( ... )
               {
                  isString = false;   // not installed / unknown id: the entry is not real here
               }
               const bool known = IsFileParameter( proc, k.substr( slash + 1 ) );
               listed.push_back( { k, isString, known } );
               listReal = listReal && isString && known;
            }
            // (r2) StarAlignment.referenceImage and its targets table: a relative path is manual and reduced to its
            //      name; a bare file name or a view id stays replayable.
            const StepRow sa = stepOf( "StarAlignment", { { "referenceImage", "lights/ref.xisf" } },
                                       { { "targets", nlohmann::json::array( { nlohmann::json::array( { true, true, "lights/a.xisf" } ) } ) } } );
            const nlohmann::json saOut = PrivacyStripStepParameters( sa.processId, sa.params["parameters"], sa.params["tableParameters"] );
            const std::string saWhy = ManualWhy( sa );
            const std::string bareWhy = ManualWhy( stepOf( "StarAlignment", { { "referenceImage", "ref.xisf" } }, nlohmann::json::object() ) );
            const std::string viewWhy = ManualWhy( stepOf( "StarAlignment", { { "referenceImage", "pcRef" } }, nlohmann::json::object() ) );
            const std::string fiWhy = ManualWhy( stepOf( "FastIntegration", { { "referenceImage", "//nas/ref.xisf" } }, nlohmann::json::object() ) );
            referenceOk = !saWhy.empty() && saWhy.find( "lights/" ) == std::string::npos
                       && saOut.at( "parameters" ).at( "referenceImage" ) == "ref.xisf"
                       && saOut.at( "tableParameters" ).at( "targets" ).at( 0 ).at( 2 ) == "a.xisf"
                       && bareWhy.empty() && viewWhy.empty() && !fiWhy.empty() && fiWhy.find( "//nas" ) == std::string::npos;
            // (r3) Off the root thread every catalog-reading entry point refuses, loudly (never an empty guess).
            J7CallThread ts( [&sa]() { PrivacyStripStepParameters( sa.processId, sa.params["parameters"], sa.params["tableParameters"] ); } );
            J7CallThread tw( [&sa]() { (void)ManualWhy( sa ); } );
            bool finished = true;
            for ( J7CallThread* t : { &ts, &tw } )
            {
               t->Start();
               finished = t->Wait( 10000 ) && finished;
            }
            const std::string offStrip = U8( ts.error ), offWhy = U8( tw.error );
            offRootOk = finished && ts.threw && tw.threw
                     && offStrip.find( "root thread" ) != std::string::npos && offWhy.find( "root thread" ) != std::string::npos;
            // (r4) A catalog failure while resolving table columns fails CLOSED: the step is manual and every
            //      value with a directory component in that table is reduced to its name. An UNKNOWN process
            //      (not installed) is not a failure: its array rows fall back to the generic check.
            const StepRow ii = stepOf( "ImageIntegration", nlohmann::json::object(),
                                       { { "images", nlohmann::json::array( { nlohmann::json::array( { true, "subs/b.xisf", "", "" } ) } ) } } );
            const StepRow iiBare = stepOf( "ImageIntegration", nlohmann::json::object(),
                                           { { "images", nlohmann::json::array( { nlohmann::json::array( { true, "b.xisf", "", "" } ) } ) } } );
            SetJourneyExportCatalogFailForSelfTest( true );
            std::string failWhy, failBareWhy;
            nlohmann::json failOut;
            try
            {
               failWhy = ManualWhy( ii );
               failBareWhy = ManualWhy( iiBare );
               failOut = PrivacyStripStepParameters( ii.processId, ii.params["parameters"], ii.params["tableParameters"] );
            }
            catch ( const pcl::Exception& x ) { failWhy = "threw: " + U8( x.Message() ); }
            SetJourneyExportCatalogFailForSelfTest( false );
            const std::string unknownWhy = ManualWhy( stepOf( "NoSuchPICopilotProcess", nlohmann::json::object(),
                                                              { { "rows", nlohmann::json::array( { nlohmann::json::array( { "x", 1 } ) } ) } } ) );
            catalogFailOk = failWhy.find( "images" ) != std::string::npos && failWhy.find( "injected" ) != std::string::npos
                         && failWhy.find( "subs/" ) == std::string::npos && !failBareWhy.empty()
                         && failOut.is_object() && failOut.at( "tableParameters" ).at( "images" ).at( 0 ).at( 1 ) == "b.xisf"
                         && unknownWhy.empty() && ManualWhy( iiBare ).empty();
            // The ICC false positive is accepted (privacy-safe: a profile path is manual, a profile NAME stays).
            const bool iccKnown = IsFileParameter( "ICCProfileTransformation", "targetProfile" );
            d["round2"] = { { "listed", listed }, { "saWhy", saWhy }, { "saOut", saOut }, { "bareWhy", bareWhy }, { "viewWhy", viewWhy },
                            { "fiWhy", fiWhy }, { "offStrip", offStrip }, { "offWhy", offWhy }, { "failWhy", failWhy },
                            { "failBareWhy", failBareWhy }, { "failOut", failOut }, { "unknownWhy", unknownWhy },
                            { "iccTargetProfileIsFile", iccKnown } };
            referenceOk = referenceOk && listReal;
         }
         // (q) Review Minor 3: a copy that fails part-way removes exactly what THIS call created --
         //     never a file or folder that was already there -- and says so.
         {
            const String leaf = File::ExtractNameAndExtension( copiedTo );   // from (h): <date>-<name>
            const String stepThumb = FromU8( steps.at( 2 ).at( "thumbnail" ).get<std::string>() );   // thumbs/<n>.jpg
            // (q1) Mid-copy: the destination folder exists with a user file; a later thumbnail's name is taken by a folder.
            JTempDir q1( "picopilot-exp-q1-" );
            const String dest = q1.Path() + "/ExpM42/" + leaf;
            File::CreateDirectory( q1.Path() + "/ExpM42" );
            File::CreateDirectory( dest );
            File::CreateDirectory( dest + "/thumbs" );
            File::CreateDirectory( dest + "/" + stepThumb );                // the conflict
            File::WriteTextFile( dest + "/notes.txt", IsoString( "mine" ) );
            String to1;
            const String e1 = CopyKeeperToExportFolder( *store, jid, q1.Path(), to1 );
            StringList left;
            for ( const char* n : { "/recipe.json", "/recipe.schema.json" } )
               if ( File::Exists( dest + n ) ) left << dest + n;
            if ( File::Exists( dest + "/" + ExportBaseName( jr ) + ".xpsm" ) ) left << "xpsm";
            int thumbFiles = 0;
            {
               DIR* dd = ::opendir( U8( dest + "/thumbs" ).c_str() );
               if ( dd != nullptr )
               {
                  for ( const dirent* e; (e = ::readdir( dd )) != nullptr; )
                     if ( std::string( e->d_name ) != "." && std::string( e->d_name ) != ".." )
                        ++thumbFiles;
                  ::closedir( dd );
               }
            }
            // (q2) First write fails: the two folders this call made are removed again; the root stays empty.
            JTempDir q2( "picopilot-exp-q2-" );
            SetSafeFileWriteFailBeforeRenameForSelfTest( true );
            String to2;
            const String e2 = CopyKeeperToExportFolder( *store, jid, q2.Path(), to2 );
            SetSafeFileWriteFailBeforeRenameForSelfTest( false );
            d["rollback"] = { { "e1", U8( e1 ) }, { "left", left.Length() }, { "thumbEntries", thumbFiles }, { "e2", U8( e2 ) },
                              { "q2Target", File::DirectoryExists( q2.Path() + "/ExpM42" ) } };
            rollbackOk = !e1.IsEmpty() && e1.Contains( stepThumb ) && e1.Contains( "removed" ) && to1.IsEmpty() && left.IsEmpty()
                      && File::Exists( dest + "/notes.txt" ) && File::DirectoryExists( dest + "/" + stepThumb ) && thumbFiles == 1
                      && !e2.IsEmpty() && e2.Contains( "removed" ) && to2.IsEmpty() && !File::DirectoryExists( q2.Path() + "/ExpM42" );
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      for ( const std::string& id : made )
         JForceClose( id );
      store.reset();
      root.reset();
      const bool ok = fixtureOk && summaryOk && recipeOk && validatorOk && privacyOk && manualOk && xpsmOk && replayOk && copyOk
                   && missingRootOk && readOnlyOk && relativeOk && retryOk && independentOk && exportThumbsOk && pathParamOk
                   && stripOk && modesOk && fileParamOk && enabledRewriteOk && rollbackOk
                   && referenceOk && offRootOk && catalogFailOk;
      out["journeyExportDetail"] = d;
      out["journeyExportChecks"] = { { "fixture", fixtureOk }, { "summary", summaryOk }, { "recipe", recipeOk },
                                     { "validator", validatorOk }, { "privacy", privacyOk }, { "manual", manualOk },
                                     { "xpsm", xpsmOk }, { "replay", replayOk }, { "copy", copyOk }, { "missingRoot", missingRootOk },
                                     { "readOnly", readOnlyOk }, { "relative", relativeOk }, { "retry", retryOk },
                                     { "independent", independentOk }, { "exportThumbs", exportThumbsOk },
                                     { "pathParam", pathParamOk }, { "strip", stripOk }, { "modes", modesOk },
                                     { "fileParam", fileParamOk }, { "enabledRewrite", enabledRewriteOk }, { "rollback", rollbackOk },
                                     { "reference", referenceOk }, { "offRoot", offRootOk }, { "catalogFail", catalogFailOk } };
      out["journeyExportError"] = U8( error );
      out["journeyExportOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J8: write-up + keep/retry (Task 9) -------------------------
   SelfTestSectionMark( "J8 keeper write-up" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool condensedOk = false, bodyOk = false, parseOk = false, loopbackOk = false, noKeyOk = false, retryOk = false,
           httpErrorOk = false, guardOk = false, truncatedOk = false, setStoreOk = false;
      bool liveSkipped = true, liveOk = true;
      String error;
      std::vector<std::string> made;
      try
      {
         JTempDir root( "picopilot-wu-" );
         String oe;
         std::unique_ptr<JourneyStore> store = JourneyStore::Open( root.Path(), oe );
         if ( !store )
            throw Error( "store: " + oe );
         made.push_back( "pcWuM" );
         const JBuilt wu = JBuildJourney( *store, "pcWuM", "WuM42", 41 );
         const int64 jid = wu.jid;
         store->MarkKept( jid, wu.img, NowIso() );
         const nlohmann::json recipe = BuildRecipe( *store, jid, "PI Copilot test" );

         // (a) Condensed input: no XPSM, no directories, capped parameters, bounded size.
         {
            const std::string c = CondensedRecipeForWriteup( recipe );
            const nlohmann::json cj = nlohmann::json::parse( c );
            nlohmann::json big = recipe;
            for ( int i = 0; i < 1500; ++i )
            {
               nlohmann::json s = recipe.at( "steps" ).at( 0 );
               s["id"] = 100000 + i;
               s["parameters"]["expression"] = std::string( 2000, 'x' );
               big["steps"].push_back( s );
            }
            const std::string cb = CondensedRecipeForWriteup( big );
            const nlohmann::json cbj = nlohmann::json::parse( cb );
            d["condensed"] = { { "chars", c.size() }, { "bigChars", cb.size() }, { "omitted", cbj.value( "omittedSteps", 0 ) } };
            condensedOk = cj.at( "steps" ).size() == 3 && c.find( "xpsm" ) == std::string::npos && c.find( "/home/" ) == std::string::npos
                       && cj.at( "steps" ).at( 0 ).contains( "median" ) && cj.at( "journey" ).at( "name" ).is_string()
                       && cb.size() <= PICopilotJourneyWriteupInputChars && cbj.value( "omittedSteps", 0 ) > 0
                       && cbj.at( "steps" ).at( 0 ).at( "parameters" ).get<std::string>().size() <= PICopilotJourneyWriteupParamChars + 3;
         }
         // (b) The request: Haiku, non-streamed, 8000 tokens, no tools/thinking, NO image anywhere, no thumbnail bytes.
         {
            const std::string body = BuildMessagesRequestBody( PICopilotJourneyWriteupModel, JourneyWriteupSystemPrompt(),
                                                               JourneyWriteupHistory( recipe ), nlohmann::json(), JourneyWriteupShape() );
            const nlohmann::json b = nlohmann::json::parse( body );
            std::string thumbB64;
            FindFileInfo info;
            for ( File::Find f( store->JourneyDir( jid ) + "/thumbs/*.jpg" ); f.NextItem( info ); )
            {
               const ByteArray bytes = File::ReadFile( store->JourneyDir( jid ) + "/thumbs/" + info.name );
               // bytes [18, 66): a multiple of 3 from the start, so its Base64 is a substring of the whole file's Base64
               if ( bytes.Length() >= 66 )
                  thumbB64 = std::string( IsoString::ToBase64( bytes.Begin() + 18, 48 ).c_str() );
               break;
            }
            d["body"] = { { "model", b.value( "model", "" ) }, { "max_tokens", b.value( "max_tokens", 0 ) }, { "thumbProbe", thumbB64 } };
            bodyOk = b.at( "model" ) == "claude-haiku-4-5" && b.at( "max_tokens" ) == 8000 && !b.contains( "stream" )
                  && !b.contains( "tools" ) && !b.contains( "thinking" ) && !b.contains( "cache_control" )
                  && b.at( "messages" ).size() == 1
                  && body.find( "\"image\"" ) == std::string::npos && body.find( "base64" ) == std::string::npos
                  && !thumbB64.empty() && body.find( thumbB64 ) == std::string::npos
                  && body.find( "CONDENSED_RECIPE_JSON:" ) != std::string::npos;
         }
         // (c) Reply parsing: the LAST json fence; missing or broken fence -> markdown kept, note set.
         {
            const std::string F( 3, '`' );   // a fence, spelled without three literal backticks
            const WriteupReply r1 = ParseWriteupReply( "# T\n\nText.\n" + F + "json\n{\"inferredReasons\":[{\"step\":1,\"reason\":\"a\"}]}\n" + F + "\n"
                                                       "More.\n" + F + "json\n{\"inferredReasons\":[{\"step\":7,\"reason\":\"lift it\"}]}\n" + F + "\n" );
            const WriteupReply r2 = ParseWriteupReply( "# T\n\nNo fence here." );
            const WriteupReply r3 = ParseWriteupReply( "# T\n" + F + "json\n{not json\n" + F + "\n" );
            // Cut off inside the fence (max_tokens): the document keeps the prose, never the open fence or raw JSON.
            const WriteupReply r4 = ParseWriteupReply( "# T\n\nProse stays.\n\n" + F + "json\n{\"inferredReasons\": [{\"step\": 3, \"rea" );
            d["parse"] = { { "r4markdown", r4.markdown }, { "r4note", r4.note } };
            parseOk = r4.ok && r4.markdown == "# T\n\nProse stays.\n" && r4.inferred.empty()
                   && r4.note.find( "cut off" ) != std::string::npos &&
r1.ok && r1.inferred.size() == 1 && r1.inferred[0].first == 7 && r1.inferred[0].second == "lift it"
                   && r1.markdown.find( "\"inferredReasons\":[{\"step\":7" ) == std::string::npos && r1.markdown.find( "More." ) != std::string::npos
                   && r2.ok && r2.markdown == "# T\n\nNo fence here." && !r2.note.empty() && r2.inferred.empty()
                   && r3.ok && !r3.note.empty() && r3.inferred.empty();
         }
         // (d) Loopback end to end: journey.md, the inferred reason stored + in recipe.json, then the export copy.
         const char* wurl = std::getenv( "PICOPILOT_SELFTEST_WRITEUP_URL" );
         if ( wurl == nullptr )
            throw Error( "PICOPILOT_SELFTEST_WRITEUP_URL not set by the harness" );
         {
            JTempDir exportRoot( "picopilot-wu-out-" );
            KeeperExporter k( store.get() );
            const KeepOutcome o = k.Keep( jid, wu.img, "sk-test-loopback", exportRoot.Path(), String( wurl ) );
            StringList notes;
            const bool idle = JWaitKeeper( k, notes, 30 );
            const String dir = ExportDirOf( *store, jid );
            const nlohmann::json after = nlohmann::json::parse( FileBytes( dir + "/recipe.json" ) );
            int inferredSteps = 0;
            for ( const nlohmann::json& s : after.at( "steps" ) )
               if ( s.at( "reasonInferred" ) == true && s.at( "reason" ) == "brighten the faint signal" ) ++inferredSteps;
            std::string why;
            const bool afterValid = ValidateRecipe( after, why );
            String copied;
            FindFileInfo info;
            for ( File::Find f( exportRoot.Path() + "/WuM42/*" ); f.NextItem( info ); )
               if ( info.IsDirectory() && info.name != "." && info.name != ".." )
                  copied = exportRoot.Path() + "/WuM42/" + info.name;
            String allNotes;
            for ( const String& n : notes ) allNotes += n + "\n";
            const int mdMode = JModeOf( dir + "/journey.md" ), markerMode = JModeOf( store->JourneyDir( jid ) + "/.copied-to" );
            d["loopback"] = { { "started", o.writeupStarted }, { "idle", idle }, { "inferred", inferredSteps },
                              { "copied", U8( copied ) }, { "notes", U8( allNotes ) }, { "mdMode", mdMode },
                              { "markerMode", markerMode }, { "recipeWhy", why } };
            loopbackOk = o.alreadyKept && !o.marked && o.files.xpsmOk && o.files.recipeOk && o.writeupStarted && idle
                      && FileBytes( dir + "/journey.md" ).rfind( "# ", 0 ) == 0 && inferredSteps == 1 && afterValid
                      && FileBytes( dir + "/journey.md" ).find( "inferredReasons" ) == std::string::npos
                      && !copied.IsEmpty() && File::Exists( copied + "/journey.md" ) && File::Exists( copied + "/recipe.json" )
                      && FileBytes( copied + "/recipe.json" ) == FileBytes( dir + "/recipe.json" )
                      && File::DirectoryExists( copied + "/thumbs" ) && !File::Exists( copied + "/.copied-to" )
                      && allNotes.Contains( "journey.md" ) && !allNotes.Contains( "sk-test-loopback" )
                      && mdMode == (0644 & ~int( JProcUmask() )) && markerMode == (0600 & ~int( JProcUmask() ));
         }
         // (e) No API key: files + copy now, the write-up named as not written.
         // (f) Retry (re-mark): only the missing journey.md is produced, then the copy is refreshed.
         {
            made.push_back( "pcWuN" );
            const JBuilt wn = JRecordJourney( *store, "pcWuN", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'WuNoKey'" } } ),
                                              43, { { "$T*1.2", "user", "" } } );
            const int64 j2 = wn.jid;
            JTempDir exportRoot( "picopilot-wu-nokey-" );
            KeeperExporter k( store.get() );
            const KeepOutcome o = k.Keep( j2, wn.img, String(), exportRoot.Path(), String( wurl ) );
            const String dir = ExportDirOf( *store, j2 );
            JourneyRow jr1;
            store->GetJourney( j2, jr1 );
            const std::string xpsmBefore = FileBytes( dir + "/" + ExportBaseName( jr1 ) + ".xpsm" );
            d["noKey"] = { { "writeupError", U8( o.writeupError ) }, { "copyDone", o.copyDone }, { "copyError", U8( o.copyError ) } };
            noKeyOk = o.marked && !o.alreadyKept && jr1.kept && jr1.endImageId == wn.img && o.files.xpsmOk && o.files.recipeOk
                   && !o.writeupStarted && o.writeupError.Contains( "API key" ) && o.copyDone && !File::Exists( dir + "/journey.md" )
                   && !xpsmBefore.empty() && !k.Busy();
            const KeepOutcome r = k.Retry( j2, "sk-test-loopback", exportRoot.Path(), String( wurl ) );
            StringList notes;
            const bool idle = JWaitKeeper( k, notes, 30 );
            JourneyRow jr2;
            store->GetJourney( j2, jr2 );
            const std::string xpsmAfter = FileBytes( dir + "/" + ExportBaseName( jr2 ) + ".xpsm" );
            d["retry"] = { { "redone", nlohmann::json::array() } };
            for ( const String& s : r.redone ) d["retry"]["redone"].push_back( U8( s ) );
            // The write-up's copy is the SECOND copy into o.copiedTo: the .copied-to marker (an absolute path)
            // sits in the journey folder, never in export/, so it is not carried out (pre-flight P7).
            d["retry"]["markerInExport"] = File::Exists( dir + "/.copied-to" );
            retryOk = r.alreadyKept && idle && r.redone.Length() == 1 && r.redone[0] == "journey.md" && File::Exists( dir + "/journey.md" )
                   && xpsmAfter == xpsmBefore
                   && File::Exists( store->JourneyDir( j2 ) + "/.copied-to" ) && !File::Exists( dir + "/.copied-to" )
                   && !o.copiedTo.IsEmpty() && File::Exists( o.copiedTo + "/journey.md" ) && !File::Exists( o.copiedTo + "/.copied-to" );
         }
         // (h) An HTTP error from the API: named in the note (verbatim), no journey.md, the copy still runs.
         {
            const char* base = std::getenv( "PICOPILOT_SELFTEST_STREAM_BASE" );
            if ( base == nullptr )
               throw Error( "PICOPILOT_SELFTEST_STREAM_BASE not set by the harness" );
            made.push_back( "pcWuE" );
            const JBuilt we = JRecordJourney( *store, "pcWuE", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'WuErr'" } } ),
                                              45, { { "$T*1.2", "user", "" } } );
            JTempDir exportRoot( "picopilot-wu-err-" );
            KeeperExporter k( store.get() );
            // A non-streamed POST to "/stream-401" is answered 400 ("\"stream\" is not true") by the loopback.
            const KeepOutcome o = k.Keep( we.jid, we.img, "sk-test-loopback", exportRoot.Path(), String( base ) + "/stream-401" );
            StringList notes;
            const bool idle = JWaitKeeper( k, notes, 30 );
            String allNotes;
            for ( const String& n : notes ) allNotes += n + "\n";
            String copied;
            FindFileInfo info;
            for ( File::Find f( exportRoot.Path() + "/WuErr/*" ); f.NextItem( info ); )
               if ( info.IsDirectory() && info.name != "." && info.name != ".." )
                  copied = exportRoot.Path() + "/WuErr/" + info.name;
            d["httpError"] = { { "notes", U8( allNotes ) }, { "copied", U8( copied ) } };
            httpErrorOk = o.writeupStarted && idle && allNotes.Contains( "journey.md was not written" )
                       && allNotes.Contains( "is not true" ) && !File::Exists( ExportDirOf( *store, we.jid ) + "/journey.md" )
                       && !copied.IsEmpty() && File::Exists( copied + "/recipe.json" ) && !File::Exists( copied + "/journey.md" )
                       && !allNotes.Contains( "sk-test-loopback" );
         }
         // (i) The inferred-reason guard (fix round 1, C1): the reply is untrusted. A step of ANOTHER
         //     journey, a PI Copilot step, a step with a stated reason, a step that does not exist and a
         //     malformed id each store nothing and are named in the note; the one valid entry is stored.
         int64 guardJid = 0;
         {
            made.push_back( "pcWuG" );
            made.push_back( "pcWuF" );
            const JBuilt g = JRecordJourney( *store, "pcWuG", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'WuGuard'" } } ), 47,
                                             { { "$T*1.2", "user", "" }, { "$T*1.1", "copilot", "stretch gently" },
                                               { "$T+0.01", "user", "lift the floor" } } );
            const JBuilt f = JRecordJourney( *store, "pcWuF", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'WuForeign'" } } ), 49,
                                             { { "$T*1.2", "user", "" } } );
            guardJid = g.jid;
            const std::vector<StepRow> gs = store->Steps( g.img, false ), fs = store->Steps( f.img, false );
            if ( gs.size() != 3 || fs.size() != 1 )
               throw Error( "guard fixture: unexpected step counts" );
            const int64 sU = gs[0].id, sC = gs[1].id, sR = gs[2].id, sF = fs[0].id, sMissing = sF + 1000000;
            const nlohmann::json entries = nlohmann::json::array( {
               { { "step", sU }, { "reason", "accepted reason" } },          // 1: valid
               { { "step", sF }, { "reason", "foreign" } },                  // 2: another journey
               { { "step", sC }, { "reason", "copilot" } },                  // 3: a PI Copilot step
               { { "step", sR }, { "reason", "overwrite" } },                // 4: already has a reason
               { { "step", sMissing }, { "reason", "missing" } },            // 5: no such step
               { { "step", std::to_string( sU ) }, { "reason", "string" } }, // 6: id as a string
               { { "step", 1.5 }, { "reason", "fraction" } },                // 7: not a whole number
               { { "reason", "no step" } },                                  // 8: no step
               "not an object" } );                                          // 9
            KeeperExporter k( store.get() );
            const KeepOutcome o = k.Keep( g.jid, g.img, "sk-test-loopback", String(),
                                          String( wurl ) + "?reasons=" + String( JPctEncode( entries.dump() ).c_str() ) );
            StringList notes;
            const bool idle = JWaitKeeper( k, notes, 30 );
            String allNotes;
            for ( const String& n : notes ) allNotes += n + "\n";
            StepRow u, fr, c, r, m;
            store->GetStep( sU, u );
            store->GetStep( sF, fr );
            store->GetStep( sC, c );
            store->GetStep( sR, r );
            const bool missingExists = store->GetStep( sMissing, m );
            auto named = [&allNotes]( const std::string& s ) { return allNotes.Contains( String( s.c_str() ) ); };
            auto stepNote = [&named]( int64 id, const char* why ) { return named( "step " + std::to_string( id ) + " (" + why + ")" ); };
            const nlohmann::json after = nlohmann::json::parse( FileBytes( ExportDirOf( *store, g.jid ) + "/recipe.json" ) );
            bool recipeHasAccepted = false;
            for ( const nlohmann::json& s : after.at( "steps" ) )
               if ( s.at( "id" ) == sU && s.at( "reason" ) == "accepted reason" && s.at( "reasonInferred" ) == true ) recipeHasAccepted = true;
            const bool acceptOk = o.writeupStarted && idle && u.reason == "accepted reason" && u.reasonInferred && recipeHasAccepted;
            const bool foreignOk = fr.reason.empty() && !fr.reasonInferred && stepNote( sF, "it belongs to another journey" );
            const bool copilotOk = c.reason == "stretch gently" && !c.reasonInferred && stepNote( sC, "it was made by PI Copilot" );
            const bool reasonOk = r.reason == "lift the floor" && !r.reasonInferred && stepNote( sR, "it already has a stated reason" );
            const bool missingOk = !missingExists && stepNote( sMissing, "there is no such step" );
            const bool malformedOk = named( "entry 6 (" ) && named( "entry 7 (" ) && named( "entry 8 (" ) && named( "entry 9 (" )
                                  && !named( "entry 1 (" );
            d["guard"] = { { "notes", U8( allNotes ) }, { "accept", acceptOk }, { "foreign", foreignOk }, { "copilot", copilotOk },
                           { "reason", reasonOk }, { "missing", missingOk }, { "malformed", malformedOk } };
            guardOk = acceptOk && foreignOk && copilotOk && reasonOk && missingOk && malformedOk;
         }
         // (j) A reply cut off inside its json fence (fix round 1, I1): journey.md keeps the prose only,
         //     no reason is stored, and the note says the reasons were cut off at the token limit.
         {
            made.push_back( "pcWuT" );
            const JBuilt t = JRecordJourney( *store, "pcWuT", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'WuTrunc'" } } ), 51,
                                             { { "$T*1.2", "user", "" } } );
            KeeperExporter k( store.get() );
            const KeepOutcome o = k.Keep( t.jid, t.img, "sk-test-loopback", String(), String( wurl ) + "?truncate=1" );
            StringList notes;
            const bool idle = JWaitKeeper( k, notes, 30 );
            String allNotes;
            for ( const String& n : notes ) allNotes += n + "\n";
            const std::string md = FileBytes( ExportDirOf( *store, t.jid ) + "/journey.md" );
            StepRow s;
            store->GetStep( store->Steps( t.img, false ).at( 0 ).id, s );
            d["truncated"] = { { "md", md }, { "notes", U8( allNotes ) } };
            truncatedOk = o.writeupStarted && idle && md.find( "brightened the faint signal" ) != std::string::npos
                       && md.find( std::string( 3, '`' ) ) == std::string::npos && md.find( "inferredReasons" ) == std::string::npos
                       && s.reason.empty() && allNotes.Contains( "8000-token limit" ) && allNotes.Contains( "cut off" );
         }
         // (k) SetStore cancels and waits for a write-up in flight (fix round 1, I2): a stalled request, the
         //     same store keeps it, another store (nullptr) cancels it at once; nothing is written afterwards.
         //     An exporter with no store refuses Keep by name and polls to nothing.
         {
            const char* stall = std::getenv( "PICOPILOT_SELFTEST_STALL_URL" );
            if ( stall == nullptr )
               throw Error( "PICOPILOT_SELFTEST_STALL_URL not set by the harness" );
            const String md = ExportDirOf( *store, guardJid ) + "/journey.md";
            const std::string mdBefore = FileBytes( md );
            KeeperExporter k( store.get() );
            const KeepOutcome o = k.Keep( guardJid, 0, "sk-test-loopback", String(), String( stall ) );
            JPump( 300 );
            k.SetStore( store.get() );
            const bool keptBusy = k.Busy();
            const jclock::time_point t0 = jclock::now();
            k.SetStore( nullptr );
            const double cancelMs = MsSince( t0 );
            const bool busyAfter = k.Busy();
            StringList notes;
            JPump( 300 );
            k.Poll( notes );
            KeeperExporter none( nullptr );
            StringList noneNotes;
            none.Poll( noneNotes );
            const KeepOutcome no = none.Keep( guardJid, 0, "sk-test-loopback", String(), String( wurl ) );
            d["setStore"] = { { "started", o.writeupStarted }, { "keptBusy", keptBusy }, { "cancelMs", cancelMs },
                              { "busyAfter", busyAfter }, { "notes", notes.Length() }, { "noneError", U8( no.files.xpsmError ) } };
            setStoreOk = o.writeupStarted && keptBusy && !busyAfter && cancelMs < 10000 && notes.IsEmpty()
                      && FileBytes( md ) == mdBefore
                      && noneNotes.IsEmpty() && !none.Busy() && !no.marked && !no.writeupStarted && !no.files.xpsmOk
                      && no.files.xpsmError.Contains( "not open" );
         }
         // (g) LIVE (gated): the real Haiku write-up.
         if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
         {
            liveSkipped = false;
            liveOk = false;
            JTempDir exportRoot( "picopilot-wu-live-" );
            KeeperExporter k( store.get() );
            const KeepOutcome o = k.Retry( jid, String( key ), exportRoot.Path() );   // default URL: the real API
            // journey.md exists from (d): make the live run produce it again.
            File::Remove( ExportDirOf( *store, jid ) + "/journey.md" );
            const KeepOutcome o2 = k.Retry( jid, String( key ), exportRoot.Path() );
            StringList notes;
            const bool idle = JWaitKeeper( k, notes, 180 );
            const std::string md = FileBytes( ExportDirOf( *store, jid ) + "/journey.md" );
            String allNotes;
            for ( const String& n : notes ) allNotes += n + "\n";
            d["live"] = { { "started", o2.writeupStarted }, { "idle", idle }, { "chars", md.size() },
                          { "head", md.substr( 0, 200 ) }, { "notes", U8( allNotes ) }, { "firstRedone", o.redone.Length() } };
            liveOk = !o.writeupStarted && o2.writeupStarted && idle && md.rfind( "# ", 0 ) == 0 && md.size() > 200
                  && md.find( "WuM42" ) != std::string::npos && md.find( "## Processing" ) != std::string::npos
                  && md.find( "inferredReasons" ) == std::string::npos && !allNotes.Contains( String( key ) );
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      for ( const std::string& id : made )
         JForceClose( id );
      const bool ok = condensedOk && bodyOk && parseOk && loopbackOk && noKeyOk && retryOk && httpErrorOk
                   && guardOk && truncatedOk && setStoreOk;
      out["journeyWriteupDetail"] = d;
      out["journeyWriteupChecks"] = { { "condensed", condensedOk }, { "body", bodyOk }, { "parse", parseOk },
                                      { "loopback", loopbackOk }, { "noKey", noKeyOk }, { "retry", retryOk },
                                      { "httpError", httpErrorOk }, { "guard", guardOk }, { "truncated", truncatedOk },
                                      { "setStore", setStoreOk } };
      out["journeyWriteupError"] = U8( error );
      out["journeyWriteupOk"] = ok;
      out["liveWriteupSkipped"] = liveSkipped;
      out["liveWriteupDetail"] = d.value( "live", nlohmann::json() );
      out["liveWriteupOk"] = liveOk && error.IsEmpty();
      allOk = allOk && ok && liveOk && error.IsEmpty();
   }

   // ---- Section J9: JourneyService keeper wiring (integration of Tasks 7 + 9) ----
   // The PRODUCTION service's own recording of the pre-phase (PreM42, made by
   // the tracker from real top-level History, J6 (a)) is kept through the
   // service's Keeper(); only the service's OnTick() -- no direct Poll() --
   // finishes the write-up (loopback), stores the inferred reason, rewrites
   // recipe.json and makes the export copy, and its notes reach TakeNotes().
   SelfTestSectionMark( "J9 JourneyService keeper wiring" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool e2eOk = false, durableOk = false, nestedOk = false;
      String error;
      JourneyService& svc = JourneyService::Instance();
      try
      {
         const char* wurl = std::getenv( "PICOPILOT_SELFTEST_WRITEUP_URL" );
         if ( wurl == nullptr )
            throw Error( "PICOPILOT_SELFTEST_WRITEUP_URL not set by the harness" );
         JourneyStore* s = svc.Store();
         if ( !svc.Started() || s == nullptr )
            throw Error( "the production journey service has no store: " + svc.StoreError() );
         int64 jid = 0, img = 0;
         for ( const JourneyRow& j : s->ListJourneys( false, "PreM42", 5 ) )
         {
            jid = j.id;
            for ( const ImageRow& i : s->Images( jid ) )
               if ( i.isMaster )
                  img = i.id;
         }
         if ( jid == 0 || img == 0 )
            throw Error( "the production service recorded no PreM42 journey (see J6 service)" );
         JourneyRow before;
         s->GetJourney( jid, before );

         // (a) + (b): keep through the service; the keep commits at synchronous=FULL.
         JTempDir exportRoot( "picopilot-j9-out-" );
         std::vector<int> syncAtCommit;
         s->SetKeepCommitHookForSelfTest( [&syncAtCommit]( int level ) { syncAtCommit.push_back( level ); } );
         (void)svc.TakeNotes();   // start from an empty note list
         KeepOutcome o;
         try
         {
            o = svc.Keeper().Keep( jid, img, "sk-test-loopback", exportRoot.Path(), String( wurl ) );
         }
         catch ( ... )
         {
            s->SetKeepCommitHookForSelfTest( nullptr );
            throw;
         }
         s->SetKeepCommitHookForSelfTest( nullptr );
         const int syncAfter = s->SynchronousLevelForSelfTest();
         JourneyRow kept;
         s->GetJourney( jid, kept );

         // Only the service's tick may finish the job. The timer is not pumped (no events are
         // processed here); OnTick() is called directly, as the timer would.
         String allNotes;
         const jclock::time_point t0 = jclock::now();
         svc.SetEnabledForSelfTest( true );
         try
         {
            while ( MsSince( t0 ) < 30000 )
            {
               svc.OnTick();
               for ( const String& n : svc.TakeNotes() )
                  allNotes += n + "\n";
               if ( !svc.Keeper().Busy() && allNotes.Contains( "journey.md" ) )
                  break;
               std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            }
         }
         catch ( ... )
         {
            svc.SetEnabledForSelfTest( false );
            throw;
         }
         svc.SetEnabledForSelfTest( false );
         const double waitMs = MsSince( t0 );
         const String dir = ExportDirOf( *s, jid );
         const std::string md = File::Exists( dir + "/journey.md" ) ? FileBytes( dir + "/journey.md" ) : std::string();
         int inferred = 0;
         bool recipeValid = false;
         if ( File::Exists( dir + "/recipe.json" ) )
         {
            const nlohmann::json after = nlohmann::json::parse( FileBytes( dir + "/recipe.json" ) );
            for ( const nlohmann::json& st : after.at( "steps" ) )
               if ( st.at( "reasonInferred" ) == true && st.at( "reason" ) == "brighten the faint signal" ) ++inferred;
            std::string why;
            recipeValid = ValidateRecipe( after, why );
         }
         int storedInferred = 0;
         for ( const StepRow& st : s->Steps( img, true ) )
            if ( st.reasonInferred && st.reason == "brighten the faint signal" ) ++storedInferred;
         String copied;
         {
            FindFileInfo info;
            for ( File::Find f( exportRoot.Path() + "/PreM42/*" ); f.NextItem( info ); )
               if ( info.IsDirectory() && info.name != "." && info.name != ".." )
                  copied = exportRoot.Path() + "/PreM42/" + info.name;
         }
         d["e2e"] = { { "journey", jid }, { "wasKept", before.kept }, { "marked", o.marked }, { "kept", kept.kept },
                      { "endImage", kept.endImageId }, { "files", { o.files.xpsmOk, o.files.recipeOk, o.files.thumbsOk } },
                      { "started", U8( o.writeupStarted ? String( "yes" ) : o.writeupError ) },
                      { "busy", svc.Keeper().Busy() }, { "waitMs", waitMs }, { "mdHead", md.substr( 0, 80 ) },
                      { "inferredInRecipe", inferred }, { "inferredStored", storedInferred }, { "recipeValid", recipeValid },
                      { "copied", U8( copied ) }, { "notes", U8( allNotes ) } };
         e2eOk = !before.kept && o.marked && kept.kept && kept.endImageId == img
              && o.files.xpsmOk && o.files.recipeOk && o.writeupStarted && !svc.Keeper().Busy()
              && md.rfind( "# ", 0 ) == 0 && md.find( "inferredReasons" ) == std::string::npos
              && inferred == 1 && storedInferred == 1 && recipeValid
              && !copied.IsEmpty() && File::Exists( copied + "/journey.md" )
              && FileBytes( copied + "/recipe.json" ) == FileBytes( dir + "/recipe.json" )
              && allNotes.Contains( "journey.md written to" ) && allNotes.Contains( "keeper copied to" )
              && !allNotes.Contains( "sk-test-loopback" );
         d["durable"] = { { "syncAtCommit", syncAtCommit }, { "syncAfter", syncAfter } };
         durableOk = syncAtCommit.size() == 1 && syncAtCommit[0] == 2 && syncAfter == 1;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      // (c) A durable keep inside an open transaction is refused loudly, writes nothing and
      //     leaves the connection at NORMAL (own temp library).
      try
      {
         JTempDir root( "picopilot-j9-" );
         String oe;
         std::unique_ptr<JourneyStore> t = JourneyStore::Open( root.Path(), oe );
         if ( !t )
            throw Error( "store: " + oe );
         const std::string now = NowIso();
         const int64 j = t->CreateJourney( "J9 nested", "J9", now );
         String refused;
         {
            JourneyStore::Transaction tx( *t );
            try { t->MarkKeptDurably( j, 0, now ); }
            catch ( const pcl::Exception& x ) { refused = x.Message(); }
         }   // rolled back
         JourneyRow r;
         t->GetJourney( j, r );
         const int sync = t->SynchronousLevelForSelfTest();
         t->MarkKeptDurably( j, 0, now );
         JourneyRow r2;
         t->GetJourney( j, r2 );
         d["nested"] = { { "refused", U8( refused ) }, { "keptAfterRefusal", r.kept }, { "sync", sync },
                         { "keptAfter", r2.kept }, { "syncAfter", t->SynchronousLevelForSelfTest() } };
         nestedOk = refused.Contains( "transaction" ) && !r.kept && sync == 1 && r2.kept
                 && t->SynchronousLevelForSelfTest() == 1;
      }
      catch ( const pcl::Exception& x ) { if ( error.IsEmpty() ) error = "nested: " + x.Message(); }
      catch ( const std::exception& x ) { if ( error.IsEmpty() ) error = "nested: " + String( x.what() ); }

      const bool ok = e2eOk && durableOk && nestedOk && error.IsEmpty();
      out["journeyWiringDetail"] = d;
      out["journeyWiringChecks"] = { { "e2e", e2eOk }, { "durable", durableOk }, { "nested", nestedOk } };
      out["journeyWiringError"] = U8( error );
      out["journeyWiringOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J10: journey tools (Task 10) --------------------------------
   // J10's own library + tracker. In-process steps never reach History (harness
   // fact), so the tracker reads its fixture windows' History from JFakeHistory
   // (the steps a top-level run would have left); every other window is real.
   SelfTestSectionMark( "J10 journey tools" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool schemaOk = false, promptOk = false, noHostOk = false, listOk = false, getOk = false, markOk = false,
           compareOk = false, freezeBusyOk = false, keepErrorOk = false, startOk = false, noEffectOk = false,
           replayMatchOk = false, attributionOk = false, cancelOk = false;
      // Fix round 1.
      bool offFreezeOk = false, inFlightOk = false, pagingOk = false, idTypeOk = false, markViewOk = false,
           orderOk = false, noteActionOk = false, scrubOk = false, replayLookupOk = false;
      String error;
      std::vector<std::string> made;
      try
      {
         auto names = []( const nlohmann::json& tools )
         {
            std::vector<std::string> n;
            for ( const nlohmann::json& t : tools )
               n.push_back( t.at( "name" ).get<std::string>() );
            return n;
         };
         auto text0 = []( const ToolOutcome& o ) { return o.content.at( 0 ).at( "text" ).get<std::string>(); };
         // GC privacy (P6): no "/"-rooted directory in text meant for the model.
         auto noDirs = []( const std::string& s )
         {
            for ( size_t i = 0; i < s.size(); ++i )
               if ( s[i] == '/' && (i == 0 || s[i-1] == ' ' || s[i-1] == '(' || s[i-1] == '\'' || s[i-1] == '"') )
                  return false;
            return true;
         };

         // (a) Schemas and mode gating (Ruling 14).
         {
            ToolOptions scripts;
            scripts.runPjsr = true;
            const std::vector<std::string> adv = names( ToolDefinitions( AgentMode::Advisor ) );
            const std::vector<std::string> cop = names( ToolDefinitions( AgentMode::Copilot, scripts ) );
            bool shapes = true;
            for ( const nlohmann::json& t : JourneyToolDefinitions( AgentMode::Copilot ) )
               shapes = shapes && t.at( "input_schema" ).at( "type" ) == "object" && !t.at( "description" ).get<std::string>().empty();
            schemaOk = shapes && cop.back() == "run_pjsr"
                    && std::find( adv.begin(), adv.end(), "mark_journey_best" ) != adv.end()
                    && std::find( adv.begin(), adv.end(), "replay_journey" ) == adv.end()
                    && std::find( adv.begin(), adv.end(), "start_journey" ) == adv.end()
                    && IsJourneyTool( "replay_journey" ) && !IsJourneyTool( "apply_process" )
                    && ToolDefinitions( AgentMode::Copilot ).at( 3 ).at( "input_schema" ).at( "properties" ).contains( "reason" )
                    && ToolDefinitions( AgentMode::Copilot ).at( 4 ).at( "input_schema" ).at( "properties" ).contains( "reason" );
            d["advisorTools"] = adv;
         }
         // (b) Prompt lines per mode.
         {
            const String pc = BuildSystemPrompt( AgentMode::Copilot ), pg = BuildSystemPrompt( AgentMode::Guided ),
                         pa = BuildSystemPrompt( AgentMode::Advisor );
            promptOk = pc.Contains( "replay_journey {" ) && pg.Contains( "replay_journey {" ) && pc.Contains( "Never invent a substitute" )
                    && pa.Contains( "list_journeys {" ) && pa.Contains( "mark_journey_best {" ) && !pa.Contains( "replay_journey" )
                    && !pa.Contains( "start_journey" ) && !pa.Contains( "apply_process {" ) && !pa.Contains( "run_global_process" )
                    && !pc.Contains( "run_pjsr" )
                    // P2: the retry route; P26: Advisor presents a replay plan it cannot run.
                    && pa.Contains( "To redo a kept journey's outputs, pass its journey_id" )
                    && pa.Contains( "present the plan from get_journey + compare_to_journey; you cannot run it" );
         }
         // (c) No library -> a precise error, never a crash.
         {
            ToolContext c;
            c.mode = AgentMode::Copilot;
            const ToolOutcome o = ExecuteTool( ToolCall{ "j0", "list_journeys", nlohmann::json::object() }, c );
            // Fix round 1 (m1): the honest state -- the tools are not connected in this session; recording is not
            // claimed to be off (the service records on its own).
            noHostOk = o.isError && text0( o ).find( "not connected" ) != std::string::npos
                    && text0( o ).find( "recording is not running" ) == std::string::npos;
            d["noHost"] = text0( o );
         }

         JTempDir root( "picopilot-jt-" );
         String oe;
         std::unique_ptr<JourneyStore> store = JourneyStore::Open( root.Path(), oe );
         if ( !store )
            throw Error( "store: " + oe );
         JFakeHistory fake;
         JourneyTracker trk( store.get() );
         trk.SetHistoryReaderForSelfTest( [&fake]( const IsoString& id, int from ) { return fake.Read( id, from ); } );
         KeeperExporter keeper( store.get() );
         int confirms = 0;
         bool answer = false;
         JourneyToolHost host;
         host.store = store.get();
         host.tracker = &trk;
         host.keeper = &keeper;
         host.apiKey = []() { return String(); };
         host.confirmKeeper = [&]( const String& html ) { ++confirms; d["summaryHtml"] = U8( html ); return answer; };
         auto ctxFor = [&]( AgentMode m, const char* view )
         {
            ToolContext c;
            c.mode = m;
            c.turnViewId = IsoString( view );
            c.journeys = &host;
            return c;
         };

         // Two recorded journeys of one target, each resumed by the tracker (Task 7's save + reopen path). Both
         // masters share one fingerprint, so B is made only after A's window is tracked: a tracked image is
         // never resumed twice (Task 7 review I2), so B then resumes its own row.
         const JBuilt A = JBuildJourney( *store, "pcJtA", "JtM42", 51 );
         made.push_back( "pcJtA" );
         fake.steps["pcJtA"] = JHistoryOfImage( *store, A.img );
         JTick( trk, 2 );
         const JBuilt B = JBuildJourney( *store, "pcJtB", "JtM42", 52 );
         made.push_back( "pcJtB" );
         fake.steps["pcJtB"] = JHistoryOfImage( *store, B.img );
         JTick( trk, 2 );
         const int64 jA = A.jid, jB = B.jid;
         d["resumed"] = { { "A", trk.JourneyOfView( "pcJtA" ) }, { "B", trk.JourneyOfView( "pcJtB" ) }, { "jA", jA }, { "jB", jB } };
         if ( trk.JourneyOfView( "pcJtA" ) != jA || trk.JourneyOfView( "pcJtB" ) != jB || trk.ImageOfView( "pcJtA" ) != A.img )
            throw Error( "fixture: the tracker did not resume pcJtA / pcJtB into their own journeys" );

         // (d) list_journeys: all, kept only, by target.
         {
            store->MarkKept( jB, B.img, NowIso() );
            const nlohmann::json all = nlohmann::json::parse( text0( ExecuteTool( ToolCall{ "l1", "list_journeys", nlohmann::json::object() }, ctxFor( AgentMode::Advisor, "pcJtA" ) ) ) );
            const nlohmann::json tgt = nlohmann::json::parse( text0( ExecuteTool( ToolCall{ "l1b", "list_journeys", { { "target", "jtm42" } } }, ctxFor( AgentMode::Advisor, "pcJtA" ) ) ) );
            const nlohmann::json kept = nlohmann::json::parse( text0( ExecuteTool( ToolCall{ "l2", "list_journeys", { { "kept_only", true }, { "target", "JtM42" } } }, ctxFor( AgentMode::Advisor, "pcJtA" ) ) ) );
            const nlohmann::json none = nlohmann::json::parse( text0( ExecuteTool( ToolCall{ "l3", "list_journeys", { { "target", "Nope" } } }, ctxFor( AgentMode::Advisor, "pcJtA" ) ) ) );
            bool both = false, shape = tgt.at( "journeys" ).size() == 2;
            int seen = 0;
            for ( const nlohmann::json& j : all.at( "journeys" ) )
               seen += (j.at( "id" ) == jA || j.at( "id" ) == jB) ? 1 : 0;
            both = seen == 2;
            for ( const nlohmann::json& j : tgt.at( "journeys" ) )
               shape = shape && j.at( "steps" ) == 3 && j.at( "masters" ).at( 0 ).at( "filter" ) == "Ha";
            d["list"] = { { "tgt", tgt }, { "kept", kept } };
            listOk = both && shape && kept.at( "journeys" ).size() == 1 && kept.at( "journeys" ).at( 0 ).at( "id" ) == jB
                  && none.at( "journeys" ).empty();
         }
         // (e) get_journey: default = the turn view's journey; parameters only on request; no directories.
         {
            const ToolOutcome g1 = ExecuteTool( ToolCall{ "g1", "get_journey", nlohmann::json::object() }, ctxFor( AgentMode::Advisor, "pcJtA" ) );
            const ToolOutcome g2 = ExecuteTool( ToolCall{ "g2", "get_journey", { { "journey_id", jA }, { "include_parameters", true } } }, ctxFor( AgentMode::Advisor, "" ) );
            const ToolOutcome g3 = ExecuteTool( ToolCall{ "g3", "get_journey", nlohmann::json::object() }, ctxFor( AgentMode::Advisor, "" ) );
            const nlohmann::json j1 = nlohmann::json::parse( text0( g1 ) ), j2 = nlohmann::json::parse( text0( g2 ) );
            d["get"] = { { "g3", text0( g3 ) } };
            getOk = !g1.isError && j1.at( "journey" ).at( "id" ) == jA && !j1.at( "steps" ).at( 0 ).contains( "parameters" )
                 && j2.at( "steps" ).at( 0 ).contains( "parameters" ) && text0( g2 ).find( U8( root.Path() ) ) == std::string::npos
                 && g3.isError && text0( g3 ).find( "journey_id" ) != std::string::npos;
         }
         // (g) compare_to_journey: from the database only; per-channel start ratios (current / keeper) are numbers.
         //     Both masters come from JMakeNoiseMaster (0.1 +- 0.01), so only the shape is asserted, not a direction.
         {
            const ToolOutcome o = ExecuteTool( ToolCall{ "c1", "compare_to_journey", { { "journey_id", jB } } }, ctxFor( AgentMode::Advisor, "pcJtA" ) );
            const nlohmann::json c = nlohmann::json::parse( text0( o ) );
            d["compare"] = c;
            compareOk = !o.isError && o.content.size() == 1 && c.at( "keeper" ).at( "id" ) == jB && c.at( "current" ).at( "id" ) == jA
                     && c.at( "startRatios" ).at( "noise" ).at( 0 ).is_number() && c.contains( "divergesAtStep" )
                     && c.at( "keeper" ).at( "acquisition" ).at( "filter" ) == "Ha";
         }
         // (f) mark_journey_best: summary + confirm (declined -> nothing), then kept; again -> no dialog, retry only.
         //     After the keep, pcJtA belongs to "(continued)" (Ruling 26), so the retry names the kept journey by
         //     journey_id (the prompt and the tool description say so; spec §6.4 retry from chat).
         {
            answer = false;
            const ToolOutcome no = ExecuteTool( ToolCall{ "m1", "mark_journey_best", nlohmann::json::object() }, ctxFor( AgentMode::Advisor, "pcJtA" ) );
            JourneyRow r1;
            store->GetJourney( jA, r1 );
            answer = true;
            const ToolOutcome yes = ExecuteTool( ToolCall{ "m2", "mark_journey_best", nlohmann::json::object() }, ctxFor( AgentMode::Advisor, "pcJtA" ) );
            JourneyRow r2;
            store->GetJourney( jA, r2 );
            const int confirmsAfterYes = confirms;
            const ToolOutcome again = ExecuteTool( ToolCall{ "m3", "mark_journey_best", { { "journey_id", jA } } }, ctxFor( AgentMode::Advisor, "pcJtA" ) );
            d["mark"] = { { "no", text0( no ) }, { "yes", text0( yes ) }, { "again", text0( again ) }, { "confirms", confirms },
                          { "yesLog", U8( yes.logLine ) }, { "continued", trk.JourneyOfView( "pcJtA" ) },
                          { "status", trk.StatusFor( "pcJtA" ).name } };
            markOk = no.isError && text0( no ).find( "declined" ) != std::string::npos && !r1.kept
                  && !yes.isError && r2.kept && r2.endImageId == A.img
                  && text0( yes ).find( "recipe.json" ) != std::string::npos && text0( yes ).find( "API key" ) != std::string::npos
                  && confirmsAfterYes == 2 && confirms == 2 && !again.isError
                  && text0( again ).find( "already kept" ) != std::string::npos
                  && noDirs( text0( no ) ) && noDirs( text0( yes ) ) && noDirs( text0( again ) )
                  && text0( yes ).find( U8( root.Path() ) ) == std::string::npos
                  && yes.logLine.Contains( root.Path() )   // the chat log names the folder
                  && d["summaryHtml"].get<std::string>().find( "JtM42" ) != std::string::npos
                  // Ruling 26: the kept journey is frozen; the image continues in a new journey.
                  && trk.JourneyOfView( "pcJtA" ) != 0 && trk.JourneyOfView( "pcJtA" ) != jA
                  && trk.StatusFor( "pcJtA" ).name.find( "(continued)" ) != std::string::npos
                  && store->StepCount( jA, true ) == 3 && store->StepCount( trk.JourneyOfView( "pcJtA" ), true ) == 0;
         }
         // (f2) Freezing a journey whose image is busy never waits (P9): the image joins "(continued)" once free.
         //      Own target, so the replay matching in (i) still sees exactly jA and jB.
         {
            const JBuilt F = JBuildJourney( *store, "pcJtF", "JtFreeze", 56 );
            made.push_back( "pcJtF" );
            fake.steps["pcJtF"] = JHistoryOfImage( *store, F.img );
            JTick( trk, 2 );
            if ( trk.JourneyOfView( "pcJtF" ) != F.jid )
               throw Error( "fixture: the tracker did not resume pcJtF" );
            // Review m4: a step made right before the keep that no tick has recorded yet (in flight) must land
            // somewhere -- as the continuation's first step, never absorbed into its base.
            std::vector<HistoryStep>& hf = fake.steps["pcJtF"];
            hf.push_back( JPixelMathStep( "$T*0.98", int( hf.size() ), NowIso() ) );
            KeepFlowResult kf;
            double ms = 0;
            int64 during = -1;
            {
               View fv = ImageWindow::WindowById( "pcJtF" ).MainView();
               AutoViewLock lock( fv );
               answer = true;
               const jclock::time_point t0 = jclock::now();
               kf = RunKeepFlow( host, F.jid, "pcJtF" );
               ms = MsSince( t0 );
               during = trk.JourneyOfView( "pcJtF" );
            }
            JTick( trk );
            const int64 after = trk.JourneyOfView( "pcJtF" );
            JourneyRow fr;
            store->GetJourney( after, fr );
            d["freezeBusy"] = { { "ms", ms }, { "during", during }, { "after", after }, { "name", fr.name },
                                { "message", U8( kf.message ) }, { "modelMessage", U8( kf.modelMessage ) } };
            freezeBusyOk = kf.ok && during == 0 && ms < 2000 && after != 0 && after != F.jid
                        && fr.name.find( "(continued)" ) != std::string::npos && store->StepCount( F.jid, true ) == 3
                        && noDirs( U8( kf.modelMessage ) );
            d["inFlight"] = { { "continuedSteps", store->StepCount( after, true ) }, { "keptSteps", store->StepCount( F.jid, true ) } };
            inFlightOk = after != 0 && store->StepCount( after, true ) == 1 && store->StepCount( F.jid, true ) == 3;
         }
         // (f3) Integration concern #4: the keep's own failures (a keep inside an open transaction; the library
         //      locked by another program) are visible errors, never a crash or a silent "kept".
         {
            const JBuilt E = JBuildJourney( *store, "pcJtE", "JtErr", 57 );
            made.push_back( "pcJtE" );
            answer = true;
            const int before = confirms;
            KeepFlowResult inTx;
            {
               JourneyStore::Transaction tx( *store );
               inTx = RunKeepFlow( host, E.jid, "" );
            }
            KeepFlowResult locked;
            {
               RawDb other( store->DbPath() );
               const bool held = other.Exec( "BEGIN IMMEDIATE" );
               locked = RunKeepFlow( host, E.jid, "" );
               other.Exec( "ROLLBACK" );
               d["keepErrorHeld"] = held;
            }
            JourneyRow er;
            store->GetJourney( E.jid, er );
            d["keepError"] = { { "inTx", U8( inTx.message ) }, { "locked", U8( locked.message ) },
                               { "lockedModel", U8( locked.modelMessage ) }, { "confirms", confirms - before } };
            keepErrorOk = !inTx.ok && !inTx.declined && inTx.message.Contains( "transaction" )
                       && !locked.ok && !locked.declined && locked.message.Contains( "locked" ) && !er.kept
                       && noDirs( U8( locked.modelMessage ) );
         }
         // (f4) Review I1 (spec §8): with recording OFF a keep still freezes the journey (its images leave it),
         //      but nothing is read or stored for a "(continued)" journey.
         {
            const JBuilt G = JBuildJourney( *store, "pcJtG", "JtOff", 59 );
            made.push_back( "pcJtG" );
            fake.steps["pcJtG"] = JHistoryOfImage( *store, G.img );
            JTick( trk, 2 );
            if ( trk.JourneyOfView( "pcJtG" ) != G.jid )
               throw Error( "fixture: the tracker did not resume pcJtG" );
            const int journeysBefore = JourneyCount( *store ), readsBefore = fake.reads;
            trk.SetEnabled( false );
            answer = true;
            const KeepFlowResult k = RunKeepFlow( host, G.jid, "pcJtG" );
            const int journeysAfter = JourneyCount( *store ), readsAfter = fake.reads;
            const int64 ofView = trk.JourneyOfView( "pcJtG" );
            JourneyRow gr;
            store->GetJourney( G.jid, gr );
            JForceClose( "pcJtG" );   // closed before recording is on again (else it would start a journey then)
            trk.SetEnabled( true );
            JTick( trk );
            d["recordingOff"] = { { "journeys", { journeysBefore, journeysAfter } }, { "reads", { readsBefore, readsAfter } },
                                  { "ofView", ofView }, { "message", U8( k.modelMessage ) } };
            offFreezeOk = k.ok && gr.kept && journeysAfter == journeysBefore && readsAfter == readsBefore && ofView == 0
                       && k.modelMessage.Contains( "recording is off" );
         }
         // (h) start_journey: an unrecognized master; then "already recorded"; never in Advisor.
         {
            JMakeNoiseMaster( "pcJtPlain", 53 );
            made.push_back( "pcJtPlain" );
            JTick( trk, 2 );
            const bool untrackedBefore = trk.JourneyOfView( "pcJtPlain" ) == 0;
            const ToolOutcome s1 = ExecuteTool( ToolCall{ "s1", "start_journey", nlohmann::json::object() }, ctxFor( AgentMode::Copilot, "pcJtPlain" ) );
            const ToolOutcome s2 = ExecuteTool( ToolCall{ "s2", "start_journey", nlohmann::json::object() }, ctxFor( AgentMode::Copilot, "pcJtPlain" ) );
            const ToolOutcome s3 = ExecuteTool( ToolCall{ "s3", "start_journey", nlohmann::json::object() }, ctxFor( AgentMode::Advisor, "pcJtPlain" ) );
            d["start"] = { { "s1", text0( s1 ) }, { "s2", text0( s2 ) }, { "s3", text0( s3 ) } };
            startOk = untrackedBefore && !s1.isError && trk.JourneyOfView( "pcJtPlain" ) != 0
                   && s2.isError && text0( s2 ).find( "already recorded" ) != std::string::npos
                   && s3.isError && text0( s3 ).find( "not available in Advisor" ) != std::string::npos;
         }
         // (h2) T-graxpert concern: a Copilot run that reported success but changed nothing (noEffect) is not
         //      recorded as a step of the journey: its History entry is kept for alignment but flagged and
         //      excluded (counts, recipe), like a base step.
         {
            fake.steps["pcJtPlain"] = {};   // its real History: none
            const int64 jp = trk.JourneyOfView( "pcJtPlain" );
            const uint64 token = trk.NoteCopilotStep( "pcJtPlain", "PixelMath", "remove the gradient", {}, false, JourneyWallNow() );
            trk.SetCopilotNoteNoEffect( token );
            fake.steps["pcJtPlain"].push_back( JPixelMathStep( "$T", 0, NowIso() ) );
            JTick( trk );
            bool flagged = false;
            for ( const StepRow& r : store->Steps( trk.ImageOfView( "pcJtPlain" ), false ) )
               flagged = flagged || (r.actor == "copilot" && r.params.value( "noEffect", false ));
            const nlohmann::json recipe = BuildRecipe( *store, jp, "PI Copilot" );
            d["noEffect"] = { { "token", token }, { "flagged", flagged }, { "count", store->StepCount( jp, true ) },
                              { "recipeSteps", recipe.at( "steps" ).size() } };
            noEffectOk = token != 0 && flagged && store->StepCount( jp, true ) == 0 && recipe.at( "steps" ).empty();
         }
         // (i) replay_journey: none / several / chosen.
         {
            JMakeNoiseMaster( "pcJtNew", 54 );
            made.push_back( "pcJtNew" );
            JSetKeywords( "pcJtNew", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'JtM42'" }, { "FILTER", "'Ha'" },
                                           { "INSTRUME", "'ASI2400MC'" } } ) );
            JMakeNoiseMaster( "pcJtOther", 55 );
            made.push_back( "pcJtOther" );
            JSetKeywords( "pcJtOther", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'Nothing'" } } ) );
            JTick( trk, 2 );
            const ToolOutcome none = ExecuteTool( ToolCall{ "r0", "replay_journey", nlohmann::json::object() }, ctxFor( AgentMode::Copilot, "pcJtOther" ) );
            const ToolOutcome two = ExecuteTool( ToolCall{ "r1", "replay_journey", nlohmann::json::object() }, ctxFor( AgentMode::Copilot, "pcJtNew" ) );
            const ToolOutcome one = ExecuteTool( ToolCall{ "r2", "replay_journey", { { "journey_id", jA } } }, ctxFor( AgentMode::Guided, "pcJtNew" ) );
            const ToolOutcome adv = ExecuteTool( ToolCall{ "r3", "replay_journey", { { "journey_id", jA } } }, ctxFor( AgentMode::Advisor, "pcJtNew" ) );
            const nlohmann::json t = nlohmann::json::parse( text0( two ) ), m = nlohmann::json::parse( text0( one ) );
            // Review m6: the lookup renames nothing; its own journey is refused; the first applied step names it.
            JourneyRow cur, keptA, looked;
            store->GetJourney( trk.JourneyOfView( "pcJtNew" ), looked );
            const ToolOutcome self = ExecuteTool( ToolCall{ "r4", "replay_journey", { { "journey_id", trk.JourneyOfView( "pcJtNew" ) } } },
                                                  ctxFor( AgentMode::Copilot, "pcJtNew" ) );
            const ToolOutcome step = ExecuteTool( ToolCall{ "r5", "apply_process", { { "process_id", "PixelMath" },
                                                  { "parameters", { { "expression", "$T" } } }, { "reason", "replay step 1" } } },
                                                  ctxFor( AgentMode::Copilot, "pcJtNew" ) );
            store->GetJourney( trk.JourneyOfView( "pcJtNew" ), cur );
            store->GetJourney( jA, keptA );
            d["replayLookup"] = { { "afterLookup", looked.name }, { "self", text0( self ) }, { "afterStep", cur.name } };
            replayLookupOk = looked.name.find( "(replay of #" ) == std::string::npos && self.isError
                          && text0( self ).find( "own journey" ) != std::string::npos && !step.isError;
            d["replay"] = { { "none", text0( none ) }, { "two", t }, { "oneKeys", nlohmann::json::array() }, { "curName", cur.name } };
            for ( auto it = m.begin(); it != m.end(); ++it ) d["replay"]["oneKeys"].push_back( it.key() );
            replayMatchOk = none.isError && text0( none ).find( "no kept journey" ) != std::string::npos
                         && !two.isError && t.at( "needsChoice" ) == true && t.at( "candidates" ).size() == 2
                         && !one.isError && m.at( "keeper" ).at( "id" ) == jA && m.at( "steps" ).size() == 3
                         && m.at( "steps" ).at( 0 ).contains( "recordedMedianAfter" ) && m.at( "steps" ).at( 0 ).at( "manual" ) == false
                         && m.at( "differences" ).contains( "noiseRatio" )
                         && cur.name == keptA.name + " (replay of #" + std::to_string( jA ) + ")"   // spec §13.7 (P19), set by the step
                         && adv.isError && noDirs( text0( one ) );
         }
         // (j) Copilot attribution through the REAL tool path: the note is posted before the run and the History
         //     step it made is the Copilot's, with the reason; a created window is linked by 'copilot'; a failed
         //     run and a run that adds no History step to its target (createNewImage) leave no note behind, so the
         //     user's next PixelMath on the same image stays the user's.
         {
            // Review M5 regression guard: the step note exists while the process runs (posted BEFORE ExecuteOn).
            std::vector<std::string> notesAtRun;
            bool hookRan = false;
            SetBeforeExecuteHookForSelfTest( [&]() { hookRan = true; notesAtRun = trk.CopilotNotesForSelfTest(); } );
            const ToolOutcome a = ExecuteTool( ToolCall{ "a1", "apply_process", { { "process_id", "PixelMath" },
                                               { "parameters", { { "expression", "$T*1.05" } } }, { "reason", "a touch brighter" } } },
                                               ctxFor( AgentMode::Copilot, "pcJtA" ) );
            SetBeforeExecuteHookForSelfTest( std::function<void()>() );
            d["order"] = { { "hookRan", hookRan }, { "notesAtRun", notesAtRun } };
            orderOk = hookRan && std::find( notesAtRun.begin(), notesAtRun.end(),
                                            std::string( "pcJtA|PixelMath|a touch brighter|0" ) ) != notesAtRun.end();
            std::vector<HistoryStep>& ha = fake.steps["pcJtA"];
            ha.push_back( JPixelMathStep( "$T*1.05", int( ha.size() ), NowIso() ) );   // what the run left in History
            const ToolOutcome n = ExecuteTool( ToolCall{ "a2", "apply_process", { { "process_id", "PixelMath" },
                                               { "parameters", { { "expression", "$T" }, { "createNewImage", true }, { "newImageId", "pcJtCopy" } } },
                                               { "reason", "a working copy" } } }, ctxFor( AgentMode::Copilot, "pcJtA" ) );
            made.push_back( "pcJtCopy" );
            JTick( trk, 3 );
            const int64 aImg = trk.ImageOfView( "pcJtA" );
            bool reasonSeen = false;
            for ( const StepRow& r : store->Steps( aImg, false ) )
               reasonSeen = reasonSeen || (r.actor == "copilot" && r.reason == "a touch brighter");
            std::string ev;
            for ( const LinkRow& l : store->Links( trk.JourneyOfView( "pcJtA" ) ) )
               if ( l.toImageId == trk.ImageOfView( "pcJtCopy" ) ) ev = l.evidence;
            d["attribution"] = { { "a", text0( a ) }, { "n", text0( n ) }, { "evidence", ev } };
            attributionOk = !a.isError && !n.isError && reasonSeen && ev == "copilot";

            const ToolOutcome bad = ExecuteTool( ToolCall{ "a3", "apply_process", { { "process_id", "PixelMath" },
                                                 { "parameters", { { "expression", "$T*(" } } }, { "reason", "broken" } } },
                                                 ctxFor( AgentMode::Copilot, "pcJtA" ) );
            ha.push_back( JPixelMathStep( "$T*0.99", int( ha.size() ), NowIso() ) );   // the user's own step
            JTick( trk, 2 );
            std::string actor = "?";
            for ( const StepRow& r : store->Steps( aImg, false ) )
               if ( r.params.value( "parameters", nlohmann::json::object() ).value( "expression", std::string() ) == "$T*0.99" )
                  actor = r.actor;
            d["cancel"] = { { "bad", text0( bad ) }, { "actor", actor } };
            cancelOk = bad.isError && actor == "user";
         }
         // (k) Review I2: replay material pages under the tool-result cap; a manual step carries no parameters.
         {
            const int64 big = JAddBigKeeper( *store, 12 );
            nlohmann::json pages = nlohmann::json::array();
            std::vector<int> ns;
            bool valid = true, manualDropped = false, curvesWhole = true;
            int from = 1;
            for ( int guard = 0; guard < 20 && from > 0; ++guard )
            {
               const ToolOutcome o = ExecuteTool( ToolCall{ "p1", "replay_journey", { { "journey_id", big }, { "from_step", from } } },
                                                  ctxFor( AgentMode::Copilot, "pcJtNew" ) );
               const std::string txt = text0( o );
               nlohmann::json j;
               const bool parsed = !o.isError && txt.find( "[tool result cut" ) == std::string::npos
                                && txt.size() < PICopilotMaxToolResultChars && nlohmann::json::accept( txt );
               valid = valid && parsed;
               if ( !parsed )
               {
                  pages.push_back( { { "from", from }, { "error", txt.substr( 0, 300 ) }, { "chars", txt.size() } } );
                  break;
               }
               j = nlohmann::json::parse( txt );
               for ( const nlohmann::json& st : j.at( "steps" ) )
               {
                  ns.push_back( st.at( "n" ).get<int>() );
                  if ( st.at( "processId" ) == "DynamicBackgroundExtraction" )
                     manualDropped = st.at( "manual" ) == true && !st.contains( "parameters" ) && !st.contains( "table_parameters" )
                                  && st.contains( "parametersOmitted" );
                  else
                     curvesWhole = curvesWhole && st.at( "table_parameters" ).at( "K" ).size() == 400;
               }
               pages.push_back( { { "from", from }, { "chars", txt.size() }, { "steps", j.at( "steps" ).size() },
                                  { "totalSteps", j.value( "totalSteps", -1 ) }, { "more", j.value( "moreSteps", false ) } } );
               from = j.value( "moreSteps", false ) ? j.value( "nextFromStep", 0 ) : 0;
            }
            bool inOrder = ns.size() == 13;
            for ( size_t i = 0; inOrder && i < ns.size(); ++i )
               inOrder = ns[i] == int( i + 1 );
            d["paging"] = { { "pages", pages }, { "n", ns } };
            pagingOk = valid && inOrder && manualDropped && curvesWhole && pages.size() > 1;
         }
         // (l) Review m5: a journey_id that is present but not an integer is an error, never "the active image's".
         {
            const int before = confirms;
            const ToolOutcome g = ExecuteTool( ToolCall{ "t1", "get_journey", { { "journey_id", "1" } } }, ctxFor( AgentMode::Advisor, "pcJtA" ) );
            const ToolOutcome m = ExecuteTool( ToolCall{ "t2", "mark_journey_best", { { "journey_id", "1" } } }, ctxFor( AgentMode::Advisor, "pcJtA" ) );
            const ToolOutcome r = ExecuteTool( ToolCall{ "t3", "replay_journey", { { "journey_id", 1.5 } } }, ctxFor( AgentMode::Copilot, "pcJtNew" ) );
            d["idType"] = { { "get", text0( g ) }, { "mark", text0( m ) }, { "replay", text0( r ) } };
            idTypeOk = g.isError && m.isError && r.isError && confirms == before
                    && text0( g ).find( "journey_id must be" ) != std::string::npos
                    && text0( m ).find( "journey_id must be" ) != std::string::npos
                    && text0( r ).find( "journey_id must be" ) != std::string::npos;
         }
         // (m) Review m2: mark_journey_best {view_id} keeps THAT image as the end image (not the journey's newest).
         {
            const JBuilt V = JBuildJourney( *store, "pcJtV", "JtView", 60 );
            made.push_back( "pcJtV" );
            fake.steps["pcJtV"] = JHistoryOfImage( *store, V.img );
            JTick( trk, 2 );
            const ToolOutcome copy = ExecuteTool( ToolCall{ "v1", "apply_process", { { "process_id", "PixelMath" },
                                                  { "parameters", { { "expression", "$T" }, { "createNewImage", true }, { "newImageId", "pcJtV2" } } } } },
                                                  ctxFor( AgentMode::Copilot, "pcJtV" ) );
            made.push_back( "pcJtV2" );
            JTick( trk, 3 );
            const bool linked = trk.JourneyOfView( "pcJtV2" ) == V.jid;
            answer = true;
            const ToolOutcome k = ExecuteTool( ToolCall{ "v2", "mark_journey_best", { { "view_id", "pcJtV" } } }, ctxFor( AgentMode::Advisor, "" ) );
            JourneyRow vr;
            store->GetJourney( V.jid, vr );
            d["markView"] = { { "copy", text0( copy ).substr( 0, 200 ) }, { "linked", linked }, { "end", vr.endImageId }, { "want", V.img },
                              { "k", text0( k ) } };
            markViewOk = linked && !k.isError && vr.kept && vr.endImageId == V.img;
         }
         // (n) Review m3 + test gap: the step-note decision of apply_process, every branch (the noEffect ones cannot be
         //     produced by a real bridge here; ProcessApply's own noEffect detection is B10b's).
         {
            auto act = []( bool ok, bool target, bool unverified, bool noEffect, bool added )
            {
               ApplyProcessResult r;
               r.ok = ok; r.targetHistoryStep = target; r.unverifiedChange = unverified; r.noEffect = noEffect; r.historyStepAdded = added;
               return int( JourneyNoteActionFor( r ) );
            };
            const int K = int( JourneyNoteAction::Keep ), C = int( JourneyNoteAction::Cancel ), F = int( JourneyNoteAction::FlagNoEffect );
            const std::vector<int> got = { act( true, true, false, false, false ), act( true, false, false, false, false ),
                                           act( false, true, false, false, false ), act( false, true, true, false, false ),
                                           act( false, true, false, true, true ), act( false, true, false, true, false ),
                                           act( false, false, false, true, false ) };
            const std::vector<int> want = { K, C, C, K, F, C, C };
            d["noteAction"] = { { "got", got }, { "want", want } };
            noteActionOk = got == want;
         }
         // (o) Review m7: paths with spaces lose every directory in text for the model.
         {
            const String a = ModelTextWithoutDirectories( "copy failed: cannot create /home/s/Astro Photos/Scott/2026-09-27 JtM42/x.xisf: denied" );
            const String b = ModelTextWithoutDirectories( "journey database /Users/s/Library/Application Support/PixInsight/journeys/journeys.sqlite3: database is locked" );
            const String c = ModelTextWithoutDirectories( "Kept. In the journey's export folder: the process icon set (.xpsm), recipe.json." );
            d["scrub"] = { { "a", U8( a ) }, { "b", U8( b ) }, { "c", U8( c ) } };
            scrubOk = a == "copy failed: cannot create x.xisf: denied"
                   && b == "journey database journeys.sqlite3: database is locked"
                   && c == "Kept. In the journey's export folder: the process icon set (.xpsm), recipe.json.";
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      for ( const std::string& id : made )
         JForceClose( id );
      const bool ok = schemaOk && promptOk && noHostOk && listOk && getOk && markOk && compareOk && freezeBusyOk
                   && keepErrorOk && startOk && noEffectOk && replayMatchOk && attributionOk && cancelOk
                   && offFreezeOk && inFlightOk && pagingOk && idTypeOk && markViewOk && orderOk && noteActionOk && scrubOk
                   && replayLookupOk && error.IsEmpty();
      out["journeyToolsDetail"] = d;
      out["journeyToolsChecks"] = { { "schema", schemaOk }, { "prompt", promptOk }, { "noHost", noHostOk }, { "list", listOk },
                                    { "get", getOk }, { "mark", markOk }, { "compare", compareOk }, { "freezeBusy", freezeBusyOk },
                                    { "keepError", keepErrorOk }, { "start", startOk }, { "noEffect", noEffectOk },
                                    { "replayMatch", replayMatchOk }, { "attribution", attributionOk }, { "cancel", cancelOk },
                                    { "offFreeze", offFreezeOk }, { "inFlight", inFlightOk }, { "paging", pagingOk },
                                    { "idType", idTypeOk }, { "markView", markViewOk }, { "order", orderOk },
                                    { "noteAction", noteActionOk }, { "scrub", scrubOk }, { "replayLookup", replayLookupOk } };
      out["journeyToolsError"] = U8( error );
      out["journeyToolsOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section J10 live: Opus replays a kept journey (gated) ----------------
   // A keeper K (background down, then a midtones stretch) on one master, a
   // brighter, noisier master N of the same target: through the real tools the
   // model replays K on N step by step and lands near each recorded median. The
   // medians come from the apply_process results (the image's real statistics
   // after each run); in-process runs never reach History (harness fact).
   SelfTestSectionMark( "J10 gated LIVE journey replay" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool liveSkipped = true, liveOk = true;
      String error;
      std::vector<std::string> made;
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         liveSkipped = false;
         liveOk = false;
         try
         {
            JTempDir root( "picopilot-jtl-" );
            String oe;
            std::unique_ptr<JourneyStore> store = JourneyStore::Open( root.Path(), oe );
            if ( !store )
               throw Error( "store: " + oe );
            JourneyTracker trk( store.get() );
            KeeperExporter keeper( store.get() );
            JourneyToolHost host;
            host.store = store.get();
            host.tracker = &trk;
            host.keeper = &keeper;
            host.apiKey = []() { return String(); };
            host.confirmKeeper = []( const String& ) { return false; };
            const JBuilt K = JRecordJourney( *store, "pcJtK",
                                             Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'LiveCone'" }, { "FILTER", "'Ha'" },
                                                   { "INSTRUME", "'ASI2400MC'" }, { "EXPTIME", "300" }, { "NCOMBINE", "20" } } ),
                                             61, { { "$T-0.05", "user", "" }, { "mtf(0.2,$T)", "user", "" } } );
            made.push_back( "pcJtK" );
            store->MarkKept( K.jid, K.img, NowIso() );
            std::vector<double> recorded;
            for ( const StepRow& r : store->Steps( K.img, false ) )
            {
               const std::vector<ChannelStats> s = store->Stats( K.img, r.id );
               recorded.push_back( s.empty() ? -1 : s[0].median );
            }
            {
               ImageWindow w( 96, 64, 1, 32, true, false, true, "pcJtN" );
               View v = w.MainView();
               AutoViewLock lock( v );
               ImageVariant iv = v.Image();
               JFillNoise( static_cast<Image&>( *iv ), 0.15, 0.02, 62 );
            }
            made.push_back( "pcJtN" );
            JSetKeywords( "pcJtN", Kw( { { "IMAGETYP", "'Master Light'" }, { "OBJECT", "'LiveCone'" }, { "FILTER", "'Ha'" },
                                         { "INSTRUME", "'ASI2400MC'" } } ) );
            JTick( trk, 2 );
            if ( trk.JourneyOfView( "pcJtN" ) == 0 )
               throw Error( "fixture: pcJtN is not recorded" );
            ToolContext ctx;
            ctx.mode = AgentMode::Copilot;
            ctx.turnViewId = "pcJtN";
            ctx.journeys = &host;
            std::set<std::string> seen;
            ctx.inspectedViews = &seen;
            AgentSession session;
            StringList notes;
            {
               View nv = ImageWindow::WindowById( "pcJtN" ).MainView();
               session.BeginUserTurn( CaptureViewTurn( String().Format( "Process this image like my kept journey #%lld: replay it "
                                                       "step by step, adapted to this image, without asking me first.",
                                                       static_cast<long long>( K.jid ) ), &nv, notes ) );
            }
            nlohmann::json log = nlohmann::json::array();
            std::vector<double> replayed;
            AgentStep s;
            int requests = 0;
            do
            {
               AnthropicRequest req( String( key ), PICOPILOT_DEFAULT_MODEL, BuildSystemPrompt( AgentMode::Copilot ),
                                     session.History(), PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                     ToolDefinitions( AgentMode::Copilot ), ProductionRequestShape( PICOPILOT_DEFAULT_MODEL ) );
               const AnthropicResult r = req.Perform();
               ++requests;
               s = session.OnResponse( r, [&ctx, &replayed]( const ToolCall& c )
                                       {
                                          const ToolOutcome o = ExecuteTool( c, ctx );
                                          if ( c.name == "apply_process" && !o.isError )
                                             replayed.push_back( JApplyMedian( o ) );
                                          return o;
                                       },
                                       []() { return false; },
                                       [&log]( const String& line ) { log.push_back( U8( line ) ); } );
               if ( s.kind == AgentStep::Failed )
                  error = s.error;
               JTick( trk );
            }
            while ( s.kind == AgentStep::SendAgain && requests <= PICopilotMaxToolRounds );
            bool perStep = replayed.size() >= recorded.size() && !recorded.empty();
            for ( size_t i = 0; perStep && i < recorded.size(); ++i )
               perStep = recorded[i] >= 0 && std::fabs( replayed[i] - recorded[i] ) <= 0.03;
            const double finalMedian = JMedian( ImageWindow::WindowById( "pcJtN" ).MainView(), 0 );
            bool calledReplay = false;
            for ( const nlohmann::json& line : log )
               calledReplay = calledReplay || line.get<std::string>().find( "replay_journey" ) != std::string::npos;
            d = { { "requests", requests }, { "log", log }, { "recorded", recorded }, { "replayed", replayed },
                  { "finalMedian", finalMedian }, { "kind", int( s.kind ) }, { "text", U8( s.assistantText ).substr( 0, 600 ) } };
            liveOk = s.kind == AgentStep::Done && calledReplay && replayed.size() >= 2 && perStep
                  && std::fabs( finalMedian - recorded.back() ) <= 0.03;
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
         catch ( ... )                     { error = "unknown exception"; }
         for ( const std::string& id : made )
            JForceClose( id );
      }
      out["liveReplaySkipped"] = liveSkipped;
      out["liveReplayDetail"] = d;
      out["liveReplayError"] = U8( error );
      out["liveReplayOk"] = liveOk && (liveSkipped || error.IsEmpty());
      allOk = allOk && liveOk && (liveSkipped || error.IsEmpty());
   }

   // ---- Section J11: UI units (Task 11) --------------------------------------
   // The GUI itself cannot be driven headlessly (no modal dialog may open), so
   // every decision behind the strip, the steps dialog, the ⚙ fields and ★ is a
   // function tested here; the panel's own wiring is exercised on the real
   // (hidden) panel with the production JourneyService.
   SelfTestSectionMark( "J11 journey UI units" );
   {
      nlohmann::json d = nlohmann::json::object();
      bool stripOk = false, folderOk = false, dialogOk = false, panelOk = false, keepOk = false, notesOk = false;
      String error;
      std::vector<std::string> made;
      try
      {
         // (a) Strip wording, every state (spec §7).
         {
            JourneyStatus off;       off.state = RecordingState::Off;
            JourneyStatus none;      none.state = RecordingState::NotTracked;
            JourneyStatus paused;    paused.state = RecordingState::Paused;
            paused.reason = "journey database /x/journeys.sqlite3: database is locked (INSERT INTO step)";
            JourneyStatus longPause = paused;
            longPause.reason = String( 'x', 300 );
            JourneyStatus rec;       rec.state = RecordingState::Recording; rec.target = "M42"; rec.kind = "Ha master";
            rec.activeSteps = 7; rec.journeyId = 3;
            JourneyStatus one = rec; one.activeSteps = 1;
            JourneyStatus utf = rec; utf.target = "\xC3\x89toile";   // UTF-8 from the store
            d["strip"] = { U8( JourneyStripText( off ) ), U8( JourneyStripText( none ) ), U8( JourneyStripText( paused ) ),
                           U8( JourneyStripText( rec ) ), U8( JourneyStripText( one ) ), U8( JourneyStripText( utf ) ),
                           U8( JourneyStripText( longPause ) ).size() };
            stripOk = JourneyStripText( off ) == "Journey: recording off"
                   && JourneyStripText( none ) == "Journey: not tracked"
                   && JourneyStripText( paused ) == "Journey: recording paused: journey database /x/journeys.sqlite3: database is locked (INSERT INTO step)"
                   && JourneyStripText( rec ) == FromU8( "Journey: M42 (Ha master) \xC2\xB7 7 steps \xC2\xB7 recording" )
                   && JourneyStripText( one ) == FromU8( "Journey: M42 (Ha master) \xC2\xB7 1 step \xC2\xB7 recording" )
                   && JourneyStripText( utf ) == FromU8( "Journey: \xC3\x89toile (Ha master) \xC2\xB7 7 steps \xC2\xB7 recording" )
                   && JourneyStripText( longPause ).Length() == String( "Journey: recording paused: " ).Length() + 160
                   && JourneyStripText( longPause ).EndsWith( "..." );
         }
         // (b) ⚙ export folder: empty (off) or an absolute, existing folder; never created (Ruling 18).
         {
            JTempDir t( "picopilot-ui-" );
            File::WriteTextFile( t.Path() + "/afile", "x" );
            const String rel = ValidateExportFolderSetting( "keepers" );
            const String missing = ValidateExportFolderSetting( "/nonexistent-picopilot-ui" );
            const String file = ValidateExportFolderSetting( t.Path() + "/afile" );
            d["folder"] = { U8( rel ), U8( missing ), U8( file ) };
            folderOk = ValidateExportFolderSetting( "" ).IsEmpty() && ValidateExportFolderSetting( "   " ).IsEmpty()
                    && ValidateExportFolderSetting( t.Path() ).IsEmpty()
                    && ValidateExportFolderSetting( "  " + t.Path() + "  " ).IsEmpty()
                    && rel.Contains( "absolute" ) && missing.Contains( "does not exist" ) && file.Contains( "not a folder" )
                    && !File::DirectoryExists( "/nonexistent-picopilot-ui" );
         }
         // (c) The steps dialog lists every recorded step except base / noEffect ones, with the
         //     thumbnail as the row icon where one was recorded, who did it, the state, the median
         //     before -> after and the reason (inferred ones marked).
         {
            JTempDir root( "picopilot-ui-store-" );
            String oe;
            std::unique_ptr<JourneyStore> store = JourneyStore::Open( root.Path(), oe );
            if ( !store )
               throw Error( "store: " + oe );
            const JBuilt b = JBuildJourney( *store, "pcUiM", "UiM42", 71 );   // 2 user steps + 1 Copilot step, thumbnails
            made.push_back( "pcUiM" );
            std::vector<StepRow> rows = store->Steps( b.img, true );
            if ( rows.size() != 3 )
               throw Error( String().Format( "fixture: %d steps", int( rows.size() ) ) );
            store->SetStepState( rows[1].id, "undone" );
            store->SetStepReason( rows[0].id, "lift the faint signal", true );
            // A base step and a noEffect step (no thumbnails): never listed.
            HistoryStep h = JPixelMathStep( "$T", 3, "2026-09-25T20:48:00.000Z" );
            StepRow base = MakeStepRow( h, b.img, "active", "user", "", 4 );
            base.params["base"] = true;
            store->AddStep( base );
            StepRow ne = MakeStepRow( JPixelMathStep( "$T", 4, "2026-09-25T20:49:00.000Z" ), b.img, "active", "copilot", "", 5 );
            ne.params["noEffect"] = true;
            store->AddStep( ne );
            JourneyStepsDialog dlg( *store, b.jid );
            nlohmann::json rowsJ = nlohmann::json::array();
            for ( int r = 0; r < dlg.RowCount(); ++r )
            {
               nlohmann::json cells = nlohmann::json::array();
               for ( int c = 0; c < 7; ++c )
                  cells.push_back( U8( dlg.CellText( r, c ) ) );
               rowsJ.push_back( cells );
            }
            d["dialog"] = { { "rows", dlg.RowCount() }, { "icons", dlg.IconCount() }, { "cells", rowsJ },
                            { "preview", U8( dlg.PreviewText() ) },
                            { "title", U8( dlg.TitleText() ) } };
            dialogOk = dlg.RowCount() == 3 && dlg.IconCount() == 3
                    && dlg.CellText( 0, 1 ) == "pcUiM" && dlg.CellText( 0, 2 ) == "PixelMath"
                    && dlg.CellText( 0, 3 ) == "you" && dlg.CellText( 2, 3 ) == "PI Copilot"
                    && dlg.CellText( 1, 4 ) == "undone" && dlg.CellText( 0, 4 ) == "active"
                    && dlg.CellText( 0, 5 ).Contains( FromU8( "\xE2\x86\x92" ) )
                    && dlg.CellText( 0, 6 ) == "lift the faint signal (inferred)"
                    && dlg.CellText( 2, 6 ) == "stretch gently"
                    && dlg.TitleText().Contains( "UiM42" )
                    && dlg.ThumbnailOfRow( 0 ).EndsWith( ".jpg" ) && dlg.ThumbnailOfRow( 3 ).IsEmpty()
                    && dlg.PreviewText() == "Step 3 (after)";   // opens on the newest step with a thumbnail
         }
         // (d) The panel wires the production service into the tool context and the strip.
         JourneyService& svc = JourneyService::Instance();
         if ( ThePICopilotInterface == nullptr )
            throw Error( "ThePICopilotInterface is null" );
         PICopilotInterface& ui = *ThePICopilotInterface;
         if ( ui.GUI == nullptr )
         {
            bool dynamic = false;
            unsigned flags = 0;
            ui.Launch( *ThePICopilotProcess, nullptr, dynamic, flags );
            ui.Hide();
         }
         if ( !svc.Started() || svc.Store() == nullptr )
            throw Error( "the production journey service has no store: " + svc.StoreError() );
         // A journey in the production library the (paused) tracker does not track: ★ finds it
         // through the library (JourneyForView), the strip says "not tracked".
         const JBuilt k = JBuildJourney( *svc.Store(), "pcUiKeep", "UiKeep", 72 );
         made.push_back( "pcUiKeep" );
         {
            ui.m_turnViewId.Clear();
            const ToolContext c = ui.MakeToolContext();
            ui.UpdateJourneyStripFor( "pcUiKeep" );
            const String keepStrip = ui.GUI->JourneyStrip_Label.Text();
            const bool keepDisabled = !ui.m_keepAllowed && !ui.GUI->Keep_ToolButton.IsEnabled();
            // A view the production tracker does record (the pre-phase's), when it is still open.
            const IsoString pre = "pcJourneyPre";
            ui.UpdateJourneyStripFor( pre );
            const JourneyStatus preSt = svc.Tracker().StatusFor( pre );
            const String preStrip = ui.GUI->JourneyStrip_Label.Text();
            const bool preKeep = ui.m_keepAllowed;
            const bool preButton = ui.GUI->Keep_ToolButton.IsEnabled() == (preKeep && ui.IsEnabled());
            d["panel"] = { { "keepStrip", U8( keepStrip ) }, { "keepDisabled", keepDisabled },
                           { "preStrip", U8( preStrip ) }, { "preJourney", preSt.journeyId }, { "preKeepAllowed", preKeep }, { "preButton", preButton },
                           { "tooltip", U8( ui.GUI->JourneyStrip_Label.ToolTip() ) },
                           { "turnInProgress", ui.TurnInProgress() }, { "thread", bool( ui.m_thread ) },
                           { "handling", ui.m_handlingResult }, { "held", ui.m_resultHeld }, { "keepRunning", ui.m_keepRunning },
                           { "panelEnabled", ui.IsEnabled() } };
            panelOk = c.journeys != nullptr && c.journeys->tracker == &svc.Tracker()
                   && c.journeys->store == svc.Store() && c.journeys->keeper == &svc.Keeper()
                   && c.journeys->confirmKeeper && c.journeys->apiKey
                   && keepStrip == "Journey: not tracked" && keepDisabled
                   && preStrip == JourneyStripText( preSt ) && preKeep == (preSt.journeyId != 0) && preButton;
         }
         // (e) ★: every outcome is a visible line in the chat log -- no journey, the user's No,
         //     and a refusal from the library (open transaction; Task 10 concern / integration #4).
         {
            int asked = 0;
            PICopilotInterface::s_confirmKeeperForSelfTest = [&asked]( const String& ) { ++asked; return false; };
            String noJourney, declined, refused;
            try
            {
               noJourney = ui.KeepJourneyOfView( "pcUiNoSuchView" );
               declined = ui.KeepJourneyOfView( "pcUiKeep" );
               {
                  JourneyStore::Transaction tx( *svc.Store() );
                  refused = ui.KeepJourneyOfView( "pcUiKeep" );
               }   // rolled back: nothing was written
            }
            catch ( ... )
            {
               PICopilotInterface::s_confirmKeeperForSelfTest = nullptr;
               throw;
            }
            PICopilotInterface::s_confirmKeeperForSelfTest = nullptr;
            JourneyRow jr;
            svc.Store()->GetJourney( k.jid, jr );
            const String log = ui.GUI->ChatLog.Text();
            d["keep"] = { { "noJourney", U8( noJourney ) }, { "declined", U8( declined ) }, { "refused", U8( refused ) },
                          { "asked", asked }, { "kept", jr.kept } };
            keepOk = noJourney.Contains( "not part of a recorded journey" )
                  && declined.Contains( "Not kept" ) && refused.Contains( "keeping journey failed" )
                  && refused.Contains( "transaction" ) && asked == 1 && !jr.kept
                  && log.Contains( noJourney ) && log.Contains( declined ) && log.Contains( refused );
         }
         // (f) Service notes reach the chat log, but never inside a live-streamed reply.
         {
            (void)svc.TakeNotes();
            svc.AddNote( "pc-ui-note-held" );
            ui.m_replyShown = true;
            ui.DrainJourneyNotes();
            ui.m_replyShown = false;
            const bool heldBack = !ui.GUI->ChatLog.Text().Contains( "pc-ui-note-held" );
            ui.DrainJourneyNotes();
            const bool shown = ui.GUI->ChatLog.Text().Contains( "(pc-ui-note-held)" );
            const bool drained = svc.TakeNotes().IsEmpty();
            d["notes"] = { { "heldBack", heldBack }, { "shown", shown }, { "drained", drained } };
            notesOk = heldBack && shown && drained;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      for ( const std::string& id : made )
         JForceClose( id );
      const bool ok = stripOk && folderOk && dialogOk && panelOk && keepOk && notesOk && error.IsEmpty();
      out["journeyUiDetail"] = d;
      out["journeyUiChecks"] = { { "strip", stripOk }, { "folder", folderOk }, { "dialog", dialogOk },
                                 { "panel", panelOk }, { "keep", keepOk }, { "notes", notesOk } };
      out["journeyUiError"] = U8( error );
      out["journeyUiOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- journey sections end ----
   SelfTestSectionMark( nullptr );

   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }
   return allOk;
}

} // namespace pcl
