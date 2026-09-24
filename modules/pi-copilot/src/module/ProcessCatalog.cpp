// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessCatalog.h"
#include "ProcessSummaries.h"   // generated: kProcessSummariesJson
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>

#include <algorithm>
#include <cfloat>
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
         const int value = v.ToInt();
         for ( const ProcessParameter::EnumerationElement& e : p.EnumerationElements() )
            if ( e.value == value )
               return std::string( e.id.c_str() );
         return value;
      }
   case ProcessParameterType::String:
      return U8( v.ToString() );
   case ProcessParameterType::Block:
      return nullptr;
   default:
      return p.IsNumeric() ? nlohmann::json( v.ToDouble() ) : nlohmann::json( nullptr );
   }
}

nlohmann::json ParameterJson( const ProcessParameter& p )
{
   nlohmann::json j = {
      { "id", std::string( p.Id().c_str() ) },
      { "type", TypeName( p.Type() ) },
      { "readOnly", p.IsReadOnly() },
      { "required", p.IsRequired() }
   };

   // Some enumeration parameters throw a low-level core API error when their
   // element identifiers are queried through cross-module introspection
   // (observed live for PixelMath's newImageColorSpace/newImageSampleFormat:
   // "GetParameterElementIdentifier(): API function error" from both
   // DefaultValue() -- which resolves the default element's id -- and
   // EnumerationElements() itself). That must not poison the whole
   // DescribeProcess() response for an otherwise well-behaved process with
   // 20+ good parameters: surface it loudly on the one parameter instead of
   // silently omitting it or throwing the whole description away.
   try
   {
      const nlohmann::json def = DefaultValueJson( p );
      if ( !def.is_null() )
         j["default"] = def;
      if ( p.IsEnumeration() )
      {
         nlohmann::json e = nlohmann::json::array();
         for ( const ProcessParameter::EnumerationElement& el : p.EnumerationElements() )
            e.push_back( { { "id", std::string( el.id.c_str() ) }, { "value", el.value } } );
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
