// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "HistoryStepTool.h"
#include "AnthropicClient.h"   // JpegImageBlock
#include "EvalGuard.h"
#include "HistoryReader.h"
#include "JourneyTools.h"     // ModelTextWithoutDirectories
#include "PjsrRunner.h"
#include "ToolHelpers.h"       // Fail, IsBusy, StringField, TextBlock
#include "Utf8.h"
#include "ViewContext.h"
#include "ViewPreview.h"

#include <pcl/Exception.h>
#include <pcl/View.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace pcl
{

namespace
{

using hclock = std::chrono::steady_clock;

// The int64 guard of Task 10 R1: an unsigned count above this is refused, never
// wrapped; every other count is compared with the depth as int64 and narrowed
// only after it passed (4294967297 must never become 1).
constexpr int64 kMaxCount = std::numeric_limits<int64>::max();

String Line( bool ok, const String& what, const String& tail )
{
   return String::UTF8ToUTF16( ok ? "\xE2\x96\xB6 " : "\xE2\x9C\x96 " ) + what
          + String::UTF8ToUTF16( " \xE2\x86\x92 " ) + tail;
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

View ByFullId( const IsoString& fullId )
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

// The count as a validated int64 in [1, kMaxCount]; "" or the error. Absent / null = 1.
String ParseCount( const nlohmann::json& in, int64& count )
{
   count = 1;
   if ( !in.contains( "count" ) || in["count"].is_null() )
      return String();
   const nlohmann::json& c = in["count"];
   const String bad = "count must be a whole number of steps, at least 1 (e.g. 2); nothing was changed";
   if ( !c.is_number_integer() )
      return bad;
   if ( c.is_number_unsigned() )
   {
      const uint64_t u = c.get<uint64_t>();
      if ( u > uint64_t( kMaxCount ) )
         return "count " + String( IsoString( std::to_string( u ).c_str() ) ) + " is far more steps than any History holds; "
                "pass the number of steps to step (check get_journey or ask the user); nothing was changed";
      count = int64( u );
   }
   else
   {
      count = c.get<int64>();
   }
   if ( count < 1 )
      return bad;
   return String();
}

// Self-test seams (HistoryStepTool.h): 0 / empty in production.
int g_indexOffsetForSelfTest = 0;
std::function<void( HistorySnapshot& )> g_rereadHookForSelfTest;

// A short, path-free hint that tells same-process steps apart: PixelMath's expression, else the first
// two scalar parameters ("k=v"), cut to 60 characters. Paths are reduced to file names (GC privacy).
std::string StepHint( const HistoryStep& h )
{
   auto scalar = []( const nlohmann::json& v ) -> std::string {
      if ( v.is_string() ) return v.get<std::string>();
      if ( v.is_boolean() ) return v.get<bool>() ? "true" : "false";
      if ( v.is_number() ) return v.dump();
      return std::string();
   };
   std::string hint;
   if ( h.parameters.is_object() && h.parameters.contains( "expression" ) && h.parameters["expression"].is_string() )
      hint = h.parameters["expression"].get<std::string>();
   else if ( h.parameters.is_object() )
   {
      int n = 0;
      for ( auto it = h.parameters.begin(); it != h.parameters.end() && n < 2; ++it )
      {
         const std::string v = scalar( it.value() );
         if ( v.empty() )
            continue;
         hint += (n++ > 0 ? ", " : "") + it.key() + "=" + v;
      }
   }
   String t = ModelTextWithoutDirectories( FromU8( hint ) );
   for ( size_type i = 0; i < t.Length(); ++i )
      if ( t[i] == '\n' || t[i] == '\r' || t[i] == '\t' )
         t[i] = ' ';
   if ( t.Length() > 60 )
      t = t.Left( 57 ) + "...";
   return U8( t );
}

// "HH:MM:SS UTC" of a step's <time start> ("" when absent).
std::string StepTime( const HistoryStep& h )
{
   const size_t tpos = h.started.find( 'T' );
   return (tpos == std::string::npos || h.started.size() < tpos + 9) ? std::string() : h.started.substr( tpos + 1, 8 ) + " UTC";
}

// The History entries a step would pass, one per line: "#<step> <process>, <time>: <hint>".
String StepsText( const std::vector<HistoryStep>& steps )
{
   String s;
   for ( const HistoryStep& h : steps )
   {
      std::string line = "#" + std::to_string( h.combinedIndex + 1 ) + " " + h.processId;
      const std::string t = StepTime( h ), hint = StepHint( h );
      if ( !t.empty() )
         line += ", " + t;
      if ( !hint.empty() )
         line += ": " + hint;
      s += FromU8( line ) + "\n";
   }
   return s;
}

// Assigns View.historyIndex (the only history-navigation API: PCL has none) and reads it back.
nlohmann::json SetHistoryIndex( const IsoString& fullId, int index )
{
   return EvaluateAsciiJson(
      "var v = null; try { v = View.viewById( " + ScriptLiteral( String( fullId ) ) + " ); } catch ( e ) { v = null; }"
      " if ( v == null || v.isNull ) return pcAscii( JSON.stringify( { error: \"no view\" } ) );"
      " v.historyIndex = " + std::to_string( index ) + ";"
      " return pcAscii( JSON.stringify( { historyIndex: v.historyIndex, length: v.processing.length } ) );" );
}

} // namespace

nlohmann::json HistoryStepToolDefinition()
{
   nlohmann::json props = nlohmann::json::object();
   props["direction"] = { { "type", "string" }, { "enum", nlohmann::json::array( { "undo", "redo" } ) },
                          { "description", "\"undo\" steps back through the view's History, \"redo\" steps forward again." } };
   props["count"] = { { "type", "integer" }, { "minimum", 1 },
                      { "description", "How many History steps (default 1). More than the view can undo/redo is refused." } };
   props["view_id"] = { { "type", "string" },
                        { "description", "Target view id; default: the view this message is about. Any other view must first be "
                                         "inspected with get_view_context in the same turn. A preview id steps that preview's own "
                                         "History, never the main image's." } };
   nlohmann::json t = nlohmann::json::object();
   t["name"] = "history_step";
   t["description"] = "Undo or redo steps in a view's History, exactly like Edit > Undo / Redo: the image returns to "
                      "that earlier (or later) state. Undone steps stay redoable until a new process runs on the view. "
                      "Only steps made since the image was opened can be undone. Returns which History entries were "
                      "stepped (process ids), the new position, the new statistics and a fresh preview.";
   t["input_schema"] = { { "type", "object" }, { "properties", props },
                         { "required", nlohmann::json::array( { "direction" } ) } };
   return t;
}

String HistoryStepDialogHtml( bool undo, int count, const String& viewId, const String& stepsText )
{
   return "<p>" + String( undo ? "Undo " : "Redo " ) + String( count ) + (count == 1 ? " History step" : " History steps")
          + " on <b>" + EscapeHtml( viewId ) + "</b>?</p>"
          + "<p>" + EscapeHtml( stepsText ) + "</p>"
          + "<p>" + String( undo ? "Undone steps can be redone (Edit > Redo) until a new process runs on the image."
                                 : "Redone steps can be undone again (Edit > Undo)." ) + "</p>";
}

ToolOutcome ExecuteHistoryStepTool( const nlohmann::json& in, const ToolContext& ctx )
{
   const hclock::time_point t0 = hclock::now();
   double dialogS = 0;   // time in the Guided dialog, reported apart from the run (review minor 5)
   const std::string direction = StringField( in, "direction" );
   const bool undo = direction == "undo";
   String what = "history_step " + FromU8( direction );

   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "history_step is not available in Advisor mode (read-only); tell the user to use Edit > Undo / "
                         "Redo or the History Explorer" );
   if ( direction != "undo" && direction != "redo" )
      return Fail( "history_step", "history_step needs direction \"undo\" or \"redo\"; nothing was changed" );
   int64 count = 1;
   {
      const String e = ParseCount( in, count );
      if ( !e.IsEmpty() )
         return Fail( what, e );
   }
   what += String().Format( " %lld", static_cast<long long>( count ) );

   // The target: view_id, else the turn's view -- the same rule as apply_process.
   IsoString targetId;
   {
      View target;
      const std::string viewId = StringField( in, "view_id" );
      if ( !viewId.empty() )
      {
         target = ByFullId( IsoString( viewId.c_str() ) );
         if ( target.IsNull() )
            return Fail( what, "no view with id '" + FromU8( viewId ) + "'; nothing was changed" );
         const IsoString fullId = target.FullId();
         const bool inspected = ctx.inspectedViews != nullptr && ctx.inspectedViews->count( std::string( fullId.c_str() ) ) > 0;
         if ( fullId != ctx.turnViewId && !inspected )
            return Fail( what + " on " + String( fullId ),
                         "view '" + String( fullId ) + "' is not the view this message is about ("
                         + (ctx.turnViewId.IsEmpty() ? String( "none was active" ) : String( ctx.turnViewId ))
                         + ") and has not been inspected in this turn: call get_view_context with view_id '"
                         + String( fullId ) + "' first, then history_step again" );
      }
      else
      {
         if ( ctx.turnViewId.IsEmpty() )
            return Fail( what, "no active image when the user sent this message: ask the user which image to use, or pass view_id" );
         target = ByFullId( ctx.turnViewId );
         if ( target.IsNull() )
            return Fail( what, "the image this message is about (" + String( ctx.turnViewId ) + ") is no longer open; "
                               "ask the user which image to use" );
      }
      targetId = target.FullId();
   }
   what += " on " + String( targetId );
   const bool preview = targetId.Contains( "->" );

   // Refused, never waited for: a busy / locked view, or another script evaluation running.
   auto busyError = [&]( const View& v ) -> String
   {
      if ( IsBusy( v ) )
         return "view " + String( targetId ) + " is busy (locked, or a process is running on it); nothing was changed. "
                "Try again once PixInsight is idle";
      if ( IsPjsrScriptRunning() || EvaluateScriptDepth() > 0 )
         return "another script is running in PixInsight; nothing was changed. Try again once it has finished";
      return String();
   };
   {
      const String e = busyError( ByFullId( targetId ) );
      if ( !e.IsEmpty() )
         return Fail( what, e );
   }

   // Counts first (no steps read), then the entries the step passes.
   HistorySnapshot counts = ReadViewHistory( targetId, std::numeric_limits<int>::max()/2 );
   if ( !counts.ok )
      return Fail( what, counts.busy ? String( "another script is running in PixInsight; nothing was changed. Try again once it has finished" )
                                     : "could not read the History of " + String( targetId ) + ": " + counts.error + "; nothing was changed" );
   const int64 depth = undo ? int64( counts.historyIndex ) : int64( counts.length ) - int64( counts.historyIndex );
   if ( depth <= 0 )
      return Fail( what, undo ? "nothing to undo on " + String( targetId ) + ": PixInsight can only undo steps made since the image "
                                "was opened (its History has no step of this session before the current position); nothing was changed"
                              : "nothing to redo on " + String( targetId ) + ": no undone step follows the current position "
                                "(a new process after an undo discards the redo steps); nothing was changed" );
   if ( count > depth )
      return Fail( what, String().Format( "only %lld step%s can be %s on ", static_cast<long long>( depth ), depth == 1 ? "" : "s",
                                          undo ? "undone" : "redone" )
                         + String( targetId ) + String().Format( " (asked for %lld); nothing was changed", static_cast<long long>( count ) ) );
   const int n = int( count );   // narrowed only now: 1 <= count <= depth <= History length
   const int newIndex = undo ? counts.historyIndex - n : counts.historyIndex + n;
   const int lo = std::min( counts.historyIndex, newIndex );
   HistorySnapshot range = ReadViewHistory( targetId, counts.initialLength + lo );
   if ( !range.ok || range.historyIndex != counts.historyIndex || range.length != counts.length || int( range.steps.size() ) < n )
      return Fail( what, "could not read the History entries to " + String( direction.c_str() ) + " on " + String( targetId ) + ": "
                         + (range.busy ? String( "another script is running" ) : range.ok ? String( "the History changed while it was read" ) : range.error)
                         + "; nothing was changed" );
   range.steps.resize( size_t( n ) );
   const int readFrom = counts.initialLength + lo;
   // Undo passes the entries newest first (the order they are taken back).
   std::vector<HistoryStep> passed;
   if ( undo )
      for ( auto it = range.steps.rbegin(); it != range.steps.rend(); ++it )
         passed.push_back( *it );
   else
      passed = range.steps;

   if ( ctx.mode == AgentMode::Guided )
   {
      if ( !ctx.confirmHistory )
         return Fail( what, "internal error: no confirmation callback" );
      const hclock::time_point d0 = hclock::now();
      const bool approved = ctx.confirmHistory( HistoryStepDialogHtml( undo, n, String( targetId ), StepsText( passed ) ) );
      dialogS = std::chrono::duration<double>( hclock::now() - d0 ).count();
      if ( !approved )
      {
         ToolOutcome o;
         o.isError = true;
         o.content.push_back( TextBlock( "The user declined this history_step call (" + direction + " "
                                         + std::to_string( n ) + " on " + std::string( targetId.c_str() )
                                         + "). Nothing was changed. Do not repeat it; ask what they would like instead." ) );
         o.logLine = Line( false, what, "declined by user" );
         return o;
      }
      // The dialog pumped events: the image may be closed, busy, or stepped by the user meanwhile.
      const View again = ByFullId( targetId );
      if ( again.IsNull() )
         return Fail( what, "view " + String( targetId ) + " is no longer open (closed while the confirmation dialog was up); nothing was changed" );
      const String e = busyError( again );
      if ( !e.IsEmpty() )
         return Fail( what, e );
      // Same position AND the same entries (review minor 2): an undo + a different new step while the
      // dialog was up keeps the shape but changes the identity (process, start time, parameters digest).
      HistorySnapshot now = ReadViewHistory( targetId, readFrom );
      if ( g_rereadHookForSelfTest )
         g_rereadHookForSelfTest( now );
      bool same = now.ok && now.historyIndex == counts.historyIndex && now.length == counts.length
               && int( now.steps.size() ) >= n;
      for ( int i = 0; same && i < n; ++i )
         same = now.steps[size_t( i )].identity == range.steps[size_t( i )].identity;
      if ( !same )
         return Fail( what, "the History of " + String( targetId ) + " changed while the confirmation dialog was up; nothing was "
                            "changed. Check the image again before stepping" );
   }

   nlohmann::json set;
   try
   {
      set = SetHistoryIndex( targetId, newIndex + g_indexOffsetForSelfTest );
   }
   catch ( const pcl::Exception& x )
   {
      return Fail( what, "PixInsight refused to move the History of " + String( targetId ) + ": " + x.Message() );
   }
   catch ( const std::exception& x )
   {
      return Fail( what, "PixInsight refused to move the History of " + String( targetId ) + ": " + String( x.what() ) );
   }
   if ( set.contains( "error" ) || set.value( "historyIndex", -1 ) != newIndex || set.value( "length", -1 ) != counts.length )
   {
      // Loud: an assignment PixInsight ignores is silent (measured), so it is checked here, never assumed.
      ToolOutcome o = Fail( what, "PixInsight did not move the History of " + String( targetId )
                                  + String().Format( " to position %d (it reports position %d of %d)", newIndex,
                                                     set.value( "historyIndex", -1 ), set.value( "length", -1 ) )
                                  + ". Tell the user what you tried; they can use Edit > Undo / Redo themselves" );
      o.mutated = set.value( "historyIndex", counts.historyIndex ) != counts.historyIndex;
      return o;
   }

   nlohmann::json stepped = nlohmann::json::array();
   for ( const HistoryStep& h : passed )
   {
      nlohmann::json e = { { "step", h.combinedIndex + 1 }, { "process", h.processId } };
      const std::string hint = StepHint( h );
      if ( !hint.empty() )
         e["hint"] = hint;
      stepped.push_back( e );
   }
   nlohmann::json summary = {
      { "result", "ok" },
      { "view", std::string( targetId.c_str() ) },
      { "direction", direction },
      { "count", n },
      { "steps", stepped },   // the History entries stepped, in the order they were taken back / reapplied
      { "historyIndex", newIndex },
      { "undoAvailable", newIndex },
      { "redoAvailable", counts.length - newIndex }
   };
   if ( preview )
      summary["scope"] = "preview";   // the preview's own History; the main image is unchanged
   summary["note"] = undo ? "Undone steps stay redoable until a new process runs on this view; the next process discards them."
                          : "The redone steps are active again.";
   const View v = ByFullId( targetId );
   if ( !v.IsNull() )
   {
      try
      {
         summary["newContext"] = CollapsedViewContext( BuildViewContext( v ) );
      }
      catch ( const pcl::Exception& x )
      {
         summary["newContextError"] = U8( x.Message() );
      }
   }
   ViewPreviewResult p;
   if ( !v.IsNull() )
      p = RenderViewPreview( v );
   else
      p.error = "the view closed right after the step";
   if ( !p.ok )
      summary["previewError"] = U8( p.error );

   ToolOutcome o;
   o.mutated = true;
   o.content.push_back( TextBlock( summary.dump() ) );
   if ( p.ok )
      o.content.push_back( JpegImageBlock( p.base64 ) );
   const double s = std::chrono::duration<double>( hclock::now() - t0 ).count() - dialogS;
   o.logLine = Line( true, what, ctx.mode == AgentMode::Guided
                                    ? String().Format( "ok (%.1f s, plus %.1f s in the confirmation dialog)", s, dialogS )
                                    : String().Format( "ok (%.1f s)", s ) );
   return o;
}

} // namespace pcl

namespace pcl
{

void SetHistoryStepIndexOffsetForSelfTest( int offset )
{
   g_indexOffsetForSelfTest = offset;
}

void SetHistoryStepRereadHookForSelfTest( std::function<void( HistorySnapshot& )> hook )
{
   g_rereadHookForSelfTest = std::move( hook );
}

} // namespace pcl
