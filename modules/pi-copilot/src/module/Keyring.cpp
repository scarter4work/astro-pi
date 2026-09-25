// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "Keyring.h"
#include "PICopilotModule.h"

#include <pcl/Exception.h>
#include <pcl/ExternalProcess.h>
#include <pcl/StringList.h>

#include <chrono>
#include <thread>

namespace pcl
{

namespace
{

struct ToolRun
{
   bool      finished = false;
   int       exitCode = -1;
   bool      crashed = false;
   IsoString out;
   IsoString err;
   String    error;   // set when it did not finish
};

IsoString Bytes( const ByteArray& b )
{
   IsoString s;
   if ( !b.IsEmpty() )
      s.Append( reinterpret_cast<const char*>( b.Begin() ), b.Length() );
   return s;
}

ToolRun RunSecretTool( const KeyringId& id, const StringList& args, const IsoString* input )
{
   ToolRun r;
   try
   {
      ExternalProcess p;
      StringList a;
      a << String( "-u" ) << String( "LD_LIBRARY_PATH" ) << id.program;
      for ( const String& s : args )
         a << s;
      p.Start( "/usr/bin/env", a );
      if ( !p.WaitForStarted( 10000 ) )
      {
         r.error = "could not start /usr/bin/env for " + id.program;
         return r;
      }
      if ( input != nullptr )
         p.Write( *input );
      p.CloseStandardInput();
      // Not WaitForFinished(): it can return before a slow child exits
      // (repo memory pi-externalprocess-gotchas). Spin, pumping events.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( PICopilotKeyringTimeoutMs );
      while ( p.IsStarting() || p.IsRunning() )
      {
         if ( std::chrono::steady_clock::now() >= deadline )
         {
            p.Kill();
            r.error = id.program + String().Format( " did not finish within %d s (is the keyring locked, with its "
                                                    "unlock prompt hidden?)", PICopilotKeyringTimeoutMs/1000 );
            return r;
         }
         ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
         std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
      }
      r.finished = true;
      r.exitCode = p.ExitCode();
      r.crashed = p.HasCrashed();
      r.out = Bytes( p.StandardOutput() );
      r.err = Bytes( p.StandardError() );
   }
   catch ( const pcl::Exception& x )
   {
      r.error = "could not run " + id.program + ": " + x.Message();
   }
   catch ( ... )
   {
      r.error = "could not run " + id.program;
   }
   return r;
}

String Detail( const ToolRun& r )
{
   const IsoString e = r.err.Trimmed();
   return e.IsEmpty() ? String( "(no message)" ) : String( e.Left( 200 ) );
}

// env exits 127 when the program cannot be found.
String NotInstalled( const KeyringId& id )
{
   return "'" + id.program + "' is not installed or not on PATH (on Fedora/Nobara: dnf install libsecret; "
          "on Debian/Ubuntu: apt install libsecret-tools)";
}

StringList Attributes( const KeyringId& id )
{
   StringList a;
   a << String( "service" ) << id.service << String( "account" ) << id.account;
   return a;
}

} // namespace

KeyringResult KeyringLookup( const KeyringId& id )
{
   KeyringResult k;
   StringList args;
   args << String( "lookup" );
   args.Add( Attributes( id ) );
   const ToolRun r = RunSecretTool( id, args, nullptr );
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
      k.error = NotInstalled( id );
   else if ( r.exitCode == 0 && !r.crashed )
   {
      k.ok = true;
      k.secret = r.out.Trimmed();
      k.found = !k.secret.IsEmpty();
   }
   else if ( r.exitCode == 1 && r.out.IsEmpty() && r.err.Trimmed().IsEmpty() )
      k.ok = true;   // no such item
   else
      k.error = String().Format( "secret-tool lookup failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

KeyringResult KeyringStore( const KeyringId& id, const String& label, const IsoString& secret )
{
   KeyringResult k;
   StringList args;
   args << String( "store" ) << ("--label=" + label);
   args.Add( Attributes( id ) );
   const ToolRun r = RunSecretTool( id, args, &secret );   // secret-tool reads the secret from stdin
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
      k.error = NotInstalled( id );
   else if ( r.exitCode == 0 && !r.crashed )
      k.ok = true;
   else
      k.error = String().Format( "secret-tool store failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

KeyringResult KeyringClear( const KeyringId& id )
{
   KeyringResult k;
   StringList args;
   args << String( "clear" );
   args.Add( Attributes( id ) );
   const ToolRun r = RunSecretTool( id, args, nullptr );
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
      k.error = NotInstalled( id );
   else if ( (r.exitCode == 0 || r.exitCode == 1) && !r.crashed && r.err.Trimmed().IsEmpty() )
      k.ok = true;   // exit 1 without a message: nothing to clear
   else
      k.error = String().Format( "secret-tool clear failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

} // namespace pcl
