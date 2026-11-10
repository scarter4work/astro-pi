// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "Keyring.h"
#include "ChildProcess.h"
#include "PICopilotModule.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/StringList.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

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
   bool      inputError = false; // stdin could not be fully written (child already gone)
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

// review m3: secret-tool's stderr is kept to this many bytes (Detail() only
// ever shows 200 characters of it). secret-tool's own stderr is always tiny in
// practice; this is a hard ceiling against a pathological program under
// KeyringId, not something real secret-tool would ever approach.
constexpr size_t kMaxStderrBytes = 64 * 1024;

// secret-tool runs directly: picopilot::RunChild() (ChildProcess.cpp) starts it
// with posix_spawnp and an argv array -- no shell and no other interpreter in
// between (CPD rule 5.3) -- with the caller's environment minus LD_LIBRARY_PATH
// (PixInsight's own library path would otherwise be inherited and can break
// system binaries). The secret travels only on the child's stdin. stdout and
// stderr come back on two separate pipes, so r.out is always pure stdout and
// r.err always the real, separate stderr: no caller needs to guess which
// channel a message landed in. (pcl::ExternalProcess cannot do that: in
// PixInsight 1.9.5 it reports the child's stderr through StandardOutput() and
// RedirectStandardError() is a silent no-op -- which is why this used to go
// through a /bin/sh wrapper that redirected fd 2 to a private file.)
// Event pumping (user input excluded) and the wait notice run while it waits.
ToolRun RunSecretTool( const KeyringId& id, const StringList& args, const IsoString* input )
{
   ToolRun r;
   WaitNotice notice;
   try
   {
      std::vector<std::string> argv;
      for ( const String& s : args )
         argv.push_back( U8( s ) );
      std::string in;
      if ( input != nullptr )
         in.assign( input->c_str(), input->Length() );

      const picopilot::ChildRun c = picopilot::RunChild(
         U8( id.program ), argv, input != nullptr ? &in : nullptr, PICopilotKeyringTimeoutMs,
         [&notice]() {
            notice.Poll();
            ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
         },
         kMaxStderrBytes );
      // The secret's only copy outside the caller's buffer: wipe it.
      if ( !in.empty() )
         std::fill( in.begin(), in.end(), '\0' );

      if ( c.timedOut )
      {
         r.error = id.program + String().Format( " did not finish within %d s (is the keyring locked, with its "
                                                 "unlock prompt hidden?)", PICopilotKeyringTimeoutMs/1000 );
         return r;
      }
      if ( !c.finished )
      {
         r.error = "could not run " + id.program + ": " + FromU8( c.error );
         return r;
      }
      r.finished = true;
      r.exitCode = c.exitCode;
      r.crashed = c.crashed;
      r.inputError = c.inputError;
      if ( !c.out.empty() )
         AppendBytes( r.out, c.out.data(), c.out.size() );
      if ( !c.err.empty() )
         AppendBytes( r.err, c.err.data(), c.err.size() );
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

// Bounded to 200 CHARACTERS (not bytes): decode stderr as UTF-8 first (it can
// be locale-translated), then truncate the decoded text, so a truncation
// point can never fall inside a multi-byte sequence and mangle it. Plain
// String( IsoString ) (the previous implementation) widens bytes as
// ISO-8859-1, which corrupts any non-ASCII UTF-8 byte regardless of length.
// r.err is now always the real, separate stderr (see RunSecretTool's
// comment) -- this never reads r.out, so it can never surface a secret that
// a lookup or search wrote there before an abnormal exit.
String Detail( const ToolRun& r )
{
   const IsoString e = r.err.Trimmed();
   if ( e.IsEmpty() )
      return String( "(no message)" );
   String d = FromU8( std::string( e.c_str(), e.Length() ) );
   if ( d.Length() > 200 )
      d = d.Left( 200 );
   if ( r.inputError )
      d += " (it exited before reading its input)";
   return d;
}

// Names the exit/crash even when there is no message text, so a failure is
// never reported as bare, silent "(no message)" with nothing else to go on.
String ExitDescription( const ToolRun& r )
{
   String d = String().Format( "exit %d", r.exitCode );
   if ( r.crashed )
      d += ", crashed";
   return d;
}

// The proven signature (keyring-flake-investigation.md): ksecretd cannot
// decrypt the secret because the DH shared-secret padding mismatched, and
// reports the misleading D-Bus error below. Matched narrowly (both
// fragments) so an unrelated "session" error is never retried.
bool IsSessionMismatch( const ToolRun& r )
{
   return r.finished && !r.crashed && r.exitCode == 1
       && r.err.Contains( "Can't find session" )
       && r.err.Contains( "/org/freedesktop/secrets/session/" );
}

// The other half of the same proven signature: ksecretd returns the secret
// re-encrypted under a key libsecret can't derive back; libsecret's
// decrypt/unpad fails silently and secret-tool exits 1 with nothing on
// either stream -- byte-identical to a genuine "no such item".
bool IsSilentMiss( const ToolRun& r )
{
   return r.finished && !r.crashed && r.exitCode == 1 && r.out.IsEmpty() && r.err.Trimmed().IsEmpty();
}

// Exit 127: the program could not be found (RunChild reports it as /usr/bin/env did).
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
   g_waitNotifier = std::move( m_previous );   // pcl-move-ok: std::function, the guard's last use
}

KeyringResult KeyringLookup( const KeyringId& id )
{
   KeyringResult k;
   StringList args;
   args << String( "lookup" );
   args.Add( Attributes( id ) );
   ToolRun r;
   // True only when the LAST completed disambiguation (the search run before
   // the most recent retry) found the item: if every lookup attempt then
   // still silently misses and the bound is hit, that is not a genuine "no
   // key" -- the item is there but could not be read this time (M2).
   bool lastSearchFoundIt = false;
   for ( int attempt = 1; ; ++attempt )
   {
      r = RunSecretTool( id, args, nullptr );
      if ( attempt >= PICopilotKeyringMaxAttempts || !IsSilentMiss( r ) )
         break;
      // Ambiguous: a silent miss is indistinguishable from a genuine "no
      // such item" (see KeyringResult). Disambiguate with a non-prompting
      // search before deciding whether to retry -- a locked keyring, whose
      // items `search` skips without --unlock, still ends up "not found"
      // here, same as today.
      const KeyringExistsResult ex = KeyringSearchExists( id );
      if ( !(ex.ok && ex.exists) )
      {
         lastSearchFoundIt = false;
         break;   // genuine miss, locked, or search itself failed -- no retry storm
      }
      lastSearchFoundIt = true;
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
   {
      k.ok = true;   // no such item (genuine, or a locked keyring -- same message either way)
      k.existsButUnreadable = lastSearchFoundIt;
   }
   else
      k.error = "secret-tool lookup failed (" + ExitDescription( r ) + "): " + Detail( r );
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
      if ( attempt >= PICopilotKeyringMaxAttempts || !IsSessionMismatch( r ) )
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
      k.error = "secret-tool store failed (" + ExitDescription( r ) + "): " + Detail( r );
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
   // empty. r.err is now always the real, separate stderr (RunSecretTool),
   // so Detail() below can never pick up a secret that search wrote to
   // stdout.
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
      k.error = "secret-tool search failed (" + ExitDescription( r ) + "): " + Detail( r );
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
   else if ( (r.exitCode == 0 || r.exitCode == 1) && !r.crashed && r.err.Trimmed().IsEmpty() )
      k.ok = true;   // exit 1 without a message: nothing to clear
   else
      k.error = "secret-tool clear failed (" + ExitDescription( r ) + "): " + Detail( r );
   return k;
}

} // namespace pcl
