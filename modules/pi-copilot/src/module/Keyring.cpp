// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "Keyring.h"
#include "PICopilotModule.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/ExternalProcess.h>
#include <pcl/StringList.h>

#include <chrono>
#include <string>
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
   String    error;              // set when it did not finish
   bool      inputError = false; // stdin could not be written/closed (child already gone)
};

std::function<void( bool )> g_waitNotifier;

// Announces a slow call (after PICopilotKeyringWaitNoticeMs) and its end.
class WaitNotice
{
public:
   WaitNotice() : m_start( std::chrono::steady_clock::now() ) {}
   ~WaitNotice()
   {
      if ( m_shown && g_waitNotifier )
         try { g_waitNotifier( false ); } catch ( ... ) {}
   }
   void Poll()
   {
      if ( !m_shown && g_waitNotifier
        && std::chrono::steady_clock::now() - m_start >= std::chrono::milliseconds( PICopilotKeyringWaitNoticeMs ) )
      {
         m_shown = true;
         g_waitNotifier( true );
      }
   }
private:
   std::chrono::steady_clock::time_point m_start;
   bool m_shown = false;
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
   WaitNotice notice;
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
      {
         // Only when there is input: CloseStandardInput() throws once the
         // child has exited (ExternalProcess.h:316-319), and a child that
         // exits early (bad arguments, keyring error) must still yield its
         // finished result -- exit code and stderr -- below.
         try
         {
            p.Write( *input );
            p.CloseStandardInput();
         }
         catch ( ... )
         {
            r.inputError = true;
         }
      }
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
         notice.Poll();
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

// Measured (self-test diagnostic, this PixInsight 2.10.8 build under
// --automation-mode): pcl::ExternalProcess reports the child's stderr bytes
// through StandardOutput(), not StandardError() -- confirmed with distinct
// markers on both real streams (a lookup script printing "REALSTDOUT" to fd 1
// and "REALSTDERR" to fd 2 comes back as r.out == "REALSTDOUTREALSTDERR",
// r.err empty). r.err is therefore not a reliable place to look for a failed
// call's diagnostic text in this environment; r.out is, whenever the call
// failed. On the SUCCESS path this never matters: secret-tool writes nothing
// to stderr when it succeeds, so a successful lookup's secret is still
// exactly r.out. FailureText() is used only on failure branches (never mixed
// with a real secret), and still prefers r.err first in case some other
// build/platform genuinely keeps the streams separate.
IsoString FailureText( const ToolRun& r )
{
   const IsoString e = r.err.Trimmed();
   return e.IsEmpty() ? r.out.Trimmed() : e;
}

// Bounded to 200 CHARACTERS (not bytes): decode the failure text as UTF-8
// first (it can be locale-translated), then truncate the decoded text, so a
// truncation point can never fall inside a multi-byte sequence and mangle
// it. Plain String( IsoString ) (the previous implementation) widens bytes
// as ISO-8859-1, which corrupts any non-ASCII UTF-8 byte regardless of
// length.
String FormatDiagnostic( const IsoString& e, bool inputError )
{
   if ( e.IsEmpty() )
      return String( "(no message)" );
   String d = FromU8( std::string( e.c_str(), e.Length() ) );
   if ( d.Length() > 200 )
      d = d.Left( 200 );
   if ( inputError )
      d += " (it exited before reading its input)";
   return d;
}

String Detail( const ToolRun& r )
{
   return FormatDiagnostic( FailureText( r ), r.inputError );
}

// `secret-tool search`'s success format embeds the secret itself ("secret =
// <value>") in stdout, so unlike Detail() this NEVER falls back to r.out: a
// partial write before a genuine failure must never surface the secret in a
// visible error, warning, note or log line (hard rule -- see KeyringId.h /
// KeyringExistsResult). A search failure may show "(no message)" more often
// than Detail() would in the merged-channel environment (no r.out fallback)
// -- privacy over verbosity, and search's own .error is not currently shown
// to the user anyway (KeyringLookup only checks ok && exists).
String SearchDetail( const ToolRun& r )
{
   return FormatDiagnostic( r.err.Trimmed(), r.inputError );
}

// The proven signature (keyring-flake-investigation.md): ksecretd cannot
// decrypt the secret because the DH shared-secret padding mismatched, and
// reports the misleading D-Bus error below. Matched narrowly (both
// fragments) so an unrelated "session" error is never retried. Reads
// FailureText(), not r.err directly -- see its comment above.
bool IsSessionMismatch( const ToolRun& r )
{
   if ( !r.finished || r.crashed || r.exitCode != 1 )
      return false;
   const IsoString t = FailureText( r );
   return t.Contains( "Can't find session" ) && t.Contains( "/org/freedesktop/secrets/session/" );
}

// The other half of the same proven signature: ksecretd returns the secret
// re-encrypted under a key libsecret can't derive back; libsecret's
// decrypt/unpad fails silently and secret-tool exits 1 with nothing on
// either stream -- byte-identical to a genuine "no such item".
bool IsSilentMiss( const ToolRun& r )
{
   return r.finished && !r.crashed && r.exitCode == 1 && r.out.IsEmpty() && r.err.Trimmed().IsEmpty();
}

// A fresh secret-tool process opens a fresh DH session (investigation:
// failure is per-session and independent across processes, p ~= 1/256), so a
// bounded retry of the same call is a correct remedy for this one signature.
constexpr int kMaxSecretToolAttempts = 3;

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

KeyringWaitScope::KeyringWaitScope( std::function<void( bool )> notify ) : m_previous( g_waitNotifier )
{
   g_waitNotifier = std::move( notify );
}

KeyringWaitScope::~KeyringWaitScope()
{
   g_waitNotifier = std::move( m_previous );
}

KeyringResult KeyringLookup( const KeyringId& id )
{
   KeyringResult k;
   StringList args;
   args << String( "lookup" );
   args.Add( Attributes( id ) );
   ToolRun r;
   for ( int attempt = 1; ; ++attempt )
   {
      r = RunSecretTool( id, args, nullptr );
      if ( attempt >= kMaxSecretToolAttempts || !IsSilentMiss( r ) )
         break;
      // Ambiguous: a silent miss is indistinguishable from a genuine "no
      // such item" (see KeyringResult). Disambiguate with a non-prompting
      // search before deciding whether to retry -- a locked keyring, whose
      // items `search` skips without --unlock, still ends up "not found"
      // here, same as today.
      const KeyringExistsResult ex = KeyringSearchExists( id );
      if ( !(ex.ok && ex.exists) )
         break;   // genuine miss, locked, or search itself failed -- no retry storm
   }
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
   {
      k.notInstalled = true;
      k.error = NotInstalled( id );
   }
   else if ( r.exitCode == 0 && !r.crashed )
   {
      k.ok = true;
      k.secret = r.out.Trimmed();
      k.found = !k.secret.IsEmpty();
   }
   else if ( r.exitCode == 1 && r.out.IsEmpty() && r.err.Trimmed().IsEmpty() )
      k.ok = true;   // no such item (genuine, or a locked keyring -- same message either way)
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
   ToolRun r;
   for ( int attempt = 1; ; ++attempt )
   {
      r = RunSecretTool( id, args, &secret );   // secret-tool reads the secret from stdin
      if ( attempt >= kMaxSecretToolAttempts || !IsSessionMismatch( r ) )
         break;
      // A retry re-runs secret-tool from scratch (fresh process, fresh DH
      // session) and `store` has replace semantics, so it is idempotent --
      // safe to repeat, and also overwrites any ghost item a failed attempt
      // may have left behind.
   }
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
   {
      k.notInstalled = true;
      k.error = NotInstalled( id );
   }
   else if ( r.exitCode == 0 && !r.crashed && !r.inputError )
      k.ok = true;
   else
      k.error = String().Format( "secret-tool store failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

KeyringExistsResult KeyringSearchExists( const KeyringId& id )
{
   KeyringExistsResult k;
   StringList args;
   args << String( "search" );   // deliberately no --unlock: never prompts, skips locked items
   args.Add( Attributes( id ) );
   const ToolRun r = RunSecretTool( id, args, nullptr );
   // secret-tool search prints "secret = <value>" in plaintext for a match --
   // r.out is therefore never logged, stored or returned, only whether it is
   // empty. The error branch below uses SearchDetail(), not Detail(): it
   // never falls back to r.out, so a partial stdout write before a genuine
   // failure still can't leak the secret into k.error.
   if ( !r.finished )
      k.error = r.error;
   else if ( r.exitCode == 127 )
   {
      k.notInstalled = true;
      k.error = NotInstalled( id );
   }
   else if ( r.exitCode == 0 && !r.crashed )
   {
      k.ok = true;
      k.exists = !r.out.Trimmed().IsEmpty();
   }
   else
      k.error = String().Format( "secret-tool search failed (exit %d): ", r.exitCode ) + SearchDetail( r );
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
   {
      k.notInstalled = true;
      k.error = NotInstalled( id );
   }
   else if ( (r.exitCode == 0 || r.exitCode == 1) && !r.crashed && FailureText( r ).IsEmpty() )
      k.ok = true;   // exit 1 without a message: nothing to clear
   else
      k.error = String().Format( "secret-tool clear failed (exit %d): ", r.exitCode ) + Detail( r );
   return k;
}

} // namespace pcl
