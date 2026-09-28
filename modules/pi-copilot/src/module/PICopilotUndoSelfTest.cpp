// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotUndoSelfTest.h"
#include "AgentSession.h"
#include "AgentTools.h"
#include "AnthropicClient.h"
#include "EvalGuard.h"
#include "JourneyStore.h"
#include "JourneyTools.h"
#include "JourneyTracker.h"
#include "PICopilotModule.h"
#include "ProcessActivity.h"
#include "SelfTestTiming.h"
#include "SystemPrompt.h"
#include "Utf8.h"
#include "ViewCapture.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Variant.h>
#include <pcl/View.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pcl
{

namespace
{

using uclock = std::chrono::steady_clock;

void UPump( int ms )
{
   const uclock::time_point t0 = uclock::now();
   while ( std::chrono::duration<double, std::milli>( uclock::now() - t0 ).count() < ms )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
   }
}

// Like JWaitIdle (Section J6): a tick must never be silently skipped.
void UWaitIdle()
{
   const uclock::time_point t0 = uclock::now();
   for ( ;; )
   {
      const ProcessActivityState a = CurrentProcessActivity();
      if ( !a.busy )
         return;
      if ( std::chrono::duration<double>( uclock::now() - t0 ).count() > 3 )
         throw Error( "PixInsight stayed busy for 3 s (" + a.reason + "); the tracker cannot tick" );
      ThePICopilotModule->ProcessEvents( true );
      std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
   }
}

void UTick( JourneyTracker& t, int n = 1 )
{
   for ( int i = 0; i < n; ++i )
   {
      UWaitIdle();
      t.Tick( JourneyWallNow(), true/*forceScan*/ );
      UPump( 60 );
   }
}

void UForceClose( const char* id )
{
   try
   {
      ImageWindow w = ImageWindow::WindowById( IsoString( id ) );
      if ( !w.IsNull() )
         w.ForceClose();
   }
   catch ( ... )
   {
   }
}

// View by full id, re-resolved at every call (never kept).
View UView( const char* fullId )
{
   const View v = View::ViewById( IsoString( fullId ) );
   if ( v.IsNull() )
      throw Error( String( "no view " ) + fullId );
   return v;
}

double UPixel( const char* fullId )
{
   const View v = UView( fullId );
   ImageVariant iv = v.Image();
   if ( !iv.IsFloatSample() || iv.BitsPerSample() != 32 )
      throw Error( String( "not a 32-bit float image: " ) + fullId );
   return double( static_cast<const Image&>( *iv ).Pixel( 0, 0 ) );
}

// {historyIndex, processing.length} of a view, read through PJSR (PCL has no accessor).
std::pair<int, int> UHistoryPos( const char* fullId )
{
   const Variant r = ThePICopilotModule->EvaluateScript(
      String( "(function(){ var v = View.viewById( \"" ) + fullId + "\" ); return v.isNull ? \"-1 -1\" : "
      "v.historyIndex + \" \" + v.processing.length; })()", "JavaScript" );
   const IsoString s = r.IsValid() ? IsoString( r.ToString() ) : IsoString();
   int a = -1, b = -1;
   if ( std::sscanf( s.c_str(), "%d %d", &a, &b ) != 2 )
      throw Error( "history position read of " + String( fullId ) + " failed: '" + String( s ) + "'" );
   return { a, b };
}

std::string Text0( const ToolOutcome& o )
{
   return o.content.is_array() && !o.content.empty() ? o.content.at( 0 ).value( "text", std::string() ) : std::string();
}

bool Has( const std::string& s, const char* what )
{
   return s.find( what ) != std::string::npos;
}

// ---- Phase "undo": USER vs TOOL undo, as the journey tracker records them ----

const char* const kToolView = "pcUndoTool";
const char* const kUserView = "pcUndoUser";

struct UndoPhaseState
{
   std::unique_ptr<JourneyStore>   store;   // declaration order: the tracker goes first
   std::unique_ptr<JourneyTracker> trk;
   String                          root;
   int64                           toolImg = 0, userImg = 0, toolJ = 0, userJ = 0;
   std::string                     toolJourneyName;
   nlohmann::json                  d = nlohmann::json::object();
   std::vector<std::string>        errors;
   std::map<std::string, bool>     ok;
};

UndoPhaseState& UP()
{
   static UndoPhaseState s;
   return s;
}

// Every recorded row of an image (superseded included): "<seq>:<process>:<state>:<actor>".
std::vector<std::string> URows( JourneyStore& store, int64 img )
{
   std::vector<std::string> r;
   for ( const StepRow& s : store.Steps( img, true ) )
      if ( !s.params.value( "base", false ) )
         r.push_back( std::to_string( s.seq ) + ":" + s.processId + ":" + s.state + ":" + s.actor );
   return r;
}

// The rows without seq (the two images' base histories may differ in length).
std::vector<std::string> UShape( JourneyStore& store, int64 img )
{
   std::vector<std::string> r;
   for ( const StepRow& s : store.Steps( img, true ) )
      if ( !s.params.value( "base", false ) )
         r.push_back( s.processId + ":" + s.state + ":" + s.actor + ":" + s.reason );
   return r;
}

ToolContext UToolCtx( UndoPhaseState& st, JourneyToolHost& host )
{
   host.store = st.store.get();
   host.tracker = st.trk.get();
   ToolContext ctx;
   ctx.mode = AgentMode::Copilot;
   ctx.turnViewId = kToolView;
   ctx.journeys = &host;
   return ctx;
}

void UndoStep( UndoPhaseState& st, const std::string& step, const nlohmann::json& payload )
{
   if ( step == "begin" )
   {
      st.root = File::UniqueFileName( File::SystemTempDirectory(), 12, "picopilot-undo-" );
      if ( !st.root.StartsWith( '/' ) )
         st.root = File::SystemTempDirectory() + '/' + st.root;
      File::CreateDirectory( st.root );
      String oe;
      st.store = JourneyStore::Open( st.root, oe );
      if ( !st.store )
         throw Error( "store: " + oe );
      st.trk.reset( new JourneyTracker( st.store.get() ) );
      UTick( *st.trk, 8 );   // earlier fixtures' windows spread over several ticks (per-tick cap)
      st.toolImg = st.trk->ImageOfView( kToolView );
      st.userImg = st.trk->ImageOfView( kUserView );
      st.toolJ = st.trk->JourneyOfView( kToolView );
      st.userJ = st.trk->JourneyOfView( kUserView );
      JourneyRow jr;
      st.store->GetJourney( st.toolJ, jr );
      st.toolJourneyName = jr.name;
      st.d["begin"] = { { "toolImg", st.toolImg }, { "userImg", st.userImg }, { "name", jr.name } };
      st.ok["joined"] = st.toolImg != 0 && st.userImg != 0 && st.toolJ != 0 && st.userJ != 0 && st.toolJ != st.userJ;
      return;
   }
   if ( !st.trk )
      throw Error( "no tracker (step begin did not complete)" );
   JourneyTracker& trk = *st.trk;
   JourneyStore& store = *st.store;

   if ( step == "recorded" )
   {
      UTick( trk, 2 );
      st.d["recorded"] = { { "tool", URows( store, st.toolImg ) }, { "user", URows( store, st.userImg ) } };
      st.ok["recorded"] = URows( store, st.toolImg ).size() == 3 && UShape( store, st.toolImg ) == UShape( store, st.userImg );
   }
   else if ( step == "toolUndo" || step == "toolRedo" )
   {
      // The USER's step was already made at top level (selftest.js); the TOOL makes the same one here.
      const bool undo = step == "toolUndo";
      const std::pair<int, int> before = UHistoryPos( kToolView );
      JourneyToolHost host;
      const ToolContext ctx = UToolCtx( st, host );
      const ToolOutcome o = ExecuteTool( ToolCall{ "toolu_" + step, "history_step",
                                                   { { "direction", undo ? "undo" : "redo" },
                                                     { "count", payload.at( "count" ).get<int>() } } }, ctx );
      const std::pair<int, int> after = UHistoryPos( kToolView );
      const std::vector<std::string> notes = trk.CopilotNotesForSelfTest();
      st.d[step] = { { "isError", o.isError }, { "text", Text0( o ) }, { "log", U8( o.logLine ) },
                     { "before", { before.first, before.second } }, { "after", { after.first, after.second } },
                     { "copilotNotes", notes } };
      st.ok[step + ".tool"] = !o.isError && o.mutated && notes.empty()
                           && after.first == before.first + (undo ? -1 : 1)*payload.at( "count" ).get<int>()
                           && after.second == before.second;
   }
   else if ( step == "compare" )
   {
      // After the user's and the tool's step (and a tick): the two images' journeys are identical.
      const std::string label = payload.at( "label" ).get<std::string>();
      UTick( trk, 2 );
      const std::vector<std::string> t = UShape( store, st.toolImg ), u = UShape( store, st.userImg );
      JourneyRow jr;
      store.GetJourney( st.toolJ, jr );
      const std::vector<std::string> notes = trk.CopilotNotesForSelfTest();
      st.d["compare." + label] = { { "tool", URows( store, st.toolImg ) }, { "user", URows( store, st.userImg ) },
                                   { "name", jr.name }, { "copilotNotes", notes },
                                   { "positions", payload.value( "positions", nlohmann::json() ) } };
      int undone = 0, superseded = 0, copilot = 0;
      for ( const std::string& r : t )
      {
         undone += Has( r, ":undone:" ) ? 1 : 0;
         superseded += Has( r, ":superseded:" ) ? 1 : 0;
         copilot += Has( r, ":copilot:" ) ? 1 : 0;
      }
      const int wantUndone = payload.at( "undone" ).get<int>(), wantSuperseded = payload.at( "superseded" ).get<int>();
      const int wantRows = payload.at( "rows" ).get<int>();
      // Identical to the user's (no phantom step, no Copilot row, no pending note), the expected
      // state counts, and the journey keeps its name (never renamed as a replay).
      st.ok["compare." + label] = t == u && int( t.size() ) == wantRows && undone == wantUndone && superseded == wantSuperseded
                               && copilot == 0 && notes.empty() && jr.name == st.toolJourneyName;
   }
   else if ( step == "end" )
   {
      st.trk.reset();
      st.store.reset();
      try { RemoveDirectoryTree( st.root ); } catch ( ... ) {}
   }
   else
      throw Error( String( "undo: unknown step " ) + step.c_str() );
}

} // namespace

nlohmann::json PhaseUndoTool( const nlohmann::json& payload )
{
   UndoPhaseState& st = UP();
   const std::string step = payload.value( "step", std::string( "?" ) );
   try
   {
      UndoStep( st, step, payload );
   }
   catch ( const pcl::Exception& x ) { st.errors.push_back( step + ": " + U8( x.Message() ) ); }
   catch ( const std::exception& x ) { st.errors.push_back( step + ": " + x.what() ); }
   catch ( ... )                     { st.errors.push_back( step + ": unknown exception" ); }
   return { { "step", step } };
}

bool RunUndoSelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section JU: history_step (undo / redo) --------------------------------------------
   SelfTestSectionMark( "JU history_step tool (undo/redo)" );
   {
      nlohmann::json d = nlohmann::json::object();
      std::map<std::string, bool> ok;
      String error;
      // Fixtures (selftest.js, top level): pcUndoUnit PixelMath 0.2, $T*1.5, $T+0.1 (0.2, 0.3, 0.4), with a
      // preview "pv" holding one step (0.9); pcUndoOther one step (0.6).
      const char* unit = "pcUndoUnit";
      const char* pv = "pcUndoUnit->pv";
      try
      {
         auto run = [&]( AgentMode mode, const IsoString& turn, const nlohmann::json& in, int* confirmCalls = nullptr,
                         bool approve = true, std::string* dialog = nullptr, std::set<std::string>* inspected = nullptr )
         {
            ToolContext ctx;
            ctx.mode = mode;
            ctx.turnViewId = turn;
            ctx.inspectedViews = inspected;
            ctx.confirm = []( const String&, const String&, const String& ) { return true; };
            ctx.confirmHistory = [=]( const String& html ) {
               if ( confirmCalls != nullptr ) ++*confirmCalls;
               if ( dialog != nullptr ) *dialog = U8( html );
               return approve;
            };
            return ExecuteTool( ToolCall{ "toolu_ju", "history_step", in }, ctx );
         };
         auto pos = [&]( const char* id ) { const std::pair<int, int> p = UHistoryPos( id ); return nlohmann::json::array( { p.first, p.second } ); };
         const nlohmann::json start = pos( unit );
         d["start"] = { { "unit", start }, { "px", UPixel( unit ) }, { "pv", pos( pv ) } };
         ok["fixture"] = start == nlohmann::json::array( { 3, 3 } ) && std::fabs( UPixel( unit ) - 0.4 ) < 1e-6
                      && pos( pv ) == nlohmann::json::array( { 1, 1 } );

         // (a) Offered in Copilot and Guided, never in Advisor; waits for idle like apply_process; the prompt says so.
         {
            auto has = []( const nlohmann::json& tools ) {
               for ( const nlohmann::json& t : tools )
                  if ( t.at( "name" ) == "history_step" )
                     return t.at( "input_schema" ).at( "required" ) == nlohmann::json::array( { "direction" } );
               return false;
            };
            const nlohmann::json tu = { { "type", "tool_use" }, { "id", "x" }, { "name", "history_step" }, { "input", nlohmann::json::object() } };
            ok["schema"] = has( ToolDefinitions( AgentMode::Copilot ) ) && has( ToolDefinitions( AgentMode::Guided ) )
                        && !has( ToolDefinitions( AgentMode::Advisor ) )
                        && ResponseCallsImageChangingTool( nlohmann::json::array( { tu } ) );
            const String pc = BuildSystemPrompt( AgentMode::Copilot ), pg = BuildSystemPrompt( AgentMode::Guided ),
                         pa = BuildSystemPrompt( AgentMode::Advisor );
            ok["prompt"] = pc.Contains( "history_step {" ) && pg.Contains( "history_step {" ) && !pa.Contains( "history_step" )
                        && pc.Contains( "undo the last" );
         }

         // (b) Advisor refuses; nothing moves.
         {
            const ToolOutcome o = run( AgentMode::Advisor, unit, { { "direction", "undo" } } );
            d["advisor"] = Text0( o );
            ok["advisor"] = o.isError && Has( Text0( o ), "not available in Advisor" ) && pos( unit ) == start;
         }

         // (c) Input validation: every bad count / direction is a precise error and nothing moves. The
         //     int64 checks run BEFORE narrowing (Task 10 R1): 4294967297 would narrow to 1 as a 32-bit int.
         {
            nlohmann::json bad = nlohmann::json::array();
            bool all = true;
            const std::vector<nlohmann::json> inputs = {
               { { "direction", "sideways" } },
               nlohmann::json::object(),
               { { "direction", "undo" }, { "count", 0 } },
               { { "direction", "undo" }, { "count", -1 } },
               { { "direction", "undo" }, { "count", 1.5 } },
               { { "direction", "undo" }, { "count", "2" } },
               { { "direction", "undo" }, { "count", 4 } },                                   // deeper than the 3 steps
               { { "direction", "undo" }, { "count", int64( 4294967297LL ) } },
               { { "direction", "undo" }, { "count", uint64_t( 9223372036854775808ULL ) } },
               { { "direction", "undo" }, { "count", uint64_t( 18446744073709551615ULL ) } },
               { { "direction", "redo" } } };                                                 // nothing to redo
            for ( const nlohmann::json& in : inputs )
            {
               const ToolOutcome o = run( AgentMode::Copilot, unit, in );
               const bool fine = o.isError && !o.mutated && pos( unit ) == start && !Text0( o ).empty();
               all = all && fine;
               bad.push_back( { { "in", in }, { "isError", o.isError }, { "text", Text0( o ) } } );
            }
            d["bad"] = bad;
            ok["validation"] = all && Has( bad[0]["text"].get<std::string>(), "\"undo\" or \"redo\"" )
                            && Has( bad[6]["text"].get<std::string>(), "only 3" )
                            && Has( bad[7]["text"].get<std::string>(), "only 3" )
                            && Has( bad[10]["text"].get<std::string>(), "nothing to redo" );
         }

         // (d) Guided asks first: declined -> nothing moves; the dialog names the steps.
         {
            int calls = 0;
            std::string html;
            const ToolOutcome no = run( AgentMode::Guided, unit, { { "direction", "undo" }, { "count", 2 } }, &calls, false, &html );
            d["guidedNo"] = { { "text", Text0( no ) }, { "calls", calls }, { "html", html } };
            ok["guidedDecline"] = no.isError && Has( Text0( no ), "declined" ) && calls == 1 && pos( unit ) == start
                               && Has( html, "PixelMath" ) && Has( html, "Undo" ) && Has( html, unit );
         }

         // (e) Copilot: undo 2 -> position 1, pixels at step 1, the result names both stepped entries.
         {
            const ToolOutcome o = run( AgentMode::Copilot, unit, { { "direction", "undo" }, { "count", 2 } } );
            const std::string t = Text0( o );
            nlohmann::json r;
            try { r = nlohmann::json::parse( t ); } catch ( ... ) {}
            d["undo2"] = { { "text", t.substr( 0, 1500 ) }, { "log", U8( o.logLine ) }, { "pos", pos( unit ) }, { "px", UPixel( unit ) },
                           { "blocks", o.content.size() } };
            bool stepsOk = r.is_object() && r.value( "steps", nlohmann::json() ).is_array() && r["steps"].size() == 2;
            if ( stepsOk )
               for ( const nlohmann::json& s : r["steps"] )
                  stepsOk = stepsOk && s.value( "process", std::string() ) == "PixelMath" && s.value( "step", 0 ) > 0;
            ok["undo"] = !o.isError && o.mutated && stepsOk && r.value( "historyIndex", -1 ) == 1
                      && r.value( "undoAvailable", -1 ) == 1 && r.value( "redoAvailable", -1 ) == 2
                      && pos( unit ) == nlohmann::json::array( { 1, 3 } ) && std::fabs( UPixel( unit ) - 0.2 ) < 1e-6
                      && U8( o.logLine ).rfind( "\xE2\x96\xB6 history_step undo 2", 0 ) == 0
                      && o.content.size() == 2;   // the summary + a fresh preview
         }

         // (f) Redo (count defaults to 1), then Guided approved redo 1 -> back at 3.
         {
            const ToolOutcome o1 = run( AgentMode::Copilot, unit, { { "direction", "redo" } } );
            const nlohmann::json p1 = pos( unit );
            const double px1 = UPixel( unit );
            int calls = 0;
            const ToolOutcome o2 = run( AgentMode::Guided, unit, { { "direction", "redo" }, { "count", 1 } }, &calls, true );
            d["redo"] = { { "t1", Text0( o1 ).substr( 0, 600 ) }, { "p1", p1 }, { "px1", px1 }, { "t2", Text0( o2 ).substr( 0, 600 ) },
                          { "calls", calls }, { "p2", pos( unit ) }, { "px2", UPixel( unit ) } };
            ok["redo"] = !o1.isError && p1 == nlohmann::json::array( { 2, 3 } ) && std::fabs( px1 - 0.3 ) < 1e-6
                      && !o2.isError && calls == 1 && pos( unit ) == start && std::fabs( UPixel( unit ) - 0.4 ) < 1e-6;
         }

         // (g) Targeting: another view needs get_view_context first (same rule as apply_process).
         {
            std::set<std::string> inspected;
            const ToolOutcome no = run( AgentMode::Copilot, unit, { { "direction", "undo" }, { "view_id", "pcUndoOther" } },
                                        nullptr, true, nullptr, &inspected );
            inspected.insert( "pcUndoOther" );
            const ToolOutcome yes = run( AgentMode::Copilot, unit, { { "direction", "undo" }, { "view_id", "pcUndoOther" } },
                                         nullptr, true, nullptr, &inspected );
            const ToolOutcome none = run( AgentMode::Copilot, unit, { { "direction", "undo" }, { "view_id", "pcNoSuchView" } } );
            const ToolOutcome gone = run( AgentMode::Copilot, "pcUndoGone", { { "direction", "undo" } } );
            d["target"] = { { "no", Text0( no ) }, { "yes", Text0( yes ).substr( 0, 400 ) }, { "none", Text0( none ) },
                            { "gone", Text0( gone ) }, { "other", pos( "pcUndoOther" ) } };
            ok["target"] = no.isError && Has( Text0( no ), "get_view_context" ) && !yes.isError
                        && pos( "pcUndoOther" ) == nlohmann::json::array( { 0, 1 } )
                        && none.isError && Has( Text0( none ), "no view with id" )
                        && gone.isError && Has( Text0( gone ), "no longer open" ) && pos( unit ) == start;
         }

         // (h) Busy: a locked view and a running script / EvaluateScript are refused, never waited for.
         {
            std::string locked, script;
            bool lockedOk = false, scriptOk = false;
            {
               View v = UView( unit );
               AutoViewLock lock( v );
               const ToolOutcome o = run( AgentMode::Copilot, unit, { { "direction", "undo" } } );
               locked = Text0( o );
               lockedOk = o.isError && Has( locked, "busy" );
            }
            {
               EvalDepthGuard busyScript;   // as if another EvaluateScript were running
               const ToolOutcome o = run( AgentMode::Copilot, unit, { { "direction", "undo" } } );
               script = Text0( o );
               scriptOk = o.isError && Has( script, "script" );
            }
            d["busy"] = { { "locked", locked }, { "script", script } };
            ok["busy"] = lockedOk && scriptOk && pos( unit ) == start;
         }

         // (i) Previews: a preview steps ITS OWN history, never the main image's.
         {
            const ToolOutcome o = run( AgentMode::Copilot, pv, { { "direction", "undo" } } );
            nlohmann::json r;
            try { r = nlohmann::json::parse( Text0( o ) ); } catch ( ... ) {}
            d["preview"] = { { "text", Text0( o ).substr( 0, 800 ) }, { "pv", pos( pv ) }, { "unit", pos( unit ) } };
            ok["preview"] = !o.isError && pos( pv ) == nlohmann::json::array( { 0, 1 } ) && pos( unit ) == start
                         && r.value( "scope", std::string() ) == "preview";
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      // (j) Journey integration (phase "undo"): a tool undo / redo is recorded exactly like the user's.
      {
         UndoPhaseState& st = UP();
         for ( const char* k : { "joined", "recorded", "toolUndo.tool", "compare.undo", "toolRedo.tool", "compare.redo",
                                 "compare.branch" } )
            ok[std::string( "journey." ) + k] = st.ok.count( k ) > 0 && st.ok[k];
         ok["journey.noErrors"] = st.errors.empty();
         d["journey"] = st.d;
         d["journeyErrors"] = st.errors;
      }

      bool all = error.IsEmpty();
      nlohmann::json checks = nlohmann::json::object();
      for ( const auto& kv : ok )
      {
         checks[kv.first] = kv.second;
         all = all && kv.second;
      }
      for ( const char* k : { "fixture", "schema", "prompt", "advisor", "validation", "guidedDecline", "undo", "redo",
                              "target", "busy", "preview" } )
         all = all && ok.count( k ) > 0;
      out["historyStepChecks"] = checks;
      out["historyStepDetail"] = d;
      out["historyStepError"] = U8( error );
      out["historyStepOk"] = all;
      allOk = allOk && all;
      for ( const char* id : { "pcUndoUnit", "pcUndoOther", kToolView, kUserView } )
         UForceClose( id );
   }

   // ---- Section JU live: the model undoes a step when asked (gated) ------------------------
   SelfTestSectionMark( "JU live history_step (gated)" );
   {
      bool liveSkipped = true, liveOk = true;
      String error;
      nlohmann::json log = nlohmann::json::array();
      nlohmann::json p0, p1;
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         liveSkipped = false;
         liveOk = false;
         try
         {
            // Fixture pcUndoLive (top level): PixelMath 0.2 then 0.5.
            const char* id = "pcUndoLive";
            const std::pair<int, int> before = UHistoryPos( id );
            p0 = { before.first, before.second };
            ToolContext ctx;
            ctx.mode = AgentMode::Copilot;
            ctx.turnViewId = id;
            AgentSession session;
            StringList notes;
            {
               View v = UView( id );
               session.BeginUserTurn( CaptureViewTurn( "Undo the last step on this image.", &v, notes ) );
            }
            AgentStep s;
            int requests = 0;
            do
            {
               AnthropicRequest req( String( key ), PICOPILOT_DEFAULT_MODEL, BuildSystemPrompt( AgentMode::Copilot ),
                                     session.History(), PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                     ToolDefinitions( AgentMode::Copilot ), ProductionRequestShape( PICOPILOT_DEFAULT_MODEL ) );
               const AnthropicResult r = req.Perform();
               ++requests;
               s = session.OnResponse( r, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); },
                                       []() { return false; },
                                       [&log]( const String& line ) { log.push_back( U8( line ) ); } );
               if ( s.kind == AgentStep::Failed )
                  error = s.error;
            }
            while ( s.kind == AgentStep::SendAgain && requests <= PICopilotMaxToolRounds );
            const std::pair<int, int> after = UHistoryPos( id );
            p1 = { after.first, after.second };
            bool called = false, applied = false;
            for ( const nlohmann::json& line : log )
            {
               called = called || line.get<std::string>().rfind( "\xE2\x96\xB6 history_step undo", 0 ) == 0;
               applied = applied || line.get<std::string>().rfind( "\xE2\x96\xB6 apply_process", 0 ) == 0;
            }
            liveOk = s.kind == AgentStep::Done && called && !applied && after.first == before.first - 1
                  && after.second == before.second && std::fabs( UPixel( id ) - 0.2 ) < 1e-6;
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
         catch ( ... )                     { error = "unknown exception"; }
      }
      out["liveHistoryStepSkipped"] = liveSkipped;
      out["liveHistoryStepLog"] = log;
      out["liveHistoryStepPositions"] = { p0, p1 };
      out["liveHistoryStepError"] = U8( error );
      out["liveHistoryStepOk"] = liveOk;
      allOk = allOk && liveOk;
      UForceClose( "pcUndoLive" );
   }
   SelfTestSectionMark( nullptr );
   return allOk;
}

} // namespace pcl
