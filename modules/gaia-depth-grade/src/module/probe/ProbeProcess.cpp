// TEST-ONLY probe process (see ProbeProcess.h). Never part of a release build.
#include "ProbeProcess.h"
#include "ProbeJson.h"

#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>
#include <pcl/File.h>

#include <exception>

namespace pcl
{

GDGProbeProcess* TheGDGProbeProcess = nullptr;
static GDGProbeString* TheCaseParameter = nullptr;
static GDGProbeString* TheArgsParameter = nullptr;
static GDGProbeString* TheOutputParameter = nullptr;

GDGProbeProcess::GDGProbeProcess()
{
   TheGDGProbeProcess = this;
   TheCaseParameter = new GDGProbeString( this, "probeCase" );
   TheArgsParameter = new GDGProbeString( this, "probeArgs" );
   TheOutputParameter = new GDGProbeString( this, "outputPath" );
}

IsoString GDGProbeProcess::Id() const { return "GaiaDepthGradeProbe"; }
IsoString GDGProbeProcess::Categories() const { return "scarter4work"; }
uint32 GDGProbeProcess::Version() const { return 0x100; }
String GDGProbeProcess::Description() const
{
   return "<html><p>Test-only probe for the Gaia Depth Grade module's in-PixInsight tests. "
          "Not part of any release.</p></html>";
}
ProcessImplementation* GDGProbeProcess::Create() const { return new GDGProbeInstance( this ); }
ProcessImplementation* GDGProbeProcess::Clone( const ProcessImplementation& p ) const
{
   const GDGProbeInstance* i = dynamic_cast<const GDGProbeInstance*>( &p );
   if ( i == nullptr )
      throw Error( "GaiaDepthGradeProbe: cannot clone a foreign instance" );
   return new GDGProbeInstance( *i );
}
bool GDGProbeProcess::CanProcessViews() const { return true; }
bool GDGProbeProcess::CanProcessGlobal() const { return true; }

GDGProbeInstance::GDGProbeInstance( const MetaProcess* m ) : ProcessImplementation( m ) {}
GDGProbeInstance::GDGProbeInstance( const GDGProbeInstance& x ) : ProcessImplementation( x ) { Assign( x ); }

void GDGProbeInstance::Assign( const ProcessImplementation& p )
{
   const GDGProbeInstance* x = dynamic_cast<const GDGProbeInstance*>( &p );
   if ( x != nullptr )
   {
      p_case = x->p_case;
      p_args = x->p_args;
      p_output = x->p_output;
   }
}

bool GDGProbeInstance::CanExecuteOn( const View&, String& ) const { return true; }
bool GDGProbeInstance::CanExecuteGlobal( String& ) const { return true; }

bool GDGProbeInstance::Finish( const IsoString& json )
{
   if ( p_output.IsEmpty() )
      throw Error( "GaiaDepthGradeProbe: outputPath is empty" );
   File::WriteTextFile( p_output, json );
   return true;
}

bool GDGProbeInstance::ExecuteOn( View& view )
{
   IsoString body;
   try
   {
      if ( p_case == "sky_adapter_convention" )
         body = ProbeSkyAdapterConvention( view, p_args );
      else
         throw Error( "GaiaDepthGradeProbe: unknown view case '" + p_case + "'" );
      return Finish( "{\"ok\":true,\"case\":" + gdgprobe::Str( p_case ) + "," + body + "}\n" );
   }
   catch ( const Exception& e )
   {
      return Finish( "{\"ok\":false,\"case\":" + gdgprobe::Str( p_case ) + ",\"error\":" + gdgprobe::Str( e.Message() ) + "}\n" );
   }
   catch ( const std::exception& e )
   {
      return Finish( "{\"ok\":false,\"case\":" + gdgprobe::Str( p_case ) + ",\"error\":" + gdgprobe::Str( String( e.what() ) ) + "}\n" );
   }
}

bool GDGProbeInstance::ExecuteGlobal()
{
   IsoString body;
   try
   {
      if ( p_case == "local_gaia" )
         body = ProbeLocalGaia( p_args );
      else if ( p_case == "online" )
         body = ProbeOnline( p_args );
      else if ( p_case == "tap_thread" )
         body = ProbeTapThread( p_args );
      else if ( p_case == "introspect" )
         body = ProbeIntrospect( p_args );
      else if ( p_case == "solve" )
         body = ProbeSolve( p_args );
      else
         throw Error( "GaiaDepthGradeProbe: unknown global case '" + p_case + "'" );
      return Finish( "{\"ok\":true,\"case\":" + gdgprobe::Str( p_case ) + "," + body + "}\n" );
   }
   catch ( const Exception& e )
   {
      return Finish( "{\"ok\":false,\"case\":" + gdgprobe::Str( p_case ) + ",\"error\":" + gdgprobe::Str( e.Message() ) + "}\n" );
   }
   catch ( const std::exception& e )
   {
      return Finish( "{\"ok\":false,\"case\":" + gdgprobe::Str( p_case ) + ",\"error\":" + gdgprobe::Str( String( e.what() ) ) + "}\n" );
   }
}

void* GDGProbeInstance::LockParameter( const MetaParameter* p, size_type )
{
   if ( p == TheCaseParameter )   return p_case.Begin();
   if ( p == TheArgsParameter )   return p_args.Begin();
   if ( p == TheOutputParameter ) return p_output.Begin();
   return nullptr;
}

bool GDGProbeInstance::AllocateParameter( size_type n, const MetaParameter* p, size_type )
{
   String* s = nullptr;
   if ( p == TheCaseParameter )        s = &p_case;
   else if ( p == TheArgsParameter )   s = &p_args;
   else if ( p == TheOutputParameter ) s = &p_output;
   else return false;
   s->Clear();
   if ( n > 0 )
      s->SetLength( n );
   return true;
}

size_type GDGProbeInstance::ParameterLength( const MetaParameter* p, size_type ) const
{
   if ( p == TheCaseParameter )   return p_case.Length();
   if ( p == TheArgsParameter )   return p_args.Length();
   if ( p == TheOutputParameter ) return p_output.Length();
   return 0;
}

} // namespace pcl
