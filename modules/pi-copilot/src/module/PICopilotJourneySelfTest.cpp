// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "EvalGuard.h"
#include "HistoryReader.h"
#include "JourneyConstants.h"
#include "JourneySpikeProbe.h"
#include "PICopilotInterface.h"
#include "PICopilotJourneySelfTest.h"
#include "PICopilotModule.h"
#include "PICopilotProcess.h"
#include "PjsrRunner.h"
#include "ProcessApply.h"
#include "Utf8.h"
#include "ViewPreview.h"

#include <pcl/AutoViewLock.h>
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

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

void JRemoveTree( const String& dir )
{
   if ( dir.IsEmpty() || !File::DirectoryExists( dir ) )
      return;
   StringList subdirs;
   FindFileInfo info;
   for ( File::Find f( dir + "/*" ); f.NextItem( info ); )
   {
      if ( info.name == "." || info.name == ".." )
         continue;
      if ( info.IsDirectory() )
         subdirs << dir + '/' + info.name;
      else
         File::Remove( dir + '/' + info.name );
   }
   for ( const String& s : subdirs )
      JRemoveTree( s );
   File::RemoveDirectory( dir );
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
      try { JRemoveTree( m_path ); } catch ( ... ) {}
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
   bool liveReadOk = false, undoRedoOk = false, branchOk = false, maskOk = false, renameOk = false, reopenOk = false;
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

   // ---- Section J1: vendored SQLite (Task 2) -------------------------------
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
   // Pure parser/diff checks run here; every check of a LIVE history reads the
   // pcHrA fixture that test/selftest.js built at top level, through the j2.hr
   // phases (J2State), because a step executed in-process is never recorded.
   {
      nlohmann::json d = nlohmann::json::object();
      bool parseOk = false, typesOk = false, identityOk = false, notReplayableOk = false, badXmlOk = false,
           costOk = false, integrationIdOk = false, busyOk = false, phasesOk = false;
      String error;
      std::vector<std::string> made = { "pcHrA", "pcHrRenamed", "pcHrMask", "pcHrLong" };
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

         // (e)-(i) Live history, read by the j2.hr phases between top-level steps.
         const std::vector<std::string> wantSteps = { "live0", "undo", "redo", "branch", "mask", "rename", "reopen" };
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
            busyOk = depth0 == 0 && depthHeld == 1 && held.busy && !held.ok && held.error.IsEmpty() && held.steps.empty()
                  && after.ok && !after.busy && EvaluateScriptDepth() == 0;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      for ( const std::string& id : made )
         JForceClose( id );
      const bool ok = parseOk && typesOk && identityOk && notReplayableOk && badXmlOk && integrationIdOk
                   && phasesOk && st.liveReadOk && st.undoRedoOk && st.branchOk && st.maskOk && st.renameOk && st.reopenOk
                   && costOk && busyOk;
      d["verdicts"] = { { "parse", parseOk }, { "types", typesOk }, { "identity", identityOk }, { "notReplayable", notReplayableOk },
                        { "badXml", badXmlOk }, { "integrationId", integrationIdOk }, { "phases", phasesOk },
                        { "liveRead", st.liveReadOk }, { "undoRedo", st.undoRedoOk }, { "branch", st.branchOk },
                        { "mask", st.maskOk }, { "rename", st.renameOk }, { "reopen", st.reopenOk },
                        { "cost", costOk }, { "busy", busyOk } };
      out["historyReaderDetail"] = d;
      out["historyReaderError"] = U8( error );
      out["historyReaderOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- journey sections end ----

   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }
   return allOk;
}

} // namespace pcl
