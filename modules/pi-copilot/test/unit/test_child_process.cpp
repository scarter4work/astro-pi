// Unit tests for picopilot::RunChild (src/module/ChildProcess.cpp): the way
// Keyring.cpp runs secret-tool -- argv exec, no shell, the secret on stdin,
// stdout and stderr kept apart. PCL-free; runs without PixInsight.

#include "../../src/module/ChildProcess.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using picopilot::ChildRun;
using picopilot::RunChild;

static int g_failures = 0;
#define CHECK( cond ) do { if ( !(cond) ) { ++g_failures; \
   std::fprintf( stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while ( 0 )

namespace
{
fs::path g_dir;

std::string Script( const char* name, const std::string& body )
{
   const fs::path p = g_dir / name;
   std::ofstream( p ) << "#!/bin/sh\n" << body << "\n";
   fs::permissions( p, fs::perms::owner_all );
   return p.string();
}

const auto noPoll = [] {};
constexpr std::size_t kCap = 64 * 1024;

// Every shell metacharacter, quoting form and expansion a shell would act on.
// If any layer between us and the program were a shell, one of these would
// create the canary or alter the bytes.
std::string Hostile( const fs::path& canary )
{
   const std::string c = canary.string();
   return "sk-ant-'\"; touch " + c + "; $(touch " + c + ") `touch " + c + "` | tee " + c +
          " && touch " + c + " || touch " + c + " > " + c + " < /dev/null & * ? [a] ~ # \\ $HOME ${PATH} !! \t%s%n end";
}
} // namespace

int main()
{
   std::signal( SIGPIPE, SIG_DFL );   // a SIGPIPE in RunChild would kill this test
   g_dir = fs::temp_directory_path() / ("picopilot_child_test_" + std::to_string( ::getpid() ));
   fs::remove_all( g_dir );
   fs::create_directories( g_dir );
   const fs::path canary = g_dir / "canary";

   // 1. A secret full of shell syntax reaches the program's stdin byte for byte.
   {
      const std::string secret = Hostile( canary ) + "\nsecond line\n";
      const ChildRun r = RunChild( "cat", {}, &secret, 5000, noPoll, kCap );
      CHECK( r.started && r.finished && r.exitCode == 0 && !r.crashed && !r.inputError );
      CHECK( r.out == secret );
      CHECK( r.err.empty() );
      CHECK( !fs::exists( canary ) );
   }

   // 2. Arguments full of shell syntax arrive as single, unaltered argv entries.
   {
      const std::string arg = Hostile( canary );
      const ChildRun r = RunChild( "printf", { "[%s]\\n", arg, "two words" }, nullptr, 5000, noPoll, kCap );
      CHECK( r.finished && r.exitCode == 0 );
      CHECK( r.out == "[" + arg + "]\n[two words]\n" );
      CHECK( !fs::exists( canary ) );
   }

   // 3. stdout and stderr never mix.
   {
      const ChildRun r = RunChild( Script( "both", "echo to-out; echo to-err >&2; echo more-out; exit 0" ),
                                   {}, nullptr, 5000, noPoll, kCap );
      CHECK( r.finished && r.exitCode == 0 );
      CHECK( r.out == "to-out\nmore-out\n" );
      CHECK( r.err == "to-err\n" );
   }

   // 4. Environment: LD_LIBRARY_PATH removed, the rest (PATH, other variables) kept.
   {
      ::setenv( "LD_LIBRARY_PATH", "/opt/PixInsight/bin/lib", 1 );
      ::setenv( "PICOPILOT_CHILD_TEST", "kept", 1 );
      const ChildRun r = RunChild( "env", {}, nullptr, 5000, noPoll, kCap );
      CHECK( r.finished && r.exitCode == 0 );
      CHECK( r.out.find( "LD_LIBRARY_PATH=" ) == std::string::npos );
      CHECK( r.out.find( "PICOPILOT_CHILD_TEST=kept\n" ) != std::string::npos );
      CHECK( r.out.find( "PATH=" ) != std::string::npos );
      ::unsetenv( "LD_LIBRARY_PATH" );
      ::unsetenv( "PICOPILOT_CHILD_TEST" );
   }

   // 5. Not installed: exit 127, like /usr/bin/env -- by path and by PATH search.
   {
      const ChildRun a = RunChild( "/nonexistent/secret-tool", { "lookup" }, nullptr, 5000, noPoll, kCap );
      CHECK( a.finished && a.exitCode == 127 && !a.started );
      const ChildRun b = RunChild( "picopilot-no-such-program-xyz", {}, nullptr, 5000, noPoll, kCap );
      CHECK( b.finished && b.exitCode == 127 );
   }

   // 6. Not executable: exit 126, like /usr/bin/env.
   {
      const fs::path p = g_dir / "noexec";
      std::ofstream( p ) << "#!/bin/sh\nexit 0\n";
      fs::permissions( p, fs::perms::owner_read | fs::perms::owner_write );
      const ChildRun r = RunChild( p.string(), {}, nullptr, 5000, noPoll, kCap );
      CHECK( r.finished && r.exitCode == 126 );
   }

   // 7. A child that exits without reading a large input: its own exit code,
   //    inputError set, and no SIGPIPE (this process would have died).
   {
      const std::string big( 4 * 1024 * 1024, 'x' );
      const ChildRun r = RunChild( Script( "early", "exit 3" ), {}, &big, 5000, noPoll, kCap );
      CHECK( r.finished && r.exitCode == 3 && !r.crashed );
      CHECK( r.inputError );
   }

   // 8. Crash: reported as crashed, 128+signal.
   {
      const ChildRun r = RunChild( Script( "crash", "kill -SEGV $$" ), {}, nullptr, 5000, noPoll, kCap );
      CHECK( r.finished && r.crashed && r.exitCode == 128 + SIGSEGV );
   }

   // 9. Timeout: killed, reaped, not finished, poll called meanwhile.
   {
      int polls = 0;
      const ChildRun r = RunChild( "sleep", { "5" }, nullptr, 300, [&polls] { ++polls; }, kCap );
      CHECK( !r.finished && r.timedOut && !r.error.empty() );
      CHECK( polls >= 5 );
      CHECK( ::waitpid( -1, nullptr, WNOHANG ) == -1 );   // nothing left unreaped
   }

   // 10. stderr is bounded; stdout is not.
   {
      const ChildRun r = RunChild( Script( "noisy", "head -c 200000 /dev/zero | tr '\\0' e >&2; "
                                                    "head -c 300000 /dev/zero | tr '\\0' o" ),
                                   {}, nullptr, 5000, noPoll, 1024 );
      CHECK( r.finished && r.exitCode == 0 );
      CHECK( r.err.size() == 1024 );
      CHECK( r.out.size() == 300000 );
   }

   // 11. No input: stdin is at end-of-file, so a reader does not hang.
   {
      const ChildRun r = RunChild( "cat", {}, nullptr, 2000, noPoll, kCap );
      CHECK( r.finished && r.exitCode == 0 && r.out.empty() && !r.timedOut );
   }

   // 12. No descriptor of ours leaks into the child: it sees only 0, 1, 2
   //     (plus the one ls opens to read the directory).
   {
      const ChildRun r = RunChild( "ls", { "/proc/self/fd" }, nullptr, 5000, noPoll, kCap );
      CHECK( r.finished && r.exitCode == 0 );
      int n = 0;
      for ( char ch : r.out ) if ( ch == '\n' ) ++n;
      CHECK( n <= 4 );
   }

   fs::remove_all( g_dir );
   if ( g_failures == 0 )
      std::printf( "test_child_process: all checks passed\n" );
   return g_failures == 0 ? 0 : 1;
}
