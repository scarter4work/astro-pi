// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

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

// j0.timerApply.arm: {id} -- the probe's next timer tick applies PixelMath to id.
nlohmann::json PhaseTimerApplyArm( const nlohmann::json& payload )
{
   JourneySpikeProbeRequestTimerApply( payload.at( "id" ).get<std::string>() );
   return payload;
}

// j0.timerApply.check: the top-level history read (payload) + the tick's result.
nlohmann::json PhaseTimerApplyCheck( const nlohmann::json& payload )
{
   return { { "history", payload }, { "tick", JourneySpikeProbeTimerApplyResult() } };
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

   // ---- journey sections end ----

   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }
   return allOk;
}

} // namespace pcl
