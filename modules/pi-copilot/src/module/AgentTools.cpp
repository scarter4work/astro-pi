// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentTools.h"
#include "AnthropicClient.h"   // JpegImageBlock
#include "PjsrRunner.h"
#include "ProcessApply.h"
#include "ProcessCatalog.h"
#include "ProcessSafety.h"
#include "Utf8.h"
#include "ViewContext.h"
#include "ViewPreview.h"

#include <pcl/Exception.h>
#include <pcl/ImageWindow.h>
#include <pcl/Thread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <vector>

namespace pcl
{

namespace
{

using clock = std::chrono::steady_clock;

const char* const kOkMarkUtf8  = "\xE2\x96\xB6 ";   // "▶ "
const char* const kErrMarkUtf8 = "\xE2\x9C\x96 ";   // "✖ "
const char* const kArrowUtf8   = " \xE2\x86\x92 ";  // " → "

String S16( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

String Shorten( const String& s, size_type n )
{
   return (s.Length() <= n || n < 4) ? s : s.Left( n - 3 ) + "...";
}

nlohmann::json TextBlock( const std::string& utf8 )
{
   return { { "type", "text" }, { "text", utf8 } };
}

std::string StringField( const nlohmann::json& in, const char* key )
{
   return (in.contains( key ) && in[key].is_string()) ? in[key].get<std::string>() : std::string();
}

bool BoolField( const nlohmann::json& in, const char* key )
{
   return in.contains( key ) && in[key].is_boolean() && in[key].get<bool>();
}

String OkLine( const String& what, clock::time_point t0 )
{
   const double s = std::chrono::duration<double>( clock::now() - t0 ).count();
   return S16( kOkMarkUtf8 ) + what + S16( kArrowUtf8 ) + String().Format( "ok (%.1f s)", s );
}

ToolOutcome Fail( const String& what, const String& error )
{
   ToolOutcome o;
   o.isError = true;
   o.content.push_back( TextBlock( U8( error ) ) );
   o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "error: " + Shorten( error, 200 );
   return o;
}

bool IsBusy( const View& v )
{
   bool busy = true;
   try
   {
      busy = !v.CanRead() || !v.CanWrite();
   }
   catch ( ... )
   {
   }
   return busy;
}

// View::ViewById() on a full id ("Image01", "Image01->Preview01"); null when
// no such view is open. Never throws.
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

String TurnViewGone( const IsoString& turnViewId )
{
   return "the image this message is about (" + String( turnViewId ) + ") is no longer open; "
          "ask the user which image to use";
}

// The turn's own view, re-resolved by id now. error is set when there is none.
View TurnView( const ToolContext& ctx, String& error )
{
   if ( ctx.turnViewId.IsEmpty() )
   {
      error = "no active image when the user sent this message: ask the user to select an image and send again";
      return View::Null();
   }
   View v = ViewByFullId( ctx.turnViewId );
   if ( v.IsNull() )
      error = TurnViewGone( ctx.turnViewId );
   return v;
}

bool WasInspected( const ToolContext& ctx, const IsoString& fullId )
{
   return ctx.inspectedViews != nullptr && ctx.inspectedViews->count( std::string( fullId.c_str() ) ) > 0;
}

// THE deny/confirm gate, shared by every tool that runs a process
// (apply_process, run_global_process): one policy, one wording, one dialog.
//   deny                        -> false, `out` = a precise error (never a modal)
//   Guided, or a confirm rule   -> ONE confirm dialog (a confirm rule asks in
//                                  EVERY mode, Copilot included); declined or
//                                  no callback -> false, `out` set
//   otherwise                   -> true (run it)
// `target` is the dialog's target text; `callText` names the call in the
// declined message (e.g. "PixelMath on Image01"); `nothingDone` ends it.
// Advisor never reaches this: its callers refuse Advisor first.
bool PassSafetyGate( const char* tool, const std::string& pid, const nlohmann::json& params,
                     const nlohmann::json& tables, const ToolContext& ctx, const String& what,
                     const String& target, const std::string& callText, const char* nothingDone,
                     ToolOutcome& out )
{
   const SafetyVerdict safety = CheckProcessSafety( IsoString( pid.c_str() ), params, tables );
   if ( safety.kind == SafetyVerdict::Deny )
   {
      out = Fail( what, S16( pid ) + " is not allowed from PI Copilot: " + safety.reason
                        + ". Give the user the settings so they can run it themselves." );
      return false;
   }
   if ( ctx.mode != AgentMode::Guided && safety.kind != SafetyVerdict::Confirm )
      return true;
   if ( !ctx.confirm )
   {
      out = Fail( what, "internal error: no confirmation callback" );
      return false;
   }
   String changes = DescribeParameterChanges( params, tables, PICopilotConfirmChangesChars );
   if ( safety.kind == SafetyVerdict::Confirm )
      changes = "Why you are asked: " + safety.reason + ".\n\n" + changes;
   if ( ctx.confirm( S16( pid ), target, changes ) )
      return true;
   out = ToolOutcome();
   out.isError = true;
   out.content.push_back( TextBlock( std::string( "The user declined this " ) + tool + " call (" + callText + "). "
                                     + nothingDone + " Do not repeat it; ask what they would like instead." ) );
   out.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
   return false;
}

ToolOutcome ApplyProcessTool( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string pid = StringField( in, "process_id" );
   const nlohmann::json params = in.contains( "parameters" ) ? in["parameters"] : nlohmann::json::object();
   const nlohmann::json tables = in.contains( "table_parameters" ) ? in["table_parameters"] : nlohmann::json::object();
   nlohmann::json shown = params.is_object() ? params : nlohmann::json::object();
   if ( tables.is_object() )
      for ( auto it = tables.begin(); it != tables.end(); ++it )
         shown[it.key()] = it.value();
   String what = "apply_process " + S16( pid ) + " " + Shorten( S16( shown.dump() ), PICopilotToolLogParamChars );

   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "apply_process is not available in Advisor mode (read-only); give the user the settings instead" );
   if ( pid.empty() )
      return Fail( "apply_process", "apply_process needs process_id; call list_processes for valid ids" );
   if ( HasFileTables( IsoString( pid.c_str() ) ) )
      return Fail( what, S16( pid ) + " integrates files from disk, not an open image: use run_global_process with "
                         "the file list in table_parameters" );

   View target;
   const std::string viewId = StringField( in, "view_id" );
   if ( !viewId.empty() )
   {
      target = ViewByFullId( IsoString( viewId.c_str() ) );
      if ( target.IsNull() )
         return Fail( what, "no view with id '" + S16( viewId ) + "'" );
      const IsoString fullId = target.FullId();
      if ( fullId != ctx.turnViewId && !WasInspected( ctx, fullId ) )
         return Fail( what + " on " + String( fullId ),
                      "view '" + String( fullId ) + "' is not the view this message is about ("
                      + (ctx.turnViewId.IsEmpty() ? String( "none was active" ) : String( ctx.turnViewId ))
                      + ") and has not been inspected in this turn: call get_view_context with view_id '"
                      + String( fullId ) + "' first, then apply_process again" );
   }
   else
   {
      String error;
      target = TurnView( ctx, error );
      if ( target.IsNull() )
         return Fail( what, error + (ctx.turnViewId.IsEmpty() ? String( ", or pass view_id" ) : String()) );
   }
   const IsoString targetId = target.FullId();
   what += " on " + String( targetId );

   {
      ToolOutcome refused;
      if ( !PassSafetyGate( "apply_process", pid, params, tables, ctx, what, String( targetId ),
                            pid + " on " + std::string( targetId.c_str() ), "Nothing was changed.", refused ) )
         return refused;
   }
   // A dialog pumps events: the user may have closed (or replaced) the image
   // meanwhile. Never run on a stale handle (a no-op when nothing was asked).
   target = ViewByFullId( targetId );
   if ( target.IsNull() )
      return Fail( what, "view " + String( targetId ) + " is no longer open (closed while the confirmation "
                         "dialog was up); nothing was changed" );

   const ApplyProcessResult ar = ApplyProcess( IsoString( pid.c_str() ), params, tables, target );
   if ( !ar.ok )
      return Fail( what, ar.error );

   nlohmann::json summary = {
      { "result", "ok" },
      { "process", U8( ar.processId ) },
      { "view", U8( ar.viewId ) },
      { "parametersSet", ar.parametersSet },
      { "elapsedMs", std::lround( ar.elapsedMs ) },
      { "undo", "Recorded in the view's History; the user can undo it." }
   };
   try
   {
      summary["newContext"] = CollapsedViewContext( BuildViewContext( target ) );
   }
   catch ( const pcl::Exception& x )
   {
      summary["newContextError"] = U8( x.Message() );
   }
   const ViewPreviewResult p = RenderViewPreview( target );
   if ( !p.ok )
      summary["previewError"] = U8( p.error );

   ToolOutcome o;
   o.mutated = true;
   o.content.push_back( TextBlock( summary.dump() ) );
   if ( p.ok )
      o.content.push_back( JpegImageBlock( p.base64 ) );
   o.logLine = OkLine( what, t0 );
   return o;
}

// run_global_process: a process in the global context (e.g. ImageIntegration
// over files on disk). It opens NEW windows and never changes an open image.
ToolOutcome RunGlobalTool( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string pid = StringField( in, "process_id" );
   const nlohmann::json params = in.contains( "parameters" ) ? in["parameters"] : nlohmann::json::object();
   const nlohmann::json tables = in.contains( "table_parameters" ) ? in["table_parameters"] : nlohmann::json::object();
   nlohmann::json shown = params.is_object() ? params : nlohmann::json::object();
   if ( tables.is_object() )
      for ( auto it = tables.begin(); it != tables.end(); ++it )
         shown[it.key()] = it.value();
   const String what = "run_global_process " + S16( pid ) + " " + Shorten( S16( shown.dump() ), PICopilotToolLogParamChars );

   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "run_global_process is not available in Advisor mode (read-only); give the user the settings instead" );
   if ( pid.empty() )
      return Fail( "run_global_process", "run_global_process needs process_id; call list_processes for valid ids" );

   const String pre = PrecheckGlobalRun( IsoString( pid.c_str() ), params, tables );
   if ( !pre.IsEmpty() )
      return Fail( what, pre );

   {
      ToolOutcome refused;
      if ( !PassSafetyGate( "run_global_process", pid, params, tables, ctx, what,
                            "(global run: creates new images, changes no open image)", pid, "Nothing was run.", refused ) )
         return refused;
   }

   const GlobalRunResult g = RunGlobalProcess( IsoString( pid.c_str() ), params, tables );
   if ( !g.ok )
   {
      String e = g.error;
      std::vector<std::string> opened = g.createdWindows;
      opened.insert( opened.end(), g.otherNewWindows.begin(), g.otherNewWindows.end() );
      if ( !opened.empty() )
      {
         e += " (windows that opened before it stopped: ";
         for ( size_t i = 0; i < opened.size(); ++i )
            e += (i > 0 ? String( ", " ) : String()) + S16( opened[i] );
         e += ")";
      }
      return Fail( what, e );
   }

   // The main result: the integration image if the process names one it
   // opened, else the first new window. It is described (and listed) first.
   std::vector<std::string> order = g.createdWindows;
   const std::string named = g.outputIds.value( "integrationImageId", std::string() );
   auto hit = std::find( order.begin(), order.end(), named );
   if ( hit != order.end() )
      std::rotate( order.begin(), hit, hit + 1 );
   const std::string primary = order.empty() ? std::string() : order.front();

   nlohmann::json windows = nlohmann::json::array();
   for ( const std::string& id : order )
   {
      if ( windows.size() >= PICopilotMaxDescribedWindows )
         break;
      nlohmann::json w = { { "id", id } };
      try
      {
         const ImageWindow iw = ImageWindow::WindowById( IsoString( id.c_str() ) );
         if ( iw.IsNull() )
            w["contextError"] = "the window was closed before it could be described";
         else if ( IsBusy( iw.MainView() ) )
            w["contextError"] = "the view is busy (locked by a running process)";
         else
            w["context"] = CollapsedViewContext( BuildViewContext( iw.MainView() ) );
      }
      catch ( const pcl::Exception& x )
      {
         w["contextError"] = U8( x.Message() );
      }
      windows.push_back( w );
   }
   nlohmann::json summary = {
      { "result", "ok" },
      { "process", U8( g.processId ) },
      { "parametersSet", g.parametersSet },
      { "elapsedMs", std::lround( g.elapsedMs ) },
      { "outputIds", g.outputIds },
      { "createdWindows", windows },
      { "createdWindowCount", g.createdWindows.size() },
      { "note", primary.empty()
                   ? std::string( "The process completed but opened no new image window; no open image was modified." )
                   : "New image windows were created; no open image was modified. The preview shows " + primary + "." }
   };
   if ( g.createdWindows.size() > PICopilotMaxDescribedWindows )
      summary["windowsNotDescribed"] = g.createdWindows.size() - PICopilotMaxDescribedWindows;
   if ( !g.otherNewWindows.empty() )
   {
      summary["otherNewWindows"] = g.otherNewWindows;
      summary["otherNewWindowsNote"] = "opened during the run; may not be results";
   }

   ToolOutcome o;
   if ( !primary.empty() )
   {
      const ImageWindow iw = ImageWindow::WindowById( IsoString( primary.c_str() ) );
      if ( iw.IsNull() )
         summary["previewError"] = "the window " + primary + " was closed before the preview";
      else if ( IsBusy( iw.MainView() ) )
         summary["previewError"] = "the view " + primary + " is busy (locked by a running process)";
      else
      {
         const ViewPreviewResult p = RenderViewPreview( iw.MainView() );
         if ( p.ok )
         {
            o.content.push_back( TextBlock( summary.dump() ) );
            o.content.push_back( JpegImageBlock( p.base64 ) );
            o.logLine = OkLine( what, t0 );
            return o;
         }
         summary["previewError"] = U8( p.error );
      }
   }
   o.content.push_back( TextBlock( summary.dump() ) );
   o.logLine = OkLine( what, t0 );
   return o;
}

ToolOutcome RunPjsrTool( const nlohmann::json& in, const ToolContext& ctx, clock::time_point t0 )
{
   const std::string purpose = StringField( in, "purpose" );
   const String code = S16( StringField( in, "code" ) );
   const String what = "run_pjsr \"" + Shorten( S16( purpose ), 80 ) + "\"";
   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "run_pjsr is not available in Advisor mode (read-only); give the user the script instead" );
   if ( !ctx.runPjsr )
      return Fail( what, "run_pjsr is turned off. The user can allow scripts in PI Copilot's settings (the gear "
                         "button); until then use the processes, or give the user the script to run themselves." );
   if ( code.Trimmed().IsEmpty() )
      return Fail( what, "run_pjsr needs {\"code\": \"<JavaScript function body>\", \"purpose\": \"<one sentence>\"}" );
   if ( code.Length() > PICopilotMaxScriptChars )
      return Fail( what, String().Format( "the script has %u characters; the limit is %u. Split the work, or use "
                                          "processes", unsigned( code.Length() ), unsigned( PICopilotMaxScriptChars ) ) );
   if ( String( S16( purpose ) ).Trimmed().IsEmpty() )
      return Fail( what, "run_pjsr needs a one-sentence purpose; it is shown to the user in the approval dialog" );

   // Parse first: a syntax error never reaches the user's dialog. The engine
   // gives a SyntaxError no position (inc-5 Task 1), so none is invented.
   const PjsrCheck check = CheckPjsrSyntax( code );
   if ( !check.ok )
      return Fail( what, (check.line > 0 ? String().Format( "syntax error at line %d: ", check.line )
                                         : String( "syntax error (line unknown): " ))
                         + check.error + " (the script was not shown to the user and did not run; fix it and call "
                         "run_pjsr again)" );

   if ( !ctx.confirmScript )
      return Fail( what, "internal error: no script confirmation callback" );
   if ( !ctx.confirmScript( S16( purpose ), code, ctx.turnViewId ) )
   {
      ToolOutcome o;
      o.isError = true;
      o.content.push_back( TextBlock( "The user declined to run this script. Nothing was run. Do not send the same "
                                      "script again; ask what they would like instead." ) );
      o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
      return o;
   }

   const PjsrRun run = RunPjsr( code, ctx.turnViewId );
   ToolOutcome o;
   o.mutated = true;   // a script may have changed images (conservative: the turn-end note mentions History)
   if ( !run.ok )
   {
      const nlohmann::json e = {
         { "result", "error" }, { "error", U8( run.error ) },
         { "line", run.line > 0 ? nlohmann::json( run.line ) : nlohmann::json( "unknown" ) },
         { "console", U8( run.console ) }, { "consoleTruncated", run.consoleTruncated },
         { "note", "The script ran until the error: anything it changed before that point stays changed." }
      };
      o.isError = true;
      o.content.push_back( TextBlock( e.dump() ) );
      o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 )
                + (run.line > 0 ? String().Format( "error at line %d: ", run.line ) : String( "error (line unknown): " ))
                + Shorten( run.error, 160 );
      return o;
   }
   const nlohmann::json summary = {
      { "result", "ok" },
      { "value", run.value.IsEmpty() ? nlohmann::json() : nlohmann::json( U8( run.value ) ) },
      { "valueTruncated", run.valueTruncated },
      { "console", U8( run.console ) },
      { "consoleTruncated", run.consoleTruncated },
      { "elapsedMs", std::lround( run.elapsedMs ) },
      { "note", "A script's changes are undoable only if it used view.beginProcess()/endProcess() or ran process instances." }
   };
   o.content.push_back( TextBlock( summary.dump() ) );
   o.logLine = OkLine( what, t0 );
   return o;
}

} // namespace

AgentMode AgentModeFromIndex( int index )
{
   return index == 1 ? AgentMode::Advisor : index == 2 ? AgentMode::Guided : AgentMode::Copilot;
}

nlohmann::json ToolDefinitions( AgentMode mode, const ToolOptions& options )
{
   nlohmann::json tools = nlohmann::json::array();

   nlohmann::json list = nlohmann::json::object();
   list["name"] = "list_processes";
   list["description"] = "List the processes installed in this PixInsight: id, categories, and a one-line summary when known.";
   list["input_schema"] = { { "type", "object" }, { "properties", nlohmann::json::object() } };
   tools.push_back( list );

   nlohmann::json describe = nlohmann::json::object();
   describe["name"] = "describe_process";
   describe["description"] = "Describe one process's parameters: id, type, default, numeric range, enumeration "
                             "element ids, and table columns in row order. Call it before setting parameters "
                             "you have not used yet in this conversation.";
   describe["input_schema"] = { { "type", "object" },
                                { "properties", { { "id", { { "type", "string" }, { "description", "Process id, e.g. PixelMath" } } } } },
                                { "required", nlohmann::json::array( { "id" } ) } };
   tools.push_back( describe );

   nlohmann::json context = nlohmann::json::object();
   context["name"] = "get_view_context";
   context["description"] = "Fresh facts about a view (default: the view this message is about, i.e. the one active "
                            "when the user sent it): geometry, per-channel median/MAD/mean/min/max of the real data, "
                            "FITS keywords; with include_preview, also a new auto-stretched preview image. Inspect "
                            "another view with view_id before apply_process can target it.";
   context["input_schema"] = { { "type", "object" },
                               { "properties", { { "include_preview", { { "type", "boolean" },
                                                                        { "description", "Attach a fresh preview image (default false)." } } },
                                                 { "view_id", { { "type", "string" },
                                                                { "description", "View id to inspect; default: the view this message is about." } } } } } };
   tools.push_back( context );

   if ( mode != AgentMode::Advisor )
   {
      nlohmann::json props = nlohmann::json::object();
      props["process_id"] = { { "type", "string" }, { "description", "Process id, e.g. PixelMath" } };
      props["parameters"] = { { "type", "object" },
                              { "description", "{parameterId: value} for non-table parameters. Enumerations take the element id as a string." } };
      props["table_parameters"] = { { "type", "object" },
                                    { "description", "{tableParameterId: [[row values in column order], ...]}; replaces the whole table." } };
      props["view_id"] = { { "type", "string" }, { "description", "Target view id; default: the view this message is about. Any other view must first be "
                                                                 "inspected with get_view_context in the same turn." } };
      nlohmann::json apply = nlohmann::json::object();
      apply["name"] = "apply_process";
      apply["description"] = "Run a PixInsight process on the user's real image (recorded in the view's History, so "
                             "it can be undone). Starts from the process's DEFAULT settings and sets only the "
                             "parameters given. Returns ok with the new statistics and a fresh preview, or a precise error.";
      apply["input_schema"] = { { "type", "object" }, { "properties", props },
                                { "required", nlohmann::json::array( { "process_id" } ) } };
      tools.push_back( apply );

      nlohmann::json gprops = nlohmann::json::object();
      gprops["process_id"] = { { "type", "string" }, { "description", "Process id, e.g. ImageIntegration" } };
      gprops["parameters"] = props["parameters"];
      gprops["table_parameters"] = { { "type", "object" },
                                     { "description", "{tableParameterId: [[row values in column order], ...]}. "
                                                      "ImageIntegration: {\"images\": [[enabled, path, drizzlePath, "
                                                      "localNormalizationDataPath], ...]} with absolute paths." } };
      nlohmann::json global = nlohmann::json::object();
      global["name"] = "run_global_process";
      global["description"] = "Run a PixInsight process in the global context (not on a view), e.g. ImageIntegration "
                              "over image files on disk. It creates NEW image windows and never modifies an open "
                              "image. Starts from the process's DEFAULT settings and sets only the parameters given. "
                              "File paths must be absolute and must exist. Returns the created windows (ids and "
                              "statistics) and a preview of the main result, or a precise error.";
      global["input_schema"] = { { "type", "object" }, { "properties", gprops },
                                 { "required", nlohmann::json::array( { "process_id" } ) } };
      tools.push_back( global );
   }
   if ( mode != AgentMode::Advisor && options.runPjsr )
   {
      nlohmann::json sprops = nlohmann::json::object();
      sprops["code"] = { { "type", "string" },
                         { "description", "JavaScript (PJSR) function body. `return` a value to get it back (JSON); "
                                          "console.writeln output is captured; targetViewId holds the id of the view "
                                          "this message is about (may be empty)." } };
      sprops["purpose"] = { { "type", "string" }, { "description", "One sentence for the user saying what the script does." } };
      nlohmann::json script = nlohmann::json::object();
      script["name"] = "run_pjsr";
      script["description"] = "Run a short PixInsight JavaScript (PJSR) script, for jobs no process can do "
                              "(inspection-driven decisions, window/preview management, custom measurements). The "
                              "user sees the whole script and must approve it every time. Prefer apply_process. "
                              "To change pixels directly, wrap the change in "
                              "view.beginProcess(UndoFlag.PixelData) ... view.endProcess() so it can be undone "
                              "(constants are namespaced here: UndoFlag.PixelData, ImageOp.Mul). A "
                              "script cannot be interrupted: never write loops that might not end.";
      script["input_schema"] = { { "type", "object" }, { "properties", sprops },
                                 { "required", nlohmann::json::array( { "code", "purpose" } ) } };
      tools.push_back( script );
   }
   return tools;
}

nlohmann::json CollapsedViewContext( const nlohmann::json& full )
{
   nlohmann::json c = nlohmann::json::object();
   for ( const char* k : { "viewId", "fullId", "geometry", "channelStats" } )
      if ( full.contains( k ) )
         c[k] = full[k];
   return c;
}

nlohmann::json ToolResultBlock( const std::string& toolUseId, const ToolOutcome& o )
{
   nlohmann::json content = o.content;
   if ( !content.is_array() || content.empty() )
   {
      content = nlohmann::json::array();
      content.push_back( TextBlock( o.isError ? "error (no detail available)" : "ok" ) );
   }
   return { { "type", "tool_result" }, { "tool_use_id", toolUseId }, { "content", content }, { "is_error", o.isError } };
}

ToolOutcome ExecuteTool( const ToolCall& call, const ToolContext& ctx )
{
   const clock::time_point t0 = clock::now();
   const String name = S16( call.name );
   if ( !Thread::IsRootThread() )
      return Fail( name, "internal: tools must run on the UI thread" );
   const nlohmann::json in = call.input.is_object() ? call.input : nlohmann::json::object();
   try
   {
      if ( call.name == "list_processes" )
      {
         ToolOutcome o;
         o.content.push_back( TextBlock( ListProcesses().dump() ) );
         o.logLine = OkLine( name, t0 );
         return o;
      }
      if ( call.name == "describe_process" )
      {
         const std::string id = StringField( in, "id" );
         if ( id.empty() )
            return Fail( name, "describe_process needs {\"id\": \"<process id>\"}; call list_processes for ids" );
         const String what = name + " " + S16( id );
         const nlohmann::json d = DescribeProcess( IsoString( id.c_str() ) );
         if ( d.contains( "error" ) && !d.contains( "parameters" ) )
            return Fail( what, S16( d["error"].get<std::string>() ) );
         ToolOutcome o;
         o.content.push_back( TextBlock( d.dump() ) );
         o.logLine = OkLine( what, t0 );
         return o;
      }
      if ( call.name == "get_view_context" )
      {
         View view;
         const std::string viewId = StringField( in, "view_id" );
         if ( !viewId.empty() )
         {
            view = ViewByFullId( IsoString( viewId.c_str() ) );
            if ( view.IsNull() )
               return Fail( name + " " + S16( viewId ), "no view with id '" + S16( viewId ) + "'" );
         }
         else
         {
            String error;
            view = TurnView( ctx, error );
            if ( view.IsNull() )
               return Fail( name, error + (ctx.turnViewId.IsEmpty() ? String( ", or pass view_id" ) : String()) );
         }
         const IsoString fullId = view.FullId();
         const String what = name + " " + String( fullId );
         if ( IsBusy( view ) )
            return Fail( what, "view " + String( fullId ) + " is busy (locked by a running process); try again when it finishes" );
         ToolOutcome o;
         o.content.push_back( TextBlock( BuildViewContext( view ).dump() ) );
         if ( BoolField( in, "include_preview" ) )
         {
            const ViewPreviewResult p = RenderViewPreview( view );
            if ( p.ok )
               o.content.push_back( JpegImageBlock( p.base64 ) );
            else
               o.content.push_back( TextBlock( "preview failed: " + U8( p.error ) ) );
         }
         if ( ctx.inspectedViews != nullptr )
            ctx.inspectedViews->insert( std::string( fullId.c_str() ) );
         o.logLine = OkLine( what, t0 );
         return o;
      }
      if ( call.name == "apply_process" )
         return ApplyProcessTool( in, ctx, t0 );
      if ( call.name == "run_global_process" )
         return RunGlobalTool( in, ctx, t0 );
      if ( call.name == "run_pjsr" )
         return RunPjsrTool( in, ctx, t0 );
      return Fail( name, "unknown tool '" + name + "'; available: list_processes, describe_process, get_view_context"
                         + (ctx.mode == AgentMode::Advisor ? String()
                                                           : String( ", apply_process, run_global_process, run_pjsr (when the user allows scripts)" )) );
   }
   catch ( const pcl::Exception& x )
   {
      return Fail( name, name + " failed: " + x.Message() );
   }
   catch ( const std::exception& x )
   {
      return Fail( name, name + " failed: " + String( x.what() ) );
   }
   catch ( ... )
   {
      return Fail( name, name + " failed: unknown error" );
   }
}

} // namespace pcl
