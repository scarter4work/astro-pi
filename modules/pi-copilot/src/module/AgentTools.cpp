// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentTools.h"
#include "AnthropicClient.h"   // JpegImageBlock
#include "ProcessApply.h"
#include "ProcessCatalog.h"
#include "Utf8.h"
#include "ViewContext.h"
#include "ViewPreview.h"

#include <pcl/Exception.h>
#include <pcl/Thread.h>

#include <chrono>
#include <cmath>
#include <exception>

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

View DefaultView( const ToolContext& ctx )
{
   return ctx.activeView ? ctx.activeView() : View::Null();
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
   const String what = "apply_process " + S16( pid ) + " " + Shorten( S16( shown.dump() ), PICopilotToolLogParamChars );

   if ( ctx.mode == AgentMode::Advisor )
      return Fail( what, "apply_process is not available in Advisor mode (read-only); give the user the settings instead" );
   if ( pid.empty() )
      return Fail( "apply_process", "apply_process needs process_id; call list_processes for valid ids" );

   View target;
   const std::string viewId = StringField( in, "view_id" );
   if ( !viewId.empty() )
   {
      target = View::ViewById( IsoString( viewId.c_str() ) );
      if ( target.IsNull() )
         return Fail( what, "no view with id '" + S16( viewId ) + "'" );
   }
   else
   {
      target = DefaultView( ctx );
      if ( target.IsNull() )
         return Fail( what, "no active image: ask the user to open or select an image, or pass view_id" );
   }

   if ( ctx.mode == AgentMode::Guided )
   {
      if ( !ctx.confirm )
         return Fail( what, "internal error: Guided mode has no confirmation callback" );
      const String changes = DescribeParameterChanges( params, tables, PICopilotConfirmChangesChars );
      if ( !ctx.confirm( S16( pid ), String( target.FullId() ), changes ) )
      {
         ToolOutcome o;
         o.isError = true;
         o.content.push_back( TextBlock( "The user declined this apply_process call (" + pid + " on "
                                         + std::string( target.FullId().c_str() )
                                         + "). Nothing was changed. Do not repeat it; ask what they would like instead." ) );
         o.logLine = S16( kErrMarkUtf8 ) + what + S16( kArrowUtf8 ) + "declined by user";
         return o;
      }
   }

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
   o.content.push_back( TextBlock( summary.dump() ) );
   if ( p.ok )
      o.content.push_back( JpegImageBlock( p.base64 ) );
   o.logLine = OkLine( what, t0 );
   return o;
}

} // namespace

AgentMode AgentModeFromIndex( int index )
{
   return index == 1 ? AgentMode::Advisor : index == 2 ? AgentMode::Guided : AgentMode::Copilot;
}

nlohmann::json ToolDefinitions( AgentMode mode )
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
   context["description"] = "Fresh facts about the active view: geometry, per-channel median/MAD/mean/min/max of the "
                            "real data, FITS keywords; with include_preview, also a new auto-stretched preview image.";
   context["input_schema"] = { { "type", "object" },
                               { "properties", { { "include_preview", { { "type", "boolean" },
                                                                        { "description", "Attach a fresh preview image (default false)." } } } } } };
   tools.push_back( context );

   if ( mode != AgentMode::Advisor )
   {
      nlohmann::json props = nlohmann::json::object();
      props["process_id"] = { { "type", "string" }, { "description", "Process id, e.g. PixelMath" } };
      props["parameters"] = { { "type", "object" },
                              { "description", "{parameterId: value} for non-table parameters. Enumerations take the element id as a string." } };
      props["table_parameters"] = { { "type", "object" },
                                    { "description", "{tableParameterId: [[row values in column order], ...]}; replaces the whole table." } };
      props["view_id"] = { { "type", "string" }, { "description", "Target view id; default: the active view." } };
      nlohmann::json apply = nlohmann::json::object();
      apply["name"] = "apply_process";
      apply["description"] = "Run a PixInsight process on the user's real image (recorded in the view's History, so "
                             "it can be undone). Starts from the process's DEFAULT settings and sets only the "
                             "parameters given. Returns ok with the new statistics and a fresh preview, or a precise error.";
      apply["input_schema"] = { { "type", "object" }, { "properties", props },
                                { "required", nlohmann::json::array( { "process_id" } ) } };
      tools.push_back( apply );
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
         const View view = DefaultView( ctx );
         if ( view.IsNull() )
            return Fail( name, "no active image: ask the user to open or select an image" );
         const String what = name + " " + String( view.FullId() );
         if ( IsBusy( view ) )
            return Fail( what, "view " + String( view.FullId() ) + " is busy (locked by a running process); try again when it finishes" );
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
         o.logLine = OkLine( what, t0 );
         return o;
      }
      if ( call.name == "apply_process" )
         return ApplyProcessTool( in, ctx, t0 );
      return Fail( name, "unknown tool '" + name + "'; available: list_processes, describe_process, get_view_context"
                         + (ctx.mode == AgentMode::Advisor ? String() : String( ", apply_process" )) );
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
