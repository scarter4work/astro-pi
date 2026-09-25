// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessCatalog.h"
#include "PICopilotModule.h"
#include "ProcessSummaries.h"   // generated: kProcessSummariesJson
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>
#include <pcl/api/APIInterface.h>

#include <algorithm>
#include <cfloat>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pcl
{

namespace
{

nlohmann::json StringListJson( const IsoStringList& list )
{
   nlohmann::json a = nlohmann::json::array();
   for ( const IsoString& s : list )
      a.push_back( std::string( s.c_str() ) );
   return a;
}

const char* TypeName( ProcessParameter::data_type t )
{
   switch ( t )
   {
   case ProcessParameterType::UInt8:       return "UInt8";
   case ProcessParameterType::Int8:        return "Int8";
   case ProcessParameterType::UInt16:      return "UInt16";
   case ProcessParameterType::Int16:       return "Int16";
   case ProcessParameterType::UInt32:      return "UInt32";
   case ProcessParameterType::Int32:       return "Int32";
   case ProcessParameterType::UInt64:      return "UInt64";
   case ProcessParameterType::Int64:       return "Int64";
   case ProcessParameterType::Float:       return "Float";
   case ProcessParameterType::Double:      return "Double";
   case ProcessParameterType::Boolean:     return "Boolean";
   case ProcessParameterType::Enumeration: return "Enumeration";
   case ProcessParameterType::String:      return "String";
   case ProcessParameterType::Block:       return "Block";
   case ProcessParameterType::Table:       return "Table";
   default:                                return "Invalid";
   }
}

// PJSR route of EnumerationInfoOf(). Only the element IDENTIFIER query is
// broken; the element count and values still come from the core natively.
// Each value is assigned to a fresh instance in PJSR and the element id the
// core's own toSource() writes for it is recorded -- the core's real ids, not
// a hand-written table. Only native element values are ever assigned: an
// arbitrary number can crash the core inside toSource() (seen live: SIGSEGV in
// pi::MetaEnumeration::ValueToSource after assigning SCNR.colorToRemove = 4,
// a protectionMethod constant). Throws Error with the reason.
EnumerationInfo ScriptEnumerationInfo( const IsoString& processId, const IsoString& paramId )
{
   // Both ids are interpolated into source code: accept identifiers only.
   if ( !processId.IsValidIdentifier() || !paramId.IsValidIdentifier() )
      throw Error( "not a plain identifier" );

   // ProcessParameter::Handle() is private; look the same handle up through
   // the public API table (exactly what Process(id)/ProcessParameter(P, id) do).
   const meta_process_handle hp = (*API->Process->GetProcessByName)( ModuleHandle(), processId.c_str() );
   const meta_parameter_handle h = (hp == nullptr) ? nullptr : (*API->Process->GetParameterByName)( hp, paramId.c_str() );
   if ( h == nullptr )
      throw Error( "the core did not resolve the parameter handle" );
   const size_type count = (*API->Process->GetParameterElementCount)( h );
   if ( count == 0 )
      throw Error( "the core reports no enumeration elements" );
   IsoString values = "[";
   for ( size_type i = 0; i < count; ++i )
   {
      if ( i > 0 )
         values << ',';
      values << IsoString( int( (*API->Process->GetParameterElementValue)( h, i ) ) );
   }
   values << ']';

   IsoString script =
      "(function()\n"
      "{\n"
      "   try\n"
      "   {\n"
      "      var re = /^P\\.@PARAM@ = @PROC@\\.([A-Za-z_][A-Za-z0-9_]*);$/m;\n"
      "      var values = @VALUES@;\n"
      "      var elements = [];\n"
      "      for ( var i = 0; i < values.length; ++i )\n"
      "      {\n"
      "         var Q = new @PROC@;\n"
      "         Q.@PARAM@ = values[i];\n"
      "         var src = Q.toSource( \"JavaScript\", \"P\", 0, 0 );\n"
      "         var m = re.exec( src );\n"
      "         if ( !m )\n"
      "            throw new Error( \"toSource() wrote no element id for value \" + values[i] );\n"
      "         elements.push( { id: m[1], value: values[i] } );\n"
      "      }\n"
      "      var d = re.exec( (new @PROC@).toSource( \"JavaScript\", \"P\", 0, 0 ) );\n"
      "      return JSON.stringify( { elements: elements, defaultId: d ? d[1] : \"\" } );\n"
      "   }\n"
      "   catch ( e )\n"
      "   {\n"
      "      return JSON.stringify( { error: String( e ) } );\n"
      "   }\n"
      "})();\n";
   script.ReplaceString( IsoString( "@PROC@" ), processId );
   script.ReplaceString( IsoString( "@PARAM@" ), paramId );
   script.ReplaceString( IsoString( "@VALUES@" ), values );

   const Variant result = ThePICopilotModule->EvaluateScript( String( script ), "JavaScript" );
   const nlohmann::json j = nlohmann::json::parse( U8( result.ToString() ) );
   if ( j.contains( "error" ) )
      throw Error( String::UTF8ToUTF16( j.at( "error" ).get<std::string>().c_str() ) );

   EnumerationInfo info;
   info.viaScript = true;
   for ( const nlohmann::json& e : j.at( "elements" ) )
   {
      ProcessParameter::EnumerationElement el;
      el.id = IsoString( e.at( "id" ).get<std::string>().c_str() );
      el.value = e.at( "value" ).get<int>();
      info.elements << el;
   }
   info.defaultId = IsoString( j.at( "defaultId" ).get<std::string>().c_str() );
   if ( info.elements.IsEmpty() )
      throw Error( "the script runtime reported no elements" );
   return info;
}

} // namespace

const EnumerationInfo& EnumerationInfoOf( const ProcessParameter& p )
{
   // Root thread only (catalog introspection + EvaluateScript), so a plain
   // map needs no lock.
   static std::map<std::string, EnumerationInfo> cache;

   const ProcessParameter table = p.ParentTable();
   const IsoString processId = p.ParentProcess().Id();
   const IsoString paramId = table.IsNull() ? p.Id() : table.Id() + '.' + p.Id();
   const std::string key = std::string( processId.c_str() ) + '.' + paramId.c_str();
   auto it = cache.find( key );
   if ( it != cache.end() )
      return it->second;

   const String name = String( processId ) + '.' + String( paramId );
   if ( !p.IsEnumeration() )
      throw Error( name + " is not an enumeration parameter" );

   EnumerationInfo info;
   String nativeError;
   try
   {
      info.elements = p.EnumerationElements();
      const int index = p.DefaultValue().ToInt();   // an INDEX (ProcessParameter.cpp:333-338)
      if ( index >= 0 && size_type( index ) < info.elements.Length() )
         info.defaultId = info.elements[index].id;
   }
   catch ( const pcl::Exception& x )
   {
      nativeError = x.Message();
   }

   if ( !nativeError.IsEmpty() )
   {
      if ( !table.IsNull() )
         throw Error( name + ": the element identifiers of this enumeration cannot be read ("
                      + nativeError + "; table columns have no script fallback)" );
      try
      {
         info = ScriptEnumerationInfo( processId, paramId );
      }
      catch ( const pcl::Exception& x )
      {
         throw Error( name + ": the element identifiers of this enumeration cannot be read (native: "
                      + nativeError + "; script: " + x.Message() + ")" );
      }
      catch ( const std::exception& x )
      {
         throw Error( name + ": the element identifiers of this enumeration cannot be read (native: "
                      + nativeError + "; script: " + String( x.what() ) + ")" );
      }
   }

   return cache.emplace( key, std::move( info ) ).first->second;
}

namespace
{

// ProcessParameter::DefaultValue() throws pcl::Error for table parameters
// (ProcessParameter.h:490: "For table parameters this function throws an
// Error exception") -- so a table parameter must never reach that call.
nlohmann::json DefaultValueJson( const ProcessParameter& p )
{
   if ( p.IsTable() )
      return nullptr;
   const Variant v = p.DefaultValue();
   if ( !v.IsValid() )
      return nullptr;
   switch ( p.Type() )
   {
   case ProcessParameterType::Boolean:
      return v.ToBoolean();
   case ProcessParameterType::Enumeration:
      {
         // DefaultValue() is the default element's INDEX
         // (GetParameterDefaultElementIndex, ProcessParameter.cpp:333-338),
         // not its value; EnumerationInfoOf() resolves it to the element id
         // (or reads it from PJSR when native element ids are unreadable).
         return EnumerationDefaultJson( EnumerationInfoOf( p ), v.ToInt() );
      }
   case ProcessParameterType::String:
      return U8( v.ToString() );
   case ProcessParameterType::Block:
      return nullptr;
   default:
      return p.IsNumeric() ? nlohmann::json( v.ToDouble() ) : nlohmann::json( nullptr );
   }
}

// Compact on purpose (a describe_process result must fit one tool_result,
// PICopilotMaxToolResultChars, for every installed process -- self-tested):
// "readOnly"/"required" only when true, enumerations as their element ids
// (the form apply_process documents; the integer values are not needed).
nlohmann::json ParameterJson( const ProcessParameter& p )
{
   nlohmann::json j = {
      { "id", std::string( p.Id().c_str() ) },
      { "type", TypeName( p.Type() ) }
   };
   if ( p.IsReadOnly() )
      j["readOnly"] = true;
   if ( p.IsRequired() )
      j["required"] = true;

   // Enumeration element ids come from EnumerationInfoOf() (native, else
   // PJSR introspection). If BOTH routes fail for one parameter, that must
   // not poison the whole DescribeProcess() response for an otherwise
   // well-behaved process with 20+ good parameters: surface it loudly on the
   // one parameter instead of silently omitting it or throwing the whole
   // description away.
   try
   {
      const nlohmann::json def = DefaultValueJson( p );
      if ( !def.is_null() )
         j["default"] = def;
      else if ( p.IsEnumeration() )
         j["defaultNote"] = "the default element could not be identified; omit this parameter to keep it";
      if ( p.IsEnumeration() )
      {
         nlohmann::json e = nlohmann::json::array();
         for ( const ProcessParameter::EnumerationElement& el : EnumerationInfoOf( p ).elements )
            e.push_back( std::string( el.id.c_str() ) );
         j["enumeration"] = e;
      }
   }
   catch ( const pcl::Exception& x )
   {
      j.erase( "default" );
      j["error"] = U8( x.Message() );
   }

   if ( p.IsNumeric() )
   {
      double lo = 0, hi = 0;
      p.GetNumericRange( lo, hi );
      if ( lo > -DBL_MAX )
         j["min"] = lo;
      if ( hi < DBL_MAX )
         j["max"] = hi;
   }
   if ( p.IsTable() )
   {
      nlohmann::json cols = nlohmann::json::array();
      for ( const ProcessParameter& col : p.TableColumns() )
         cols.push_back( ParameterJson( col ) );
      j["columns"] = cols;
   }
   return j;
}

} // namespace

nlohmann::json EnumerationDefaultJson( const EnumerationInfo& info, int defaultIndex )
{
   if ( !info.defaultId.IsEmpty() )
      return std::string( info.defaultId.c_str() );
   // Never the bare index: apply_process reads an integer as an element
   // VALUE, so a model echoing it back could pick the wrong element.
   if ( defaultIndex >= 0 && size_type( defaultIndex ) < info.elements.Length() )
      return std::string( info.elements[defaultIndex].id.c_str() );
   return nullptr;
}

const nlohmann::json& CompiledProcessSummaries()
{
   static const nlohmann::json summaries = nlohmann::json::parse( kProcessSummariesJson );
   return summaries;
}

nlohmann::json ListProcesses()
{
   const nlohmann::json& summaries = CompiledProcessSummaries();
   std::vector<std::pair<std::string, nlohmann::json>> rows;
   for ( const Process& P : Process::AllProcesses() )
   {
      const std::string id( P.Id().c_str() );
      nlohmann::json row = { { "id", id }, { "categories", StringListJson( P.Categories() ) } };
      if ( summaries.contains( id ) )
         row["summary"] = summaries.at( id );
      rows.emplace_back( id, std::move( row ) );
   }
   std::sort( rows.begin(), rows.end(),
              []( const auto& a, const auto& b ) { return a.first < b.first; } );
   nlohmann::json list = nlohmann::json::array();
   for ( auto& r : rows )
      list.push_back( std::move( r.second ) );
   nlohmann::json out;
   out["count"] = list.size();
   out["processes"] = std::move( list );
   return out;
}

nlohmann::json DescribeProcess( const IsoString& id )
{
   nlohmann::json out;
   std::unique_ptr<Process> P;
   try
   {
      P.reset( new Process( id ) );   // throws Error for an unknown id (Process.h:92)
   }
   catch ( const pcl::Exception& x )
   {
      out["error"] = "unknown process id: " + std::string( id.c_str() ) + " (" + U8( x.Message() ) + ")";
      return out;
   }

   const std::string sid( P->Id().c_str() );
   out["id"] = sid;
   out["categories"] = StringListJson( P->Categories() );
   const nlohmann::json& summaries = CompiledProcessSummaries();
   if ( summaries.contains( sid ) )
      out["summary"] = summaries.at( sid );
   String description = P->Description();
   if ( description.Length() > PICopilotMaxProcessDescriptionChars )
      description = description.Left( PICopilotMaxProcessDescriptionChars ) + "...";
   out["description"] = U8( description );
   nlohmann::json params = nlohmann::json::array();
   for ( const ProcessParameter& p : P->Parameters() )
      params.push_back( ParameterJson( p ) );
   out["parameters"] = params;
   return out;
}

} // namespace pcl
