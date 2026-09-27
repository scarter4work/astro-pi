// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_GraXpertSelfTestLock_h
#define PICopilot_GraXpertSelfTestLock_h

// Self-test only (shared by B10/B10b and J10's GraXpert measurement).

#include <pcl/String.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pcl
{

// Serializes the self-test's runs of PixInsight's GraXpert core process (real
// program or stand-in) across CONCURRENT PixInsight instances (Task
// T-graxpert). The core bridges to the external program through FIXED shared
// temp files (/tmp/PixInsight.xisf, /tmp/PixInsight_GraXpert.xisf), so two
// test slots running it at once clobber each other (proven: ok with pixels
// unchanged or blended). One fixed per-user lock file,
// /tmp/picopilot-<uid>/graxpert-selftest.lock, flock(2)ed exclusively. The
// directory is created 0700 and must be a real directory owned by this user
// with no group/other access (lstat: a planted symlink or foreign directory
// is refused); the file is opened O_NOFOLLOW. Bounded wait, reported.
class GraXpertCoreSelfTestLock
{
public:

   static String Dir()
   {
      return String().Format( "/tmp/picopilot-%u", unsigned( ::getuid() ) );
   }

   static String Path()
   {
      return Dir() + "/graxpert-selftest.lock";
   }

   explicit GraXpertCoreSelfTestLock( double timeoutS )
   {
      const auto t0 = std::chrono::steady_clock::now();
      const IsoString dir = Dir().ToUTF8(), path = Path().ToUTF8();
      if ( ::mkdir( dir.c_str(), 0700 ) != 0 && errno != EEXIST )
      {
         m_error = "cannot create " + Dir() + ": " + String( ::strerror( errno ) );
         return;
      }
      struct stat st;
      if ( ::lstat( dir.c_str(), &st ) != 0 || !S_ISDIR( st.st_mode ) || st.st_uid != ::getuid()
        || (st.st_mode & 077) != 0 )
      {
         m_error = Dir() + " is not a private directory of this user (not a real directory, a link, foreign-owned, "
                           "or group/other-accessible); refusing to use it";
         return;
      }
      m_fd = ::open( path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600 );
      if ( m_fd < 0 )
      {
         m_error = "cannot open " + Path() + ": " + String( ::strerror( errno ) );
         return;
      }
      for ( ;; )
      {
         if ( ::flock( m_fd, LOCK_EX | LOCK_NB ) == 0 )
         {
            m_locked = true;
            break;
         }
         if ( errno != EWOULDBLOCK && errno != EINTR )
         {
            m_error = "flock " + Path() + ": " + String( ::strerror( errno ) );
            break;
         }
         if ( std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() > timeoutS )
         {
            m_error = "another PixInsight test instance held " + Path()
                    + String().Format( " for more than %.0f s", timeoutS );
            break;
         }
         ::usleep( 100000 );
      }
      m_waitedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
   }

   ~GraXpertCoreSelfTestLock()
   {
      if ( m_fd >= 0 )
      {
         if ( m_locked )
            ::flock( m_fd, LOCK_UN );
         ::close( m_fd );
      }
   }

   GraXpertCoreSelfTestLock( const GraXpertCoreSelfTestLock& ) = delete;
   GraXpertCoreSelfTestLock& operator =( const GraXpertCoreSelfTestLock& ) = delete;

   bool Locked() const { return m_locked; }
   double WaitedMs() const { return m_waitedMs; }
   const String& Error() const { return m_error; }

private:

   int    m_fd = -1;
   bool   m_locked = false;
   double m_waitedMs = 0;
   String m_error;
};

} // namespace pcl

#endif // PICopilot_GraXpertSelfTestLock_h
