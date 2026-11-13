// AutoContrast -- test-only probe process (compiled only with -DAC_BUILD_PROBES=ON; never shipped).
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AutoContrastProbeProcess.h"
#include "AutoContrastProbeInstance.h"
#include "../AutoContrastModule.h"
#include "../AutoContrastVersion.h"

#include "core/Json.h"

#include <pcl/ErrorHandler.h>
#include <pcl/File.h>
#include <pcl/Process.h>

#include <string>
#include <vector>

namespace pcl
{

AutoContrastProbeProcess::AutoContrastProbeProcess() = default;

IsoString AutoContrastProbeProcess::Id() const { return "AutoContrastProbe"; }
IsoString AutoContrastProbeProcess::Categories() const { return "scarter4work"; }
uint32 AutoContrastProbeProcess::Version() const { return 0x100; }

String AutoContrastProbeProcess::Description() const
{
   return "<html><p>Test-only command-line probe for AutoContrast. Not part of any release.</p></html>";
}

ProcessInterface* AutoContrastProbeProcess::DefaultInterface() const { return nullptr; }

ProcessImplementation* AutoContrastProbeProcess::Create() const
{
   return new AutoContrastProbeInstance( this );
}

ProcessImplementation* AutoContrastProbeProcess::Clone( const ProcessImplementation& p ) const
{
   const AutoContrastProbeInstance* instance = dynamic_cast<const AutoContrastProbeInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "AutoContrastProbe: internal error - cannot clone a non-probe instance" );
   return new AutoContrastProbeInstance( *instance );
}

bool AutoContrastProbeProcess::CanProcessViews() const { return false; }
// Measured (PI 1.9.6): a process that declares neither view, global nor image processing is rejected and
// the whole module is unloaded at startup. Declare global; CanExecuteGlobal() still refuses.
bool AutoContrastProbeProcess::CanProcessGlobal() const { return true; }
bool AutoContrastProbeProcess::CanProcessCommandLines() const { return true; }

namespace
{

std::string Utf8( const String& s ) { return s.ToUTF8().c_str(); }
std::string Utf8( const IsoString& s ) { return s.c_str(); }

void WriteJson( const std::string& path, const ac::Json& j )
{
   File f = File::CreateFileForWriting( String::UTF8ToUTF16( path.c_str() ) );
   const std::string text = ac::dump_python( j, 2 ) + "\n";
   f.Write( text.data(), static_cast<fsize_type>( text.size() ) );
   f.Close();
}

} // namespace

// AutoContrastProbe --case=<id> --out=<json>
int AutoContrastProbeProcess::ProcessCommandLine( const StringList& argv ) const
{
   std::string caseId, outPath;
   ac::Json args = ac::Json::array();
   for ( const String& a : argv )
   {
      const std::string s = Utf8( a );
      args.push_back( s );
      if ( s.rfind( "--case=", 0 ) == 0 )
         caseId = s.substr( 7 );
      else if ( s.rfind( "--out=", 0 ) == 0 )
         outPath = s.substr( 6 );
   }
   if ( caseId.empty() || outPath.empty() )
      throw Error( "AutoContrastProbe: usage: AutoContrastProbe --case=<id> --out=<json file>" );

   ac::Json result = ac::Json::object();
   result["case"] = caseId;

   if ( caseId == "scp_launch_reaches_probe" )
   {
      result["reached"] = true;
      result["argv"] = args;
   }
   else if ( caseId == "ids" )
   {
      // What the running core knows about this module's processes, by id.
      const std::vector<std::string> ids = { "AutoContrast", "AutoContrastReferences", "AutoContrastProbe" };
      ac::Json processes = ac::Json::array();
      ac::Json categories = ac::Json::object();
      for ( const std::string& id : ids )
      {
         // Process( id ) throws when the core has no such process.
         Process p( IsoString( id.c_str() ) );
         processes.push_back( id );
         const IsoStringList cats = p.Categories();
         ac::Json c = ac::Json::array();
         for ( const IsoString& cat : cats )
            c.push_back( Utf8( cat ) );
         categories[id] = c;
      }
      result["module"] = Utf8( TheAutoContrastModule->Name() );
      result["version"] = AC_VERSION_STRING;
      result["processes"] = processes;
      result["categories"] = categories;
      result["category"] = categories["AutoContrast"][0];
   }
   else
      throw Error( String( "AutoContrastProbe: unknown case " ) + String( caseId.c_str() ) );

   WriteJson( outPath, result );
   return 0;
}

} // namespace pcl
