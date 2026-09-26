// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentTools.h"
#include "EvalGuard.h"
#include "HistoryReader.h"
#include "JourneyConstants.h"
#include "JourneyStore.h"
#include "JourneySpikeProbe.h"
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
#include <map>
#include <set>
#include <string>
#include <thread>
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
   };
   return handlers;
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
