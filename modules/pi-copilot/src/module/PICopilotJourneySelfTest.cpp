// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentTools.h"
#include "EvalGuard.h"
#include "HistoryReader.h"
#include "JourneyConstants.h"
#include "JourneyExport.h"
#include "JourneyStore.h"
#include "JourneySpikeProbe.h"
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
#include "Utf8.h"
#include "ViewPreview.h"
#include "SelfTestTiming.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Bitmap.h>
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
class JWindow
{
public:

   JWindow( const char* id, int w, int h, int channels, double value )
      : m_window( w, h, channels, 32, true/*float*/, channels >= 3/*color*/, true/*initialProcessing*/, IsoString( id ) )
   {
      if ( m_window.IsNull() )
         throw Error( String( "JWindow: null window " ) + id );
      View v = m_window.MainView();
      AutoViewLock lock( v );
      ImageVariant iv = v.Image();
      static_cast<Image&>( *iv ).Fill( float( value ) );
   }

   ~JWindow()
   {
      try { if ( !m_window.IsNull() ) m_window.ForceClose(); } catch ( ... ) {}
   }

   JWindow( const JWindow& ) = delete;
   JWindow& operator =( const JWindow& ) = delete;

   View MainView() const { return m_window.MainView(); }
   ImageWindow Window() const { return m_window; }

private:

   ImageWindow m_window;
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

nlohmann::json PhaseJourneyExport( const nlohmann::json& payload );   // j7.exp (Section J7's fixture, below)

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
      { "j7.exp",      PhaseJourneyExport },
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
            for ( const nlohmann::json& e : JourneySpikeProbeReport().at( "events" ) )
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
           fileModesOk = false, rootLinkOk = false, dbLinkOk = false, openRootOk = false, treeOk = false, openLockedOk = false;
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
               { "image", { "id", "journey_id", "view_id", "file_path", "fingerprint", "is_master", "created" } },
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
            all = all && tables == std::vector<std::string>( { "acquisition", "gap", "image", "journey", "link", "stats", "step" } );
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
                        { "openLocked", openLockedOk } };
      const bool ok = schemaOk && roundTripOk && utf8Ok && stateOk && redactOk && retentionOk && damagedOk && newerOk
                   && foreignOk && lockedOk && isoOk && fileModesOk && rootLinkOk && dbLinkOk && openRootOk && treeOk
                   && openLockedOk;
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
           fingerprintOk = false, auxOk = false, hardenOk = false;
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
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      const bool ok = detectOk && wbppOk && sirilOk && iiTableOk && redactOk && namesOk && fingerprintOk && auxOk && hardenOk;
      out["masterFactsDetail"] = d;
      out["masterFactsError"] = U8( error );
      out["masterFactsOk"] = ok;
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
