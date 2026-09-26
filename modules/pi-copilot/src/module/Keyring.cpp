// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "Keyring.h"
#include "PICopilotModule.h"
#include "SafeFileWrite.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/ExternalProcess.h>
#include <pcl/StringList.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

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

// Removes the private scratch directory a RunSecretTool() call made for the
// child's stderr file, on every exit path (normal return, an early return, or
// an exception unwinding through RunSecretTool) -- same discipline as
// WaitNotice above.
class ScratchDirGuard
{
public:
   explicit ScratchDirGuard( String dir ) : m_dir( std::move( dir ) ) {}
   ~ScratchDirGuard() { RemovePrivateScratchDir( m_dir ); }
   ScratchDirGuard( const ScratchDirGuard& ) = delete;
   ScratchDirGuard& operator=( const ScratchDirGuard& ) = delete;
private:
   String m_dir;
};

IsoString Bytes( const ByteArray& b )
{
   IsoString s;
   if ( !b.IsEmpty() )
      s.Append( reinterpret_cast<const char*>( b.Begin() ), b.Length() );
   return s;
}

// Measured (review, confirmed independently: PI core 1.9.5 "Lockhart" build
// 1702 / PCL headers 2.10.8, PCL source ExternalProcess.cpp:304-336): the
// child's real stderr bytes come back through what pcl::ExternalProcess
// reports as StandardOutput(); StandardError() is always empty. This holds
// in BOTH --automation-mode and a normal interactive GUI session (the
// original finding here understated it as automation-only). The core reads
// the child's stderr on its own pipe and interleaves it into the stdout
// buffer at write granularity -- confirmed from the child side: fd 1 and fd
// 2 are different pipes, so it is not a spawn-time dup2 merge. Also:
// ExternalProcess::RedirectStandardError() is a documented but silent no-op
// (the file is never created, fd 2 stays a pipe) -- so PCL itself offers no
// way to keep the two channels apart.
//
// The fix here does not read either of PCL's channels for diagnostic text:
// it makes the CHILD redirect its own real fd 2 to a private file before it
// execs secret-tool, via a `/bin/sh -c` wrapper. Nothing variable is ever
// interpolated into the wrapper's script text -- it is one fixed literal,
// and `id.program`, every element of `args`, and the private error-file path
// travel as ordinary positional shell parameters ($0.."$@"), read back only
// by the fixed names ERR/PROG the script itself assigns. After the child
// exits, the file (which only this process's own, freshly mkdtemp()'d,
// 0700 directory could ever contain) is read back and removed. r.out is then
// always pure stdout, and r.err is always the real, separate stderr: no
// caller needs to guess which channel a message landed in any more.
ToolRun RunSecretTool( const KeyringId& id, const StringList& args, const IsoString* input )
{
   ToolRun r;
   WaitNotice notice;

   String scratchDir;
   String scratchWhy;
   if ( !CreatePrivateScratchDir( scratchDir, scratchWhy ) )
   {
      r.error = "could not create a private directory for secret-tool's stderr: " + scratchWhy;
      return r;
   }
   ScratchDirGuard scratchGuard( scratchDir );   // removed on every path below, including an exception
   const String errPath = scratchDir + "/stderr";

   try
   {
      // Created here -- not left to the child's "2>" (a plain open() with
      // O_CREAT|O_TRUNC, no O_EXCL/O_NOFOLLOW) -- so a pre-existing name or
      // a symbolic link at this exact path is refused even though scratchDir
      // is freshly made, unguessable (mkdtemp) and 0700. Defence in depth:
      // the same posture SafeFileWrite.cpp uses for its own temp files.
      const std::string errNative = U8( errPath );
      const int errFd = ::open( errNative.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600 );
      if ( errFd < 0 )
      {
         r.error = "could not create a private stderr file for secret-tool: " + String( std::strerror( errno ) );
         return r;
      }
      ::close( errFd );   // the child writes into it by path (via the shell's "2>"), not this fd

      ExternalProcess p;
      StringList a;
      // sh -c SCRIPT sh <errPath> <program> <args...>
      // $0 = "sh" (placeholder; exec never reads it), $1 = errPath, then
      // (after two `shift`s) PROG = program and "$@" = args, exactly as
      // received -- so nothing here is ever re-parsed as shell syntax.
      // `exec PROG "$@" 2>"$ERR"` sets up the redirection on the exec'd
      // command, before replacing the shell's own process image, so the
      // final program (via /usr/bin/env, unchanged from before) inherits fd
      // 2 already pointing at the private file.
      a << String( "-c" )
        << String( "ERR=$1; shift; PROG=$1; shift; exec /usr/bin/env -u LD_LIBRARY_PATH \"$PROG\" \"$@\" 2>\"$ERR\"" )
        << String( "sh" ) << errPath << id.program;
      for ( const String& s : args )
         a << s;
      p.Start( "/bin/sh", a );
      if ( !p.WaitForStarted( 10000 ) )
      {
         r.error = "could not start /bin/sh to run " + id.program;
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
      r.out = Bytes( p.StandardOutput() );   // pure stdout: the child's real stderr never reaches this pipe
      ByteArray errBytes;
      String readWhy;
      if ( ReadFileNoFollow( errPath, errBytes, readWhy ) )
         r.err = Bytes( errBytes );
      // else: nothing to read (the file was never written to, or the child
      // was killed before opening it) -- r.err stays empty, which is correct.
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
   // True only when the LAST completed disambiguation (the search run before
   // the most recent retry) found the item: if every lookup attempt then
   // still silently misses and the bound is hit, that is not a genuine "no
   // key" -- the item is there but could not be read this time (M2).
   bool lastSearchFoundIt = false;
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
