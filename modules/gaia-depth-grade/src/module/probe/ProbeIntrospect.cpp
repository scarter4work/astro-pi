// TEST-ONLY probe case `introspect` (plan Task 14): the parameter and table-column ids, in
// declaration order, of installed processes, read through the PCL process introspection API
// (pcl::Process::Parameters, ProcessParameter::TableColumns). The solver reads StarAlignment's
// outputData columns by id; this probe records what those ids are on the running PixInsight.
//
// probeArgs = space-separated process ids (e.g. "StarAlignment Gaia").
#include "ProbeJson.h"
#include "ProbeProcess.h"

#include <pcl/ErrorHandler.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>

namespace pcl
{

using namespace gdgprobe;

namespace
{

IsoString TypeName( const ProcessParameter& p )
{
   if ( p.IsTable() ) return "table";
   if ( p.IsBoolean() ) return "bool";
   if ( p.IsEnumeration() ) return "enum";
   if ( p.IsString() ) return "string";
   if ( p.IsBlock() ) return "block";
   if ( p.IsInteger() ) return "int";
   if ( p.IsReal() ) return "real";
   return IsoString( int( p.Type() ) );
}

IsoString Describe( const ProcessParameter& p )
{
   IsoString s = "{" + Field( "id", Str( String( p.Id() ) ) ) + "," + Field( "type", Str( String( TypeName( p ) ) ) ) +
                 "," + Field( "dataType", IsoString( int( p.Type() ) ) ) + "," +
                 Field( "readOnly", p.IsReadOnly() ? "true" : "false" );
   // Defaults and ranges (for the module's own GaiaDepthGrade parameter table).
   if ( p.IsNumeric() )
      s += "," + Field( "default", Num( p.DefaultValue().ToDouble() ) ) + "," + Field( "min", Num( p.MinimumValue() ) ) +
           "," + Field( "max", Num( p.MaximumValue() ) ) +
           (p.IsReal() ? "," + Field( "precision", IsoString( p.Precision() ) ) : IsoString());
   else if ( p.IsBoolean() )
      s += "," + Field( "default", p.DefaultValue().ToBool() ? "true" : "false" );
   else if ( p.IsEnumeration() )
      s += "," + Field( "default", IsoString( p.DefaultValue().ToInt() ) );
   if ( p.IsString() )
   {
      // Some third-party processes (StarXTerminator) fail this API call: record that.
      try
      {
         s += "," + Field( "allowedCharacters", Str( p.AllowedCharacters() ) );
      }
      catch ( const Exception& x )
      {
         s += "," + Field( "allowedCharacters", Str( "unavailable: " + x.Message() ) );
      }
   }
   if ( p.IsEnumeration() )
   {
      // Some core processes do not publish element identifiers (the API call fails): record that.
      try
      {
         IsoString e = "[";
         int n = 0;
         for ( const auto& el : p.EnumerationElements() )
            e += (n++ ? "," : "") + Str( String( el.id ) + "=" + String( el.value ) );
         s += "," + Field( "elements", e + "]" );
      }
      catch ( const Exception& x )
      {
         s += "," + Field( "elements", Str( "unavailable: " + x.Message() ) );
      }
   }
   if ( p.IsTable() )
   {
      IsoString c = "[";
      int n = 0;
      const ProcessParameter::parameter_list columns = p.TableColumns(); // const: see Parameters() below
      for ( const ProcessParameter& col : columns )
         c += (n++ ? "," : "") + Describe( col );
      s += "," + Field( "columns", c + "]" );
   }
   return s + "}";
}

} // namespace

IsoString ProbeIntrospect( const String& args )
{
   StringList ids;
   args.Break( ids, ' ', true/*trim*/ );
   IsoString out = "\"processes\":{";
   int np = 0;
   for ( const String& id : ids )
   {
      if ( id.IsEmpty() )
         continue;
      Process P{ IsoString( id ) }; // throws Error when no such process is installed
      IsoString list = "[";
      int n = 0;
      // Const arrays: range-for over a temporary calls the non-const begin() (EnsureUnique), which
      // GCC 16 flags with a false -Walloc-size-larger-than.
      const Process::parameter_list params = P.Parameters();
      for ( const ProcessParameter& p : params )
         list += (n++ ? "," : "") + Describe( p );
      out += (np++ ? "," : "") + Str( id ) + ":" + list + "]";
      IsoString cats = "[";
      int nc = 0;
      for ( const IsoString& c : P.Categories() )
         cats += (nc++ ? "," : "") + Str( String( c ) );
      out += "," + Str( id + ".categories" ) + ":" + cats + "]";
   }
   return out + "}";
}

} // namespace pcl
