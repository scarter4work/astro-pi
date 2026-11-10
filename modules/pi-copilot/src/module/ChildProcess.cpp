// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ChildProcess.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace picopilot
{

namespace
{

class Fd
{
public:
   Fd() = default;
   explicit Fd( int fd ) : m_fd( fd ) {}
   ~Fd() { Close(); }
   Fd( const Fd& ) = delete;
   Fd& operator=( const Fd& ) = delete;
   int Get() const { return m_fd; }
   void Reset( int fd ) { Close(); m_fd = fd; }
   void Close() { if ( m_fd >= 0 ) ::close( m_fd ); m_fd = -1; }
private:
   int m_fd = -1;
};

class SpawnActions
{
public:
   SpawnActions() { m_ok = ::posix_spawn_file_actions_init( &m_a ) == 0; }
   ~SpawnActions() { if ( m_ok ) ::posix_spawn_file_actions_destroy( &m_a ); }
   posix_spawn_file_actions_t* Get() { return &m_a; }
   bool Ok() const { return m_ok; }
private:
   posix_spawn_file_actions_t m_a;
   bool m_ok = false;
};

class SpawnAttr
{
public:
   SpawnAttr() { m_ok = ::posix_spawnattr_init( &m_a ) == 0; }
   ~SpawnAttr() { if ( m_ok ) ::posix_spawnattr_destroy( &m_a ); }
   posix_spawnattr_t* Get() { return &m_a; }
   bool Ok() const { return m_ok; }
private:
   posix_spawnattr_t m_a;
   bool m_ok = false;
};

void SetNonBlocking( int fd )
{
   const int fl = ::fcntl( fd, F_GETFL );
   if ( fl >= 0 )
      ::fcntl( fd, F_SETFL, fl | O_NONBLOCK );
}

// Reads what is available now. Returns false at end-of-file or on a hard error.
bool Drain( int fd, std::string& into, std::size_t cap )
{
   char buf[ 16384 ];
   for ( ;; )
   {
      const ssize_t n = ::read( fd, buf, sizeof buf );
      if ( n > 0 )
      {
         if ( into.size() < cap )
            into.append( buf, std::min( std::size_t( n ), cap - into.size() ) );
         continue;   // past the cap: keep reading so the child never blocks on a full pipe
      }
      if ( n == 0 )
         return false;
      if ( errno == EINTR )
         continue;
      return errno == EAGAIN || errno == EWOULDBLOCK;
   }
}

} // namespace

ChildRun RunChild( const std::string& program, const std::vector<std::string>& args,
                   const std::string* input, int timeoutMs,
                   const std::function<void()>& poll, std::size_t maxErrBytes )
{
   ChildRun r;

   // stdout and stderr: pipes. stdin: a socket pair when there is input, so the
   // parent can write with MSG_NOSIGNAL (a child that exits early gives EPIPE,
   // never SIGPIPE); /dev/null otherwise. Every parent-side descriptor is
   // close-on-exec, and the child gets only fds 0-2 (closefrom below).
   int outP[ 2 ], errP[ 2 ];
   if ( ::pipe2( outP, O_CLOEXEC ) != 0 )
   {
      r.error = std::string( "could not create a pipe: " ) + std::strerror( errno );
      return r;
   }
   Fd outR( outP[ 0 ] ), outW( outP[ 1 ] );
   if ( ::pipe2( errP, O_CLOEXEC ) != 0 )
   {
      r.error = std::string( "could not create a pipe: " ) + std::strerror( errno );
      return r;
   }
   Fd errR( errP[ 0 ] ), errW( errP[ 1 ] );
   Fd inParent, inChild;
   if ( input != nullptr )
   {
      int sp[ 2 ];
      if ( ::socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp ) != 0 )
      {
         r.error = std::string( "could not create an input channel: " ) + std::strerror( errno );
         return r;
      }
      inParent.Reset( sp[ 0 ] );
      inChild.Reset( sp[ 1 ] );
   }
   else
   {
      inChild.Reset( ::open( "/dev/null", O_RDONLY | O_CLOEXEC ) );
      if ( inChild.Get() < 0 )
      {
         r.error = std::string( "could not open /dev/null: " ) + std::strerror( errno );
         return r;
      }
   }

   SpawnActions fa;
   SpawnAttr attr;
   if ( !fa.Ok() || !attr.Ok() )
   {
      r.error = "could not prepare to start " + program;
      return r;
   }
   ::posix_spawn_file_actions_adddup2( fa.Get(), inChild.Get(), 0 );
   ::posix_spawn_file_actions_adddup2( fa.Get(), outW.Get(), 1 );
   ::posix_spawn_file_actions_adddup2( fa.Get(), errW.Get(), 2 );
   ::posix_spawn_file_actions_addclosefrom_np( fa.Get(), 3 );

   // Clean signal state in the child: default dispositions (PixInsight may
   // ignore SIGPIPE) and nothing blocked.
   sigset_t all, none;
   sigfillset( &all );
   sigemptyset( &none );
   ::posix_spawnattr_setsigdefault( attr.Get(), &all );
   ::posix_spawnattr_setsigmask( attr.Get(), &none );
   ::posix_spawnattr_setflags( attr.Get(), POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK );

   // argv exactly as given; environment minus LD_LIBRARY_PATH.
   std::vector<char*> argv;
   argv.push_back( const_cast<char*>( program.c_str() ) );
   for ( const std::string& a : args )
      argv.push_back( const_cast<char*>( a.c_str() ) );
   argv.push_back( nullptr );
   std::vector<char*> envp;
   for ( char** e = environ; e != nullptr && *e != nullptr; ++e )
      if ( std::strncmp( *e, "LD_LIBRARY_PATH=", 16 ) != 0 )
         envp.push_back( *e );
   envp.push_back( nullptr );

   pid_t pid = -1;
   const int rc = ::posix_spawnp( &pid, program.c_str(), fa.Get(), attr.Get(), argv.data(), envp.data() );
   if ( rc != 0 )
   {
      // What /usr/bin/env reported for the same cases: 127 not found, 126 found but not runnable.
      if ( rc == ENOENT || rc == ENOTDIR )
      {
         r.finished = true;
         r.exitCode = 127;
      }
      else if ( rc == EACCES || rc == ENOEXEC || rc == EPERM )
      {
         r.finished = true;
         r.exitCode = 126;
      }
      else
         r.error = "could not start " + program + ": " + std::strerror( rc );
      return r;
   }
   r.started = true;
   inChild.Close();
   outW.Close();
   errW.Close();
   SetNonBlocking( outR.Get() );
   SetNonBlocking( errR.Get() );

   std::size_t written = 0;
   if ( input != nullptr )
   {
      SetNonBlocking( inParent.Get() );
      if ( input->empty() )
         inParent.Close();
   }

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeoutMs );
   bool outOpen = true, errOpen = true, reaped = false;
   int status = 0;
   while ( !reaped )
   {
      // Feed stdin as far as the child takes it; EOF once it is all written.
      if ( inParent.Get() >= 0 )
      {
         while ( written < input->size() )
         {
            const ssize_t n = ::send( inParent.Get(), input->data() + written, input->size() - written, MSG_NOSIGNAL );
            if ( n > 0 ) { written += std::size_t( n ); continue; }
            if ( n < 0 && errno == EINTR ) continue;
            if ( n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ) break;
            r.inputError = true;   // EPIPE / ECONNRESET: the child is gone or closed stdin
            break;
         }
         if ( written >= input->size() || r.inputError )
            inParent.Close();
      }

      pollfd fds[ 3 ];
      nfds_t n = 0;
      if ( outOpen ) fds[ n++ ] = { outR.Get(), POLLIN, 0 };
      if ( errOpen ) fds[ n++ ] = { errR.Get(), POLLIN, 0 };
      if ( inParent.Get() >= 0 ) fds[ n++ ] = { inParent.Get(), POLLOUT, 0 };
      if ( n > 0 )
         ::poll( fds, n, 20 );
      else
         ::usleep( 20000 );
      if ( outOpen ) outOpen = Drain( outR.Get(), r.out, std::string::npos );
      if ( errOpen ) errOpen = Drain( errR.Get(), r.err, maxErrBytes );

      const pid_t w = ::waitpid( pid, &status, WNOHANG );
      if ( w == pid )
      {
         reaped = true;
         break;
      }
      if ( w < 0 && errno != EINTR )
      {
         // Reaped elsewhere (SIGCHLD ignored): the exit status is unknowable,
         // so this is not reported as a finished run.
         r.error = std::string( "could not wait for " ) + program + ": " + std::strerror( errno );
         return r;
      }
      if ( std::chrono::steady_clock::now() >= deadline )
      {
         ::kill( pid, SIGKILL );
         while ( ::waitpid( pid, &status, 0 ) < 0 && errno == EINTR ) {}
         r.timedOut = true;
         r.error = "timed out";
         return r;
      }
      if ( poll )
         poll();
   }

   // The child has exited: take what is left in the pipes (non-blocking, so a
   // grandchild still holding them open cannot stall this).
   if ( outOpen ) Drain( outR.Get(), r.out, std::string::npos );
   if ( errOpen ) Drain( errR.Get(), r.err, maxErrBytes );
   if ( input != nullptr && written < input->size() )
      r.inputError = true;

   r.finished = true;
   if ( WIFEXITED( status ) )
      r.exitCode = WEXITSTATUS( status );
   else if ( WIFSIGNALED( status ) )
   {
      r.crashed = true;
      r.exitCode = 128 + WTERMSIG( status );
   }
   return r;
}

} // namespace picopilot
