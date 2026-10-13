// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ScriptTools.h"
#include "EvalGuard.h"          // CoreScriptGuard
#include "HistoryReader.h"      // ReadViewHistory (journey attribution)
#include "JourneyTools.h"       // JourneyToolHost
#include "JourneyTracker.h"
#include "ModuleScript.h"       // console capture (module's own fixed script)
#include "ProcessApply.h"       // OpenMainViewIds
#include "ScriptCatalog.h"
#include "ScriptSource.h"
#include "TextSafety.h"
#include "ToolHelpers.h"        // Fail, TextBlock, StringField, IsBusy
#include "Utf8.h"
#include "ViewContext.h"

#include <pcl/ByteArray.h>
#include <pcl/Console.h>
#include <pcl/Exception.h>
#include <pcl/ImageWindow.h>
#include <pcl/StringList.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <map>
#include <set>

namespace pcl
{

const char* const kScriptToolsPromptRead =
   "- list_scripts {query}: the PixInsight scripts PI Copilot can start (installed by PixInsight or its update system, "
   "signed by a certified developer): id, Script menu path, developer. With query, the matching scripts and, for "
   "matching scripts PI Copilot may NOT run, the reason.\n"
   "- describe_script {script_id}: what a script is and how to run it, read from its source text: menu path, version, "
   "description, whether it runs on a view and/or globally, the parameters it reads (name, type, default) and whether "
   "it opens a dialog. Call it before run_script.\n";

const char* const kScriptToolsPromptAct =
   "- run_script {script_id, arguments, context, view_id, reason}: asks PixInsight to run an installed script from "
   "list_scripts on the view this message is about (context \"view\") or globally (context \"global\"). arguments "
   "are only the parameters describe_script lists, with their types. The user is asked every time, in every mode, and "
   "sees the script, its developer, the target and the arguments. You never write or send code: only an id and "
   "arguments. If describe_script says the script opens its dialog, tell the user that it will, and that they work "
   "in it themselves. A script's changes to an image are in its History when the script records them. Never use "
   "run_script to get around a process the safety policy refused.\n";

namespace
{

using clock = std::chrono::steady_clock;

const char* const kOkMarkUtf8  = "\xE2\x96\xB6 ";   // "▶ "
const char* const kErrMarkUtf8 = "\xE2\x9C\x96 ";   // "✖ "
const char* const kArrowUtf8   = " \xE2\x86\x92 ";  // " → "

constexpr size_t kMaxConsoleChars = 8000;   // the tail of what the script wrote

String S16( const std::string& s )
{
   return FromU8( s );
}

String OkLine( const String& what, clock::time_point t0 )
{
   const double s = std::chrono::duration<double>( clock::now() - t0 ).count();
   return S16( kOkMarkUtf8 ) + what + S16( kArrowUtf8 ) + String().Format( "ok (%.1f s)", s );
}

String EscapeHtml( const String& s )
{
   String out;
   for ( size_type i = 0; i < s.Length(); ++i )
   {
      const char16_type c = s[i];
      if ( c == '&' )
         out += "&amp;";
      else if ( c == '<' )
         out += "&lt;";
      else if ( c == '>' )
         out += "&gt;";
      else if ( c == '\n' )
         out += "<br/>";
      else
         out += c;
   }
   return out;
}

View ViewByFullId( const IsoString& fullId )
{
   if ( fullId.IsEmpty() )
      return View::Null();
   try
   {
      return View::ViewById( fullId );
   }
   catch ( ... )
   {
      return View::Null();
   }
}

String ScriptsRoot( const ToolContext& ctx, String& error )
{
   error.Clear();
   if ( !ctx.scriptsRootOverride.IsEmpty() )
      return ctx.scriptsRootOverride;   // self-test only (never set by the panel)
   return ProductionScriptsRoot( error );
}

std::string Lower( std::string s )
{
   for ( char& c : s )
      c = char( std::tolower( uint8( c ) ) );
   return s;
}

std::string DeveloperText( const ScriptEntry& e )
{
   return e.developerName.empty() || e.developerName == e.developerId ? e.developerId
                                                                       : e.developerName + " (" + e.developerId + ")";
}

// The catalog entry for `id`, re-verified now. false + error (naming why a
// refused script is refused, when the id names one).
bool ResolveScript( const ToolContext& ctx, const std::string& id, ScriptEntry& entry, String& root, String& error )
{
   root = ScriptsRoot( ctx, error );
   if ( root.IsEmpty() )
      return false;
   const ScriptCatalog c = BuildScriptCatalog( root, true );
   if ( !c.ok )
   {
      error = c.error;
      return false;
   }
   const ScriptEntry* e = FindScript( c, id );
   if ( e == nullptr )
   {
      std::string why;
      for ( const ScriptRefusal& r : c.refused )
      {
         const size_t slash = r.rel.rfind( '/' );
         const std::string stem = r.rel.substr( slash == std::string::npos ? 0 : slash + 1 );
         if ( Lower( stem ) == Lower( id + ".js" ) || Lower( r.rel ) == Lower( id ) )
         {
            why = " " + r.rel + " is installed, but PI Copilot may not run it: " + r.reason + ".";
            break;
         }
      }
      error = FromU8( "no script with id '" + id + "' that PI Copilot may run." + why
                      + " Call list_scripts (with a query) for the ids." );
      return false;
   }
   ScriptEntry fresh;
   String reason;
   if ( !CheckScriptEligibility( e->PathString(), root, fresh, reason ) )
   {
      error = "script '" + S16( id ) + "' (" + S16( e->rel ) + ") cannot be run now: " + reason;
      return false;
   }
   entry = fresh;
   entry.id = e->id;   // the catalog id (disambiguated when two scripts share a signature id)
   return true;
}

// console.endLog() text without the core's banner and per-line timestamps.
bool IsDigit( char16_type c )
{
   return c >= '0' && c <= '9';
}

bool HasLogTimestamp( const String& line )
{
   if ( line.Length() < 22 || line[0] != '[' || line[20] != ']' || line[21] != ' ' )
      return false;
   const char* const shape = "dddd-dd-dd dd:dd:dd";
   for ( int i = 0; i < 19; ++i )
      if ( shape[i] == 'd' ? !IsDigit( line[1+i] ) : line[1+i] != char16_type( shape[i] ) )
         return false;
   return true;
}

bool IsStarRule( const String& line )
{
   if ( line.Length() < 10 )
      return false;
   for ( size_type i = 0; i < line.Length(); ++i )
      if ( line[i] != '*' )
         return false;
   return true;
}

String ConsoleText( const String& log )
{
   StringList lines;
   log.Break( lines, char16_type( '\n' ) );
   for ( String& line : lines )
      if ( HasLogTimestamp( line ) )
         line.DeleteLeft( 22 );
   size_type first = 0;
   if ( lines.Length() >= 3 && IsStarRule( lines[0] ) && lines[1].StartsWith( "PixInsight Core " ) && IsStarRule( lines[2] ) )
      first = 3;
   String out;
   for ( size_type i = first; i < lines.Length(); ++i )
   {
      out += lines[i];
      if ( i+1 < lines.Length() )
         out += '\n';
   }
   return out;
}

// Console capture around the core's run, through the module's own fixed
// script (PJSR console.beginLog/endLog; no PCL equivalent).
bool BeginConsoleLog( String& error )
{
   try
   {
      EvaluateAsciiJson( "console.beginLog(); return pcAscii( JSON.stringify( true ) );" );
      return true;
   }
   catch ( const pcl::Exception& x )
   {
      error = x.Message();
   }
   catch ( const std::exception& x )
   {
      error = String( x.what() );
   }
   return false;
}

String EndConsoleLog( String& error )
{
   try
   {
      const nlohmann::json j = EvaluateAsciiJson(
         "var b = console.endLog(); return pcAscii( JSON.stringify( b ? String( b.toBase64() ) : \"\" ) );" );
      const ByteArray raw = IsoString( j.get<std::string>().c_str() ).FromBase64();
      return ConsoleText( FromU8( std::string( reinterpret_cast<const char*>( raw.Begin() ), raw.Length() ) ) );
   }
   catch ( const pcl::Exception& x )
   {
      error = x.Message();
   }
   catch ( const std::exception& x )
   {
      error = String( x.what() );
   }
   return String();
}

nlohmann::json Identity( const ScriptEntry& e )
{
   return { { "id", e.id }, { "menu", e.menu }, { "file", e.rel }, { "developer", DeveloperText( e ) },
            { "signature", "valid (verified by PixInsight; signed " + e.signedAt + ")" } };
}

ToolOutcome ListScripts( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string query = Lower( StringField( in, "query" ) );
   const String what = "list_scripts" + (query.empty() ? String() : " \"" + S16( query ) + "\"");
   String error;
   const String root = ScriptsRoot( ctx, error );
   if ( root.IsEmpty() )
      return Fail( what, error );
   const ScriptCatalog c = BuildScriptCatalog( root, true );
   if ( !c.ok )
      return Fail( what, c.error );
   nlohmann::json scripts = nlohmann::json::array();
   for ( const ScriptEntry& e : c.eligible )
   {
      if ( !query.empty() )
      {
         const std::string hay = Lower( e.id + " " + e.menu + " " + e.rel + " " + e.developerId + " " + e.developerName );
         if ( hay.find( query ) == std::string::npos )
            continue;
         scripts.push_back( { { "id", e.id }, { "menu", e.menu }, { "developer", DeveloperText( e ) }, { "file", e.rel } } );
      }
      else
         scripts.push_back( { { "id", e.id }, { "menu", e.menu }, { "developer", e.developerId } } );
   }
   nlohmann::json r = { { "scripts", scripts }, { "count", scripts.size() } };
   if ( query.empty() )
   {
      std::map<std::string, int> byReason;
      for ( const ScriptRefusal& x : c.refused )
      {
         std::string k = x.reason;
         if ( k.compare( 0, 15, "it is signed by" ) == 0 )
            k = "signed by a developer who is not certified";
         else if ( k.compare( 0, 10, "not signed" ) == 0 )
            k = "not signed";
         ++byReason[k];
      }
      r["notRunnable"] = { { "count", c.refused.size() }, { "byReason", byReason },
                           { "note", "Installed scripts PI Copilot may not start; list_scripts with a query names them." } };
   }
   else
   {
      nlohmann::json no = nlohmann::json::array();
      for ( const ScriptRefusal& x : c.refused )
         if ( Lower( x.rel ).find( query ) != std::string::npos )
            no.push_back( { { "file", x.rel }, { "whyNot", x.reason } } );
      r["notRunnable"] = no;
   }
   r["note"] = "Call describe_script before run_script. Only these ids can be run; PI Copilot never runs any other code.";
   ToolOutcome o;
   o.content.push_back( TextBlock( r.dump() ) );
   o.logLine = OkLine( what, t0 ) + String().Format( " (%u scripts)", unsigned( scripts.size() ) );
   return o;
}

ToolOutcome DescribeScript( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string id = StringField( in, "script_id" );
   const String what = "describe_script " + S16( id );
   if ( id.empty() )
      return Fail( "describe_script", "describe_script needs {\"script_id\": <id from list_scripts>}" );
   ScriptEntry e;
   String root, error;
   if ( !ResolveScript( ctx, id, e, root, error ) )
      return Fail( what, error );
   const ScriptDescription d = DescribeScriptSource( e.PathString(), root, PixInsightIncludeDirectory() );
   if ( !d.ok )
      return Fail( what, "the script's source could not be read: " + S16( d.error ) );
   nlohmann::json r = Identity( e );
   r["description"] = ScriptDescriptionJson( d );
   r["howToRun"] = "run_script {script_id, arguments: {name: value} (only the parameters above, with their types), "
                   "context: \"view\" or \"global\"}. The user is asked before it runs.";
   ToolOutcome o;
   o.content.push_back( TextBlock( r.dump() ) );
   o.logLine = OkLine( what, t0 );
   return o;
}

// The view a run on a view targets: view_id (the turn's view or one inspected
// in this turn) or the turn's view.
View RunTarget( const nlohmann::json& in, const ToolContext& ctx, String& error )
{
   const std::string viewId = StringField( in, "view_id" );
   if ( !viewId.empty() )
   {
      View v = ViewByFullId( IsoString( viewId.c_str() ) );
      if ( v.IsNull() )
      {
         error = "no view with id '" + S16( viewId ) + "'";
         return View::Null();
      }
      const IsoString full = v.FullId();
      const bool inspected = ctx.inspectedViews != nullptr && ctx.inspectedViews->count( std::string( full.c_str() ) ) > 0;
      if ( full != ctx.turnViewId && !inspected )
      {
         error = "view '" + String( full ) + "' is not the view this message is about and has not been inspected in "
                 "this turn: call get_view_context with view_id '" + String( full ) + "' first";
         return View::Null();
      }
      return v;
   }
   if ( ctx.turnViewId.IsEmpty() )
   {
      error = "no active image when the user sent this message: pass context \"global\", or ask the user to select an image";
      return View::Null();
   }
   View v = ViewByFullId( ctx.turnViewId );
   if ( v.IsNull() )
      error = "the image this message is about (" + String( ctx.turnViewId ) + ") is no longer open";
   return v;
}

// Restores the console's abort state the run found (a script that opens a
// file can leave it enabled; measured for model scripts in inc 5, kept as a
// guard: the busy gates read it).
struct AbortRestore
{
   bool wasEnabled = true;
   AbortRestore() { try { wasEnabled = Console().AbortEnabled(); } catch ( ... ) {} }
   ~AbortRestore() { try { if ( !wasEnabled && Console().AbortEnabled() ) Console().DisableAbort(); } catch ( ... ) {} }
};

ToolOutcome RunScript( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string id = StringField( in, "script_id" );
   String what = "run_script " + S16( id );
   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "run_script is not available in Advisor mode (read-only); tell the user which script to run "
                         "(Script menu) and with which settings" );
   if ( id.empty() )
      return Fail( "run_script", "run_script needs {\"script_id\": <id from list_scripts>}" );
   if ( !ctx.confirmRunScript )
      return Fail( what, "internal error: no script confirmation callback" );

   ScriptEntry e;
   String root, error;
   if ( !ResolveScript( ctx, id, e, root, error ) )
      return Fail( what, error );
   const ScriptDescription d = DescribeScriptSource( e.PathString(), root, PixInsightIncludeDirectory() );
   if ( !d.ok )
      return Fail( what, "the script's source could not be read: " + S16( d.error ) );

   // Context: view or global.
   const std::string context = StringField( in, "context" );
   if ( !context.empty() && context != "view" && context != "global" )
      return Fail( what, "context must be \"view\" or \"global\"" );
   if ( context == "view" && d.SupportsGlobal() && !d.SupportsView() )
      return Fail( what, "this script handles only global runs (it checks Parameters.isGlobalTarget, never "
                         "isViewTarget); use context \"global\"" );
   if ( context == "global" && d.SupportsView() && !d.SupportsGlobal() )
      return Fail( what, "this script handles only runs on a view (it checks Parameters.isViewTarget, never "
                         "isGlobalTarget); use context \"view\"" );
   bool onView = context == "view";
   View target;
   if ( context.empty() )
      onView = !(d.SupportsGlobal() && !d.SupportsView()) && (!ctx.turnViewId.IsEmpty() || !StringField( in, "view_id" ).empty());
   if ( onView )
   {
      target = RunTarget( in, ctx, error );
      if ( target.IsNull() )
         return Fail( what, error );
   }
   const IsoString targetId = onView ? target.FullId() : IsoString();
   what += onView ? " on " + String( targetId ) : String( " (global)" );

   StringKeyValueList args;
   nlohmann::json shown;
   {
      const nlohmann::json a = in.contains( "arguments" ) ? in["arguments"] : nlohmann::json();
      const String ae = ValidateScriptArguments( d, a, args, shown );
      if ( !ae.IsEmpty() )
         return Fail( what, ae );
   }
   if ( !shown.empty() )
      what += " " + S16( shown.dump() );
   if ( onView && IsBusy( target ) )
      return Fail( what, "view " + String( targetId ) + " is busy (locked by a running process); try again when it finishes" );

   const nlohmann::json desc = ScriptDescriptionJson( d );
   RunScriptConfirmation c;
   c.menu = S16( e.menu );
   c.scriptId = S16( e.id );
   c.path = e.PathString();
   c.developerName = S16( e.developerName );
   c.developerId = S16( e.developerId );
   c.signedAt = S16( e.signedAt );
   c.target = String( targetId );
   c.arguments = shown;
   c.reason = S16( StringField( in, "reason" ) );
   c.interaction = S16( desc["interaction"].value( "summary", std::string() ) );
   if ( !ctx.confirmRunScript( RunScriptConfirmHtml( c ) ) )
   {
      ToolOutcome o;
      o.isError = true;
      o.content.push_back( TextBlock( "The user declined to run script " + e.id + ". Nothing ran. Do not ask again with "
                                      "the same call; ask what they would like instead." ) );
      o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
      return o;
   }

   // The dialog pumped events: re-resolve the view, and verify the file again
   // right before PixInsight loads it (it may have changed while the user read).
   if ( onView )
   {
      target = ViewByFullId( targetId );
      if ( target.IsNull() )
         return Fail( what, "view " + String( targetId ) + " was closed while the confirmation was open; nothing ran" );
   }
   {
      ScriptEntry again;
      String reason;
      if ( !CheckScriptEligibility( e.PathString(), root, again, reason ) || again.scriptId != e.scriptId )
         return Fail( what, "the script changed while the confirmation was open and is no longer runnable ("
                            + (reason.IsEmpty() ? String( "its signature now names another script" ) : reason) + "); nothing ran" );
   }

   // Journey attribution (main views only): the steps the script adds are the Copilot's, never replayed.
   JourneyTracker* journey = ctx.journeys != nullptr ? ctx.journeys->tracker : nullptr;
   const bool mainView = onView && target.IsMainView();
   int activeBefore = -1;
   if ( journey != nullptr && mainView )
   {
      const HistorySnapshot before = ReadViewHistory( targetId, INT_MAX );
      if ( before.ok )
         activeBefore = before.ActiveCount();
   }
   const std::set<std::string> windowsBefore = OpenMainViewIds();

   String consoleError, runError;
   String console;
   const auto r0 = clock::now();
   {
      const CoreScriptGuard running;   // timer readers defer while the core runs it
      const AbortRestore abortRestore;
      const bool logging = BeginConsoleLog( consoleError );
      try
      {
         if ( onView )
         {
            // Scripts commonly act on the active window: make the target it.
            ImageWindow w = target.Window();
            if ( !w.IsNull() )
            {
               w.BringToFront();
               View v = target;
               w.SelectView( v );
            }
            Console().ExecuteScriptOn( target, e.PathString(), args );
         }
         else
            Console().ExecuteScriptGlobal( e.PathString(), args );
      }
      catch ( const pcl::Exception& x )
      {
         runError = x.Message();
      }
      catch ( const std::exception& x )
      {
         runError = String( x.what() );
      }
      catch ( ... )
      {
         runError = "unknown error";
      }
      if ( logging )
         console = EndConsoleLog( consoleError );
   }
   const double elapsedMs = std::chrono::duration<double, std::milli>( clock::now() - r0 ).count();

   std::vector<std::string> created;
   for ( const std::string& w : OpenMainViewIds() )
      if ( windowsBefore.count( w ) == 0 )
         created.push_back( w );

   nlohmann::json journeyNote;
   if ( journey != nullptr && mainView )
   {
      View still = ViewByFullId( targetId );
      if ( !still.IsNull() && activeBefore >= 0 )
      {
         const HistorySnapshot after = ReadViewHistory( targetId, activeBefore );
         if ( after.ok )
         {
            const std::string why = "made by the script " + e.menu + " (run_script " + e.id + "): a replay never re-runs a "
                                    "script; run it yourself, or ask PI Copilot to run it again";
            const std::string reason = "run_script " + e.id + (StringField( in, "reason" ).empty() ? std::string()
                                                                 : ": " + StringField( in, "reason" ));
            int n = 0;
            for ( const HistoryStep& h : after.steps )
               if ( h.combinedIndex >= activeBefore && h.combinedIndex < after.ActiveCount() )
               {
                  journey->NoteCopilotStep( targetId, h.processId, reason, {}, false, JourneyWallNow(), why );
                  ++n;
               }
            journeyNote = n;
         }
         else
            journeyNote = "the History could not be read after the run (" + U8( after.error ) + "); its steps are recorded "
                          "as the user's";
      }
      journey->NoteCopilotCreated( targetId, created, false, JourneyWallNow() );
   }

   // What the model gets: file names, never folders (the PixInsight install, your
   // image folders) -- every line through the same path scrub as FITS values.
   auto noFolders = []( const String& text )
   {
      StringList lines;
      text.Break( lines, char16_type( '\n' ) );
      String out;
      for ( size_type i = 0; i < lines.Length(); ++i )
         out += FromU8( FitsValueWithoutPaths( U8( lines[i] ) ) ) + (i+1 < lines.Length() ? "\n" : "");
      return out;
   };
   String tail = noFolders( console );
   bool consoleCut = false;
   if ( tail.Length() > kMaxConsoleChars )
   {
      tail = String().Format( "[... %u earlier characters omitted]\n", unsigned( tail.Length() - kMaxConsoleChars ) )
           + tail.Right( kMaxConsoleChars );
      consoleCut = true;
   }
   nlohmann::json r = {
      { "script", e.id }, { "menu", e.menu },
      { "context", onView ? "view" : "global" },
      { "argumentsPassed", shown },
      { "elapsedMs", std::lround( elapsedMs ) },
      { "console", U8( tail ) }, { "consoleTruncated", consoleCut },
      { "newWindows", created }
   };
   if ( onView )
      r["view"] = std::string( targetId.c_str() );
   if ( !consoleError.IsEmpty() )
      r["consoleError"] = U8( consoleError );
   if ( !journeyNote.is_null() )
      r["journeyStepsAttributed"] = journeyNote;
   ToolOutcome o;
   o.mutated = true;   // a script may change images (conservative: the turn-end note mentions History)
   // Console::ExecuteScript* reports load/authorization failures only; a script
   // that throws ends normally for the caller. The core writes that error to the
   // console as "*** Error [nnn]: <file>, line <n>: <message>" (measured), so a
   // captured line of that shape is the script's failure.
   if ( runError.IsEmpty() )
   {
      StringList lines;
      console.Break( lines, char16_type( '\n' ) );
      for ( const String& l : lines )
         if ( l.Trimmed().StartsWith( "*** Error" ) )
         {
            runError = "the script stopped with an error: " + l.Trimmed();
            break;
         }
   }
   if ( !runError.IsEmpty() )
   {
      r["result"] = "error";
      r["error"] = runError.StartsWith( "the script stopped" ) ? U8( noFolders( runError ) )
                                                               : "PixInsight did not run the script, or stopped it: " + U8( noFolders( runError ) );
      r["note"] = "Anything the script changed before the error stays changed (check the image's History).";
      o.isError = true;
      o.content.push_back( TextBlock( r.dump() ) );
      o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "error: " + runError;
      return o;
   }
   r["result"] = "ok";
   r["note"] = "PixInsight ran the script to its end. PI Copilot cannot tell whether it did what was intended: check "
               "the image (get_view_context) and the console output.";
   if ( onView )
   {
      View v = ViewByFullId( targetId );
      if ( !v.IsNull() && !IsBusy( v ) )
         try
         {
            r["newContext"] = CollapsedViewContext( BuildViewContext( v ) );
         }
         catch ( const pcl::Exception& x )
         {
            r["newContextError"] = U8( x.Message() );
         }
   }
   o.content.push_back( TextBlock( r.dump() ) );
   o.logLine = OkLine( what, t0 );
   return o;
}

} // namespace

String RunScriptConfirmHtml( const RunScriptConfirmation& c )
{
   String args;
   if ( c.arguments.is_object() && !c.arguments.empty() )
   {
      for ( auto it = c.arguments.begin(); it != c.arguments.end(); ++it )
         args += "<br/>&nbsp;&nbsp;" + EscapeHtml( DisplayText( FromU8( it.key() ), 64 ) ) + " = "
               + EscapeHtml( DisplayText( FromU8( it.value().is_string() ? it.value().get<std::string>() : it.value().dump() ), 200 ) );
   }
   else
      args = " none (the script uses its own settings)";
   return "<p><b>Run the script " + EscapeHtml( c.menu.IsEmpty() ? c.scriptId : c.menu ) + "?</b></p>"
          "<p>File: " + EscapeHtml( c.path )
        + "<br/>Signed by: " + EscapeHtml( c.developerName.IsEmpty() || c.developerName == c.developerId
                                            ? c.developerId : c.developerName + " (" + c.developerId + ")" )
        + ", a certified PixInsight developer; signature verified by PixInsight"
        + (c.signedAt.IsEmpty() ? String() : " (signed " + EscapeHtml( c.signedAt ) + ")")
        + "</p><p>Runs " + (c.target.IsEmpty() ? String( "globally (no image as target)" ) : "on <b>" + EscapeHtml( c.target ) + "</b>")
        + "<br/>Arguments:" + args + "</p>"
        + (c.reason.IsEmpty() ? String() : "<p>Why: " + EscapeHtml( DisplayText( c.reason, 300 ) ) + "</p>")
        + (c.interaction.IsEmpty() ? String() : "<p>What to expect: " + EscapeHtml( c.interaction ) + "</p>")
        + "<p>PixInsight runs the script with full access to PixInsight and your files. It may open its own windows "
          "or dialogs, and PI Copilot cannot stop it once it starts.</p>";
}

nlohmann::json ScriptToolDefinitions( AgentMode mode )
{
   auto tool = []( const char* name, const char* desc, nlohmann::json props, nlohmann::json required )
   {
      nlohmann::json schema = { { "type", "object" }, { "properties", props } };
      if ( !required.empty() )
         schema["required"] = required;
      return nlohmann::json( { { "name", name }, { "description", desc }, { "input_schema", schema } } );
   };
   nlohmann::json t = nlohmann::json::array();
   t.push_back( tool( "list_scripts",
                      "List the installed PixInsight scripts PI Copilot can start with run_script (installed by "
                      "PixInsight or its update system under the PixInsight script directory, signed by a certified "
                      "developer): id, Script menu path, developer. With query: only matching scripts, plus matching "
                      "installed scripts PI Copilot may not run and why. Read-only.",
                      { { "query", { { "type", "string" }, { "description", "Case-insensitive text to match in the id, menu path, file or developer." } } } },
                      nlohmann::json::array() ) );
   t.push_back( tool( "describe_script",
                      "Describe one script from list_scripts, read from its source text (never executed): menu path, "
                      "version, description, whether it runs on a view and/or globally, every parameter it reads "
                      "(name, type, default when the source makes it obvious; keys built at run time are listed as "
                      "unresolved), and whether it opens a dialog. Call it before run_script. Read-only.",
                      { { "script_id", { { "type", "string" }, { "description", "Script id from list_scripts." } } } },
                      nlohmann::json::array( { "script_id" } ) ) );
   if ( mode != AgentMode::Advisor )
      t.push_back( tool( "run_script",
                         "Ask PixInsight to run an installed script from list_scripts. You pass only its id and "
                         "arguments, never code or a path. The user always confirms first (every mode). arguments: only "
                         "parameters describe_script lists, as JSON values of their type (boolean, number, string).",
                         { { "script_id", { { "type", "string" }, { "description", "Script id from list_scripts." } } },
                           { "arguments", { { "type", "object" }, { "description", "{parameterName: value} for parameters describe_script lists." } } },
                           { "context", { { "type", "string" }, { "enum", { "view", "global" } },
                                          { "description", "Run on a view (default when the script supports it and an image is active) or globally." } } },
                           { "view_id", { { "type", "string" }, { "description", "Target view; default: the view this message is about. Any other view must first be inspected with get_view_context." } } },
                           { "reason", { { "type", "string" }, { "description", "One short sentence: why (shown to the user, recorded in the image journey)." } } } },
                         nlohmann::json::array( { "script_id" } ) ) );
   return t;
}

bool IsScriptTool( const std::string& name )
{
   return name == "list_scripts" || name == "describe_script" || name == "run_script";
}

void ScriptToolResultForModel( ToolOutcome& o )
{
   for ( nlohmann::json& b : o.content )
   {
      if ( !b.is_object() || b.value( "type", std::string() ) != "text" || !b.contains( "text" ) || !b["text"].is_string() )
         continue;
      const std::string t = b["text"].get<std::string>();
      try
      {
         const nlohmann::json j = nlohmann::json::parse( t );
         b["text"] = JsonWithoutAbsolutePaths( j ).dump();
      }
      catch ( ... )
      {
         b["text"] = TextWithoutAbsolutePaths( t );
      }
   }
}

ToolOutcome ExecuteScriptTool( const ToolCall& call, const ToolContext& ctx )
{
   const clock::time_point t0 = clock::now();
   const nlohmann::json in = call.input.is_object() ? call.input : nlohmann::json::object();
   ToolOutcome o;
   try
   {
      if ( call.name == "list_scripts" )
         o = ListScripts( in, ctx, t0 );
      else if ( call.name == "describe_script" )
         o = DescribeScript( in, ctx, t0 );
      else if ( call.name == "run_script" )
         o = RunScript( in, ctx, t0 );
      else
         o = Fail( S16( call.name ), "not a script tool" );
   }
   catch ( const pcl::Exception& x )
   {
      o = Fail( S16( call.name ), S16( call.name ) + " failed: " + x.Message() );
   }
   catch ( const std::exception& x )
   {
      o = Fail( S16( call.name ), S16( call.name ) + " failed: " + String( x.what() ) );
   }
   // The one exit to the model: no absolute path (the PixInsight install and the
   // user's folders may be under /home/<user>) in anything the script tools say.
   ScriptToolResultForModel( o );
   return o;
}

} // namespace pcl
