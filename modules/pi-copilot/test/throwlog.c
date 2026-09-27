// PI Copilot self-test harness -- optional exception-logging shim (plan item
// 4, thist-hang-investigation.md). LD_PRELOADed into PixInsight ONLY when
// PICOPILOT_THROWLOG=1 (see run-selftest.sh, which builds this from source
// into the run's private TMPDIR -- never commit a prebuilt .so).
//
// It logs a backtrace for every thrown std::bad_alloc-family C++ exception,
// then rethrows via the real __cxa_throw so PI's own exception handling is
// unaffected. This exists because the T-hist 900s hangs were traced to a
// PixInsight-core modal "Out of memory" MessageBox raised from inside a
// caught exception (pi::PixInsightApplication::notify), and the actual throw
// site was never identified (see the investigation doc's "Recommended fixes"
// #4): keeping this shim available lets the next occurrence be caught with a
// stack instead of only the effect (the modal / the watchdog's kill).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

struct type_info_ { void* vt; const char* name; };
typedef void (*throw_fn)(void*, void*, void (*)(void*));
static throw_fn real_throw;

void __cxa_throw( void* obj, void* tinfo, void (*dest)( void* ) )
{
   if ( !real_throw )
      real_throw = (throw_fn)dlsym( RTLD_NEXT, "__cxa_throw" );
   const char* n = tinfo ? ((struct type_info_*)tinfo)->name : "";
   if ( n && (strstr( n, "bad_alloc" ) || strstr( n, "bad_array" )) )
   {
      const char* path = getenv( "THROWLOG_FILE" );
      int fd = path ? open( path, O_WRONLY | O_CREAT | O_APPEND, 0644 ) : 2;
      if ( fd >= 0 )
      {
         char hdr[256];
         struct timespec ts;
         clock_gettime( CLOCK_REALTIME, &ts );
         int k = snprintf( hdr, sizeof hdr, "\n===== %s thrown t=%ld.%03ld tid=%ld pid=%d =====\n",
                            n, (long)ts.tv_sec, ts.tv_nsec / 1000000, (long)syscall( SYS_gettid ), getpid() );
         write( fd, hdr, k );
         void* bt[64];
         int c = backtrace( bt, 64 );
         backtrace_symbols_fd( bt, c, fd );
         if ( fd != 2 )
            close( fd );
      }
   }
   real_throw( obj, tinfo, dest );
   __builtin_unreachable();
}
