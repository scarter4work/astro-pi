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
   if ( p.IsString() )
      s += "," + Field( "allowedCharacters", Str( p.AllowedCharacters() ) );
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
      for ( const ProcessParameter& col : p.TableColumns() )
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
      for ( const ProcessParameter& p : P.Parameters() )
         list += (n++ ? "," : "") + Describe( p );
      out += (np++ ? "," : "") + Str( id ) + ":" + list + "]";
   }
   return out + "}";
}

} // namespace pcl
