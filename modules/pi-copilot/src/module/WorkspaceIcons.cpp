// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "WorkspaceIcons.h"
#include "EvalGuard.h"       // EvaluateScriptDepth
#include "HistoryReader.h"   // ParseXpsmElement
#include "PjsrRunner.h"      // EvaluateAsciiJson, ScriptLiteral
#include "ProcessApply.h"     // NoteProcessInstanceBuild
#include "ProcessSafety.h"    // CompiledProcessSafety, NoInstanceBeforeApproval
#include "JourneyExport.h"   // FileParameterValues, PrivacyStripStepParameters
#include "ToolHelpers.h"     // TextBlock, StringField, Fail
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/XML.h>

#include <algorithm>
#include <map>

namespace pcl
{

const char* const kWorkspaceIconPrompt =
   "- list_process_icons {process_id}: the process icons on the user's PixInsight workspace, i.e. settings they saved: "
   "icon id, process, the parameters that differ from the process defaults, the files they use (by name) and the "
   "processes inside ProcessContainer icons. When the user refers to \"my settings\", \"the icon\", \"what I used "
   "before\" or a setup they already have, look here first.\n"
   "- get_process_icon {icon_id, item, table, from_row}: one icon's full parameters and tables (item: the process "
   "inside a ProcessContainer icon, 1-based). A table too large for one result comes in pages: pass table and "
   "from_row. A file parameter shows as {\"file\": <file name>, \"available\": true/false}; to reuse that file, pass "
   "the same {\"file\": <file name>} object as the value and PI Copilot sets its full path (you never see folders).\n";

namespace
{

constexpr size_t kBudget = PICopilotMaxToolResultChars - 2000;

// One process of an icon (the icon itself, or one item of a ProcessContainer).
struct IconItem
{
   std::string    processId;
   bool           enabled = true;
   nlohmann::json parameters = nlohmann::json::object();
   nlohmann::json tables = nlohmann::json::object();
   std::string    note;   // parse note ("" = fine)
};

struct IconRead
{
   std::string           processId;   // "ProcessContainer" for a container
   bool                  container = false;
   bool                  notRead = false;   // an icon of a process PI Copilot builds no instance of (review I1)
   std::vector<IconItem> items;       // one for a plain icon
};

IconItem ItemFromElement( const XMLElement& e )
{
   IconItem it;
   HistoryStep h;
   String err;
   if ( !ParseXpsmElement( e, h, err ) )
   {
      it.processId = U8( e.AttributeValue( "class" ) );
      it.note = U8( err );
      return it;
   }
   it.processId = h.processId;
   it.parameters = h.parameters;
   it.tables = h.tableParameters;
   it.enabled = e.AttributeValue( "enabled" ).Trimmed() != "false";
   if ( !h.replayable )
      it.note = h.parseNote;
   return it;
}

int& IconScriptRuns()
{
   static int runs = 0;
   return runs;
}

// The processes PI Copilot never builds an instance of before the user approved a run (policy deny and
// confirmAlways: NoInstanceBeforeApproval), except ProcessContainer, whose icons are read (below).
std::vector<std::string> NoInstanceProcessIds()
{
   std::vector<std::string> ids;
   const nlohmann::json& policy = CompiledProcessSafety();
   for ( const char* section : { "deny", "confirmAlways" } )
      if ( policy.contains( section ) && policy[section].is_object() )
         for ( auto it = policy[section].begin(); it != policy[section].end(); ++it )
            if ( it.key() != "ProcessContainer" && it.key().rfind( "_", 0 ) != 0 )
               ids.push_back( it.key() );
   return ids;
}

// Every process icon on the workspace as {icon, xpsm} (or {icon, error}), read through PJSR in one script.
// PCL's ProcessInstance::FromIcon() duplicates the instance, which a ProcessContainer refuses ("Invalid routine
// invoked: MetaProcessContainer::clonationRoutine", measured); the PJSR fromIcon() serializes it fine. Nothing is
// kept past the script. Throws when a script is already running (EvaluateScript cannot nest) or on a bad result.
//
// Review I1 -- instances before approval. fromIcon() duplicates the icon's instance. An icon of a deny/confirmAlways
// process (e.g. BlurXTerminator, StarAlignment) is therefore never opened: it is found by
// ProcessInstance.iconsByProcessId() (an id lookup, no instance) and listed as not read. DECISION (written down, as
// the review asked): ProcessContainer icons ARE read. ProcessContainer is denied because PI Copilot cannot check the
// processes it would RUN; the measured hazard is its CanExecuteGlobal() on a default instance, which is never
// called here -- the duplicate is only serialized (toSource), never validated or executed. A container that holds
// an item of a confirmAlways process duplicates that item too; its contents cannot be known without reading it,
// and Scott asked for container icons to be visible. Default instances of such processes are never built
// (Defaults::Of).
std::vector<std::pair<std::string, std::string>> AllIconSources()
{
   if ( EvaluateScriptDepth() > 0 )
      throw Error( "PixInsight is running a script right now; the workspace icons cannot be read until it ends -- try again" );
   ++IconScriptRuns();
   const nlohmann::json skip = NoInstanceProcessIds();
   const nlohmann::json j = EvaluateAsciiJson(
      "var r = [], ids = ProcessInstance.icons(), skip = {}, noInstance = JSON.parse( " + ScriptLiteral( FromU8( skip.dump() ) ) + " );\n"
      "for ( var k = 0; k < noInstance.length; ++k ) {\n"
      "  try { var a = ProcessInstance.iconsByProcessId( noInstance[k] ); for ( var m = 0; m < a.length; ++m ) skip[a[m]] = noInstance[k]; }\n"
      "  catch ( e ) {}\n"
      "}\n"
      "for ( var i = 0; i < ids.length; ++i ) {\n"
      "  if ( skip.hasOwnProperty( ids[i] ) ) { r.push( { icon: pcWell( ids[i] ), skipped: pcWell( skip[ids[i]] ) } ); continue; }\n"
      "  try { var P = ProcessInstance.fromIcon( ids[i] );\n"
      "        r.push( P ? { icon: pcWell( ids[i] ), xpsm: pcWell( P.toSource( 'XPSM 1.0' ) ) }\n"
      "                  : { icon: pcWell( ids[i] ), error: 'not a process icon' } ); }\n"
      "  catch ( e ) { r.push( { icon: pcWell( ids[i] ), error: pcWell( '' + e ) } ); }\n"
      "}\n"
      "return pcAscii( JSON.stringify( r ) );" );
   std::vector<std::pair<std::string, std::string>> out;
   for ( const nlohmann::json& e : j )
      out.emplace_back( e.at( "icon" ).get<std::string>(),
                        e.contains( "xpsm" ) ? e.at( "xpsm" ).get<std::string>()
                        : e.contains( "skipped" ) ? "\x02" + e.at( "skipped" ).get<std::string>()
                        : "\x01" + e.value( "error", std::string() ) );
   return out;
}

// Parses one icon's source (from AllIconSources). "" or the error.
String ParseIcon( const std::pair<std::string, std::string>& src, IconRead& out )
{
   if ( !src.second.empty() && src.second[0] == '\x01' )
      return FromU8( "icon " + src.first + " cannot be read: " + src.second.substr( 1 ) );
   if ( !src.second.empty() && src.second[0] == '\x02' )
   {
      out.processId = src.second.substr( 1 );
      out.notRead = true;
      return String();
   }
   const std::string& xpsm = src.second;
   XMLDocument doc;
   doc.SetParserOption( XMLParserOption::IgnoreComments );
   doc.Parse( FromU8( xpsm ) );
   const XMLElement* root = doc.RootElement();
   if ( root == nullptr )
      return FromU8( "icon " + src.first + ": PixInsight returned no instance" );
   out.processId = U8( root->AttributeValue( "class" ) );
   out.container = out.processId == "ProcessContainer";
   if ( out.container )
   {
      for ( const XMLElement& e : root->ChildElements() )
         if ( e.Name() == "instance" )
            out.items.push_back( ItemFromElement( e ) );
   }
   else
      out.items.push_back( ItemFromElement( *root ) );
   return String();
}

// The process's default parameters/tables, once per process per call.
struct Defaults
{
   std::map<std::string, IconItem> cache;
   const IconItem& Of( const std::string& processId )
   {
      auto it = cache.find( processId );
      if ( it != cache.end() )
         return it->second;
      IconItem d;
      d.note = "none";   // "none": no defaults known (not compared)
      try
      {
         if ( NoInstanceBeforeApproval( IsoString( processId.c_str() ) ) )
            return cache.emplace( processId, d ).first->second;   // review I1: never a default instance of it
         std::string xpsm;
         {
            const Process proc( IsoString( processId.c_str() ) );
            NoteProcessInstanceBuild( proc.Id(), "iconDefaults" );
            const ProcessInstance P( proc );
            if ( !P.IsNull() )
               xpsm = U8( P.ToSource( "XPSM 1.0" ) );
         }
         if ( !xpsm.empty() )
         {
            XMLDocument doc;
            doc.SetParserOption( XMLParserOption::IgnoreComments );
            doc.Parse( FromU8( xpsm ) );
            if ( doc.RootElement() != nullptr )
            {
               d = ItemFromElement( *doc.RootElement() );
               d.note.clear();
            }
         }
      }
      catch ( ... )
      {
         // not installed / no default instance: nothing differs "from the defaults" then
      }
      return cache.emplace( processId, d ).first->second;
   }
};

nlohmann::json ChangedFromDefaults( const IconItem& it, Defaults& defaults )
{
   const IconItem& d = defaults.Of( it.processId );
   if ( d.note == "none" )
      return nullptr;   // not compared: no default instance is built for this process (or none exists)
   nlohmann::json changed = nlohmann::json::array();
   for ( auto p = it.parameters.begin(); p != it.parameters.end(); ++p )
      if ( !d.parameters.contains( p.key() ) || d.parameters[p.key()] != p.value() )
         changed.push_back( p.key() );
   for ( auto t = it.tables.begin(); t != it.tables.end(); ++t )
      if ( !d.tables.contains( t.key() ) || d.tables[t.key()] != t.value() )
         changed.push_back( t.key() );
   return changed;
}

// The item's parameters and tables for the model: paths reduced to file names, file values as references.
// files: the file names it uses.
nlohmann::json ModelValues( const IconItem& it, nlohmann::json& files )
{
   files = nlohmann::json::array();
   std::vector<FileParameterValue> fv;
   std::string catalogNote;
   try
   {
      fv = FileParameterValues( it.processId, it.parameters, it.tables );
   }
   catch ( const pcl::Exception& x )
   {
      catalogNote = "the columns of a table could not be read (" + U8( x.Message() ) + "); its values are shown by file name only";
   }
   nlohmann::json s = PrivacyStripStepParameters( it.processId, it.parameters, it.tables );
   nlohmann::json& p = s["parameters"];
   nlohmann::json& t = s["tableParameters"];
   for ( const FileParameterValue& v : fv )
   {
      const std::string name = PathFileName( v.value );
      if ( std::find( files.begin(), files.end(), nlohmann::json( name ) ) == files.end() )
         files.push_back( name );
      if ( FileRoleOf( it.processId, v ) != FileRole::Input )
         continue;   // pinned / output / folder values stay plain names: never a reusable reference (I2, M1)
      const nlohmann::json ref = FileReferenceJson( "file", name, !UsableFilePath( v.value ).empty(), std::string() );
      if ( !v.inTable )
         p[v.parameter] = ref;
      else if ( t.contains( v.parameter ) && t[v.parameter].is_array() && v.row < t[v.parameter].size()
                && t[v.parameter][v.row].is_array() && v.column < t[v.parameter][v.row].size() )
         t[v.parameter][v.row][v.column] = ref;
   }
   nlohmann::json r = { { "parameters", p }, { "table_parameters", t } };
   if ( !catalogNote.empty() )
      r["note"] = catalogNote;
   return r;
}

nlohmann::json ItemSummary( const IconItem& it, Defaults& defaults )
{
   nlohmann::json files;
   ModelValues( it, files );
   nlohmann::json s = { { "processId", it.processId }, { "changed", ChangedFromDefaults( it, defaults ) }, { "files", files } };
   if ( !it.note.empty() )
      s["note"] = it.note;
   return s;
}

std::string NotReadText( const std::string& processId )
{
   return "PI Copilot does not open icons of " + processId + ": it never builds an instance of " + processId
        + " before the user approves a run of it. Ask the user for its settings if they matter.";
}

ToolOutcome Ok( const String& what, const nlohmann::json& result )
{
   ToolOutcome o;
   o.content.push_back( TextBlock( result.dump() ) );
   o.logLine = FromU8( "\xE2\x96\xB6 " ) + what + FromU8( " \xE2\x86\x92 " ) + "ok";
   return o;
}

int64 PositiveInt( const nlohmann::json& in, const char* key, bool& bad )
{
   if ( !in.contains( key ) || in[key].is_null() )
      return 0;
   if ( in[key].is_number_integer() && in[key].get<int64>() > 0 )
      return in[key].get<int64>();
   bad = true;
   return 0;
}

} // namespace

nlohmann::json WorkspaceIconToolDefinitions()
{
   auto tool = []( const char* name, const char* desc, nlohmann::json props )
   {
      return nlohmann::json( { { "name", name }, { "description", desc },
                               { "input_schema", { { "type", "object" }, { "properties", props } } } } );
   };
   nlohmann::json t = nlohmann::json::array();
   t.push_back( tool( "list_process_icons", "List the process icons on the user's workspace (settings they saved): icon id, "
                      "process id, parameters that differ from the defaults, files used (by name), and the processes inside "
                      "ProcessContainer icons. Read-only.",
                      { { "process_id", { { "type", "string" }, { "description", "Only icons of this process (or containers holding it)." } } } } ) );
   t.push_back( tool( "get_process_icon", "One process icon's full parameters and tables, to reuse the user's own settings. "
                      "File parameters show as {\"file\": name, \"available\": ...}: pass the same {\"file\": name} object "
                      "to apply_process and PI Copilot sets the full path. A large table comes in pages (table + from_row). Read-only.",
                      { { "icon_id", { { "type", "string" }, { "description", "Icon id (from list_process_icons)." } } },
                        { "item", { { "type", "integer" }, { "description", "For a ProcessContainer icon: the process inside it (1-based)." } } },
                        { "table", { { "type", "string" }, { "description", "Return only this table parameter, in pages." } } },
                        { "from_row", { { "type", "integer" }, { "description", "With table: the first row to return (1-based, default 1)." } } } } ) );
   return t;
}

int WorkspaceIconScriptRunsForSelfTest()
{
   return IconScriptRuns();
}

bool IsWorkspaceIconTool( const std::string& name )
{
   return name == "list_process_icons" || name == "get_process_icon";
}

void AddWorkspaceIconFileCandidates( std::vector<FileCandidate>& to )
{
   try
   {
      for ( const auto& src : AllIconSources() )
      {
         IconRead r;
         if ( !ParseIcon( src, r ).IsEmpty() || r.notRead )
            continue;
         for ( const IconItem& it : r.items )
         {
            try
            {
               std::vector<FileParameterValue> input;
               for ( const FileParameterValue& v : FileParameterValues( it.processId, it.parameters, it.tables ) )
                  if ( FileRoleOf( it.processId, v ) == FileRole::Input )
                     input.push_back( v );
               AddFileCandidates( to, input, "workspace icon " + src.first );
            }
            catch ( ... )
            {
               // a table whose columns cannot be read: this item offers no candidate
            }
         }
      }
   }
   catch ( ... )
   {
      // the icons cannot be listed: no candidates from them
   }
}

ToolOutcome ExecuteWorkspaceIconTool( const ToolCall& call, const ToolContext& /*ctx*/ )
{
   const String name = FromU8( call.name );
   const nlohmann::json in = call.input.is_object() ? call.input : nlohmann::json::object();
   try
   {
      Defaults defaults;
      if ( call.name == "list_process_icons" )
      {
         const std::string filter = StringField( in, "process_id" );
         const std::string canonical = filter.empty() ? std::string() : CanonicalProcessIdOf( filter );
         if ( !filter.empty() && canonical.empty() )
            return Fail( name, FromU8( "no installed process '" + filter + "' (list_processes shows them)" ) );
         nlohmann::json icons = nlohmann::json::array();
         size_t used = 200, omitted = 0;
         for ( const auto& src : AllIconSources() )
         {
            IconRead r;
            const String e = ParseIcon( src, r );
            nlohmann::json row = { { "icon", src.first } };
            if ( !e.IsEmpty() )
               row["error"] = U8( e );
            else
            {
               bool match = canonical.empty() || r.processId == canonical;
               for ( const IconItem& it : r.items )
                  match = match || it.processId == canonical;
               if ( !match )
                  continue;
               row["processId"] = r.processId;
               if ( r.notRead )
                  row["notRead"] = NotReadText( r.processId );
               else if ( r.container )
               {
                  nlohmann::json items = nlohmann::json::array();
                  int n = 0;
                  for ( const IconItem& it : r.items )
                  {
                     nlohmann::json s = ItemSummary( it, defaults );
                     s["item"] = ++n;
                     s["enabled"] = it.enabled;
                     items.push_back( s );
                  }
                  row["items"] = items;
               }
               else if ( !r.items.empty() )
               {
                  const nlohmann::json s = ItemSummary( r.items.front(), defaults );
                  row["changed"] = s.at( "changed" );
                  row["files"] = s.at( "files" );
                  if ( s.contains( "note" ) )
                     row["note"] = s.at( "note" );
               }
            }
            const size_t size = row.dump().size() + 1;
            if ( used + size > kBudget )
            {
               ++omitted;
               continue;
            }
            used += size;
            icons.push_back( row );
         }
         nlohmann::json result = { { "icons", icons }, { "count", icons.size() } };
         if ( omitted > 0 )
            result["omitted"] = { { "icons", omitted }, { "note", "more icons than one result holds: filter with process_id" } };
         if ( icons.empty() )
            result["note"] = filter.empty() ? "there are no process icons on the workspace"
                                            : "no process icon of " + canonical + " on the workspace";
         return Ok( name + String().Format( " (%u)", unsigned( icons.size() ) ), result );
      }
      if ( call.name == "get_process_icon" )
      {
         const std::string iconId = StringField( in, "icon_id" );
         if ( iconId.empty() )
            return Fail( name, "get_process_icon needs icon_id (list_process_icons shows them)" );
         const String what = name + " " + FromU8( iconId );
         bool bad = false;
         const int64 item = PositiveInt( in, "item", bad );
         const int64 fromRow = PositiveInt( in, "from_row", bad );
         if ( bad )
            return Fail( what, "item and from_row must be positive whole numbers" );
         const std::string table = StringField( in, "table" );
         IconRead r;
         {
            bool found = false;
            for ( const auto& src : AllIconSources() )
               if ( src.first == iconId )
               {
                  found = true;
                  const String e = ParseIcon( src, r );
                  if ( !e.IsEmpty() )
                     return Fail( what, e );
                  break;
               }
            if ( !found )
               return Fail( what, FromU8( "no process icon '" + iconId + "' on the workspace (list_process_icons shows them)" ) );
         }
         if ( r.notRead )
            return Fail( what, FromU8( NotReadText( r.processId ) ) );
         if ( r.container && item == 0 )
         {
            nlohmann::json items = nlohmann::json::array();
            int n = 0;
            for ( const IconItem& it : r.items )
            {
               nlohmann::json s = ItemSummary( it, defaults );
               s["item"] = ++n;
               s["enabled"] = it.enabled;
               items.push_back( s );
            }
            return Ok( what, { { "icon", iconId }, { "processId", "ProcessContainer" }, { "items", items },
                               { "note", "A ProcessContainer: pass item (1-based) to read one of its processes in full." } } );
         }
         if ( item > int64( r.items.size() ) || (!r.container && item > 1) )
            return Fail( what, String().Format( "icon %s holds %u process(es); item %lld does not exist", iconId.c_str(),
                                                unsigned( r.items.size() ), static_cast<long long>( item ) ) );
         const IconItem& it = r.items.at( size_t( std::max<int64>( item, 1 ) - 1 ) );
         nlohmann::json files;
         const nlohmann::json v = ModelValues( it, files );
         nlohmann::json head = { { "icon", iconId }, { "processId", it.processId } };
         if ( r.container )
         {
            head["item"] = item;
            head["enabled"] = it.enabled;
         }
         if ( !it.note.empty() )
            head["note"] = it.note;
         if ( !table.empty() )
         {
            const nlohmann::json& t = v.at( "table_parameters" );
            if ( !t.contains( table ) || !t.at( table ).is_array() )
               return Fail( what, FromU8( it.processId + " has no table '" + table + "' in this icon" ) );
            const nlohmann::json& rows = t.at( table );
            const size_t total = rows.size();
            const size_t first = fromRow == 0 ? 0 : size_t( fromRow - 1 );
            if ( first > total || (first == total && total > 0) )
               return Fail( what, String().Format( "from_row %lld is past the last row (the table has %u)",
                                                   static_cast<long long>( fromRow ), unsigned( total ) ) );
            nlohmann::json page = nlohmann::json::array();
            size_t used = head.dump().size() + 300, k = first;
            for ( ; k < total; ++k )
            {
               const size_t size = rows[k].dump().size() + 1;
               if ( !page.empty() && used + size > kBudget )
                  break;
               used += size;
               page.push_back( rows[k] );
            }
            head["table"] = table;
            head["totalRows"] = total;
            head["fromRow"] = first + 1;
            head["table_parameters"] = { { table, page } };
            head["moreRows"] = k < total;
            if ( k < total )
               head["nextFromRow"] = k + 1;
            return Ok( what + " " + FromU8( table ), head );
         }
         head["changed"] = ChangedFromDefaults( it, defaults );
         head["files"] = files;
         head["parameters"] = v.at( "parameters" );
         head["table_parameters"] = v.at( "table_parameters" );
         if ( v.contains( "note" ) )
            head["note"] = v.at( "note" );
         // Too large for one result: the largest tables are left out (named), to be read with table + from_row.
         nlohmann::json& tables = head["table_parameters"];
         while ( head.dump().size() > kBudget )
         {
            std::string largest;
            size_t largestSize = 0;
            for ( auto t = tables.begin(); t != tables.end(); ++t )
               if ( t.value().is_array() && t.value().dump().size() > largestSize )
               {
                  largest = t.key();
                  largestSize = t.value().dump().size();
               }
            if ( largest.empty() )
               return Fail( what, "this icon's parameters are too large for one result even without its tables" );
            tables[largest] = { { "rows", tables[largest].size() },
                                { "omitted", "too large for this result: call get_process_icon with table \"" + largest
                                             + "\" (pages with from_row)" } };
         }
         return Ok( what, head );
      }
      return Fail( name, "unknown workspace icon tool '" + name + "'" );
   }
   catch ( const pcl::Exception& x )
   {
      return Fail( name, name + " failed: " + x.Message() );
   }
   catch ( const std::exception& x )
   {
      return Fail( name, name + " failed: " + String( x.what() ) );
   }
}

} // namespace pcl
