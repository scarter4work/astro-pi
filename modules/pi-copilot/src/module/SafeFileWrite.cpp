// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SafeFileWrite.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/File.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace pcl
{

namespace
{

bool s_failBeforeRename = false;

String Errno( int e )
{
   return String( std::strerror( e ) );
}

std::string Native( const String& path )
{
   return U8( path );
}

mode_t ModeBits( SafeFileMode mode )
{
   return mode == SafeFileMode::Private ? mode_t( 0600 ) : mode_t( 0644 );
}

// Creates <dir>/.<base>.tmp-<12 random> with O_CREAT|O_EXCL|O_NOFOLLOW and
// `mode`. The kernel applies the umask (and any default ACL) at creation, so
// the process umask is never read or changed: umask(0)/umask(m) would change
// it for EVERY thread of PixInsight for a moment (a file another thread
// created in that window would come out world-writable).
int CreateExclusiveTemp( const std::string& dir, const std::string& base, mode_t mode, std::string& temp, String& why )
{
   static const char kAlnum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
   for ( int attempt = 0; attempt < 64; ++attempt )
   {
      unsigned char rnd[ 12 ];
      ssize_t got;
      do
         got = ::getrandom( rnd, sizeof( rnd ), 0 );
      while ( got < 0 && errno == EINTR );
      if ( got != ssize_t( sizeof( rnd ) ) )
      {
         why = "no random bytes for the name: " + Errno( got < 0 ? errno : EIO );
         return -1;
      }
      std::string suffix;
      for ( unsigned char c : rnd )
         suffix += kAlnum[ c % 62 ];
      temp = dir + "/." + base + ".tmp-" + suffix;
      const int fd = ::open( temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode );
      if ( fd >= 0 )
         return fd;
      if ( errno != EEXIST )
      {
         why = Errno( errno );
         return -1;
      }
   }
   why = "64 random temporary names were all taken";
   return -1;
}

// Reads a regular file without following a link at its final component.
bool ReadNoFollow( const std::string& path, ByteArray& out, String& why )
{
   const int fd = ::open( path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC );
   if ( fd < 0 )
   {
      why = Errno( errno );
      return false;
   }
   struct stat st;
   if ( ::fstat( fd, &st ) != 0 || !S_ISREG( st.st_mode ) )
   {
      why = "not a regular file";
      ::close( fd );
      return false;
   }
   out.Clear();
   char buf[ 65536 ];
   for ( ;; )
   {
      const ssize_t n = ::read( fd, buf, sizeof( buf ) );
      if ( n < 0 )
      {
         if ( errno == EINTR )
            continue;
         why = Errno( errno );
         ::close( fd );
         return false;
      }
      if ( n == 0 )
         break;
      out.Append( reinterpret_cast<const uint8*>( buf ), reinterpret_cast<const uint8*>( buf ) + n );
   }
   ::close( fd );
   return true;
}

// Removes a private render directory (flat: the renderer writes one file,
// perhaps plus side files) without following links.
void RemovePrivateDir( const std::string& dir )
{
   if ( DIR* d = ::opendir( dir.c_str() ) )
   {
      std::vector<std::string> names;
      while ( const dirent* e = ::readdir( d ) )
         if ( std::strcmp( e->d_name, "." ) != 0 && std::strcmp( e->d_name, ".." ) != 0 )
            names.push_back( e->d_name );
      ::closedir( d );
      for ( const std::string& n : names )
         ::unlink( (dir + '/' + n).c_str() );
   }
   ::rmdir( dir.c_str() );
}

} // namespace

void SetSafeFileWriteFailBeforeRenameForSelfTest( bool on )
{
   s_failBeforeRename = on;
}

// Residual risk, accepted (review M1): this is check-then-use, not fd-pinned.
// The directory is validated once here, and callers then reopen paths under it
// by name (JourneyStore::Open lstat/open of <root>/journeys.sqlite3, SQLite's
// -wal/-shm, SafeWriteFile's temp+rename, RemoveDirectoryTree's parent open).
// O_NOFOLLOW guards only each final component, so if `dir` were swapped for a
// symlink between this check and that use, the later access would follow it.
// Only a process running as THIS SAME USER with write access to `dir`'s parent
// (e.g. ~/.local/share/PICopilot) can make that swap -- and such a process
// can already read and rewrite the user's library, key file and every other
// file of theirs directly, so pinning the fd would add no protection. Other
// users are shut out by the checks below (ours, not writable by others),
// which is the boundary that matters.
String EnsurePrivateDirectory( const String& dir )
{
   try
   {
      std::string p = Native( dir );
      while ( p.size() > 1 && p.back() == '/' )
         p.pop_back();
      if ( p.empty() || p[0] != '/' )
         return "directory path '" + dir + "' is not absolute";
      // Each missing component is made 0700. mkdir() never follows a link:
      // an existing name -- directory, file or link -- is simply EEXIST.
      for ( size_t i = 1; i <= p.size(); ++i )
         if ( i == p.size() || p[i] == '/' )
         {
            const std::string part = p.substr( 0, i );
            if ( ::mkdir( part.c_str(), 0700 ) != 0 && errno != EEXIST )
            {
               const int e = errno;
               struct stat st;
               if ( ::lstat( part.c_str(), &st ) != 0 )   // e.g. EACCES on a parent that already has it
                  return "cannot create directory " + FromU8( part ) + ": " + Errno( e );
            }
         }
      struct stat st;
      if ( ::lstat( p.c_str(), &st ) != 0 )
         return "cannot inspect directory " + dir + ": " + Errno( errno );
      if ( S_ISLNK( st.st_mode ) )
         return dir + " is a symbolic link; PI Copilot does not follow it";
      if ( !S_ISDIR( st.st_mode ) )
         return dir + " exists and is not a directory";
      if ( st.st_uid != ::geteuid() )
         return dir + " belongs to another user";
      if ( (st.st_mode & S_IWOTH) != 0 || ((st.st_mode & S_IWGRP) != 0 && st.st_gid != ::getegid()) )
         return dir + " is writable by other users";
      return String();
   }
   catch ( const pcl::Exception& x )
   {
      return "directory " + dir + ": " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return "directory " + dir + ": " + String( x.what() );
   }
}

String SafeCheckJpeg( const ByteArray& data )
{
   if ( data.Length() < 4 || data[0] != 0xFF || data[1] != 0xD8 )
      return "not a JPEG";
   return String();
}

String SafeWriteFile( const String& path, const ByteArray& data, SafeFileMode mode, const SafeFileCheck& check )
{
   try
   {
      if ( path.IsEmpty() )
         return "no output path";
      const std::string target = Native( path );

      // The target itself: absent, or an existing regular file to replace.
      struct stat st;
      if ( ::lstat( target.c_str(), &st ) == 0 )
      {
         if ( S_ISLNK( st.st_mode ) )
            return path + " is a symbolic link; refusing to write through or replace it";
         if ( !S_ISREG( st.st_mode ) )
            return path + " exists and is not a regular file; not written";
      }
      else if ( errno != ENOENT )
      {
         return "cannot inspect " + path + ": " + Errno( errno );
      }

      if ( check )
      {
         const String why = check( data );
         if ( !why.IsEmpty() )
            return path + " not written: " + why;
      }

      // Sibling temp in the same directory, so rename() is atomic. It is
      // created with the class mode; the kernel applies the umask there.
      const size_t slash = target.find_last_of( '/' );
      const std::string dir = slash == std::string::npos ? std::string( "." ) : target.substr( 0, slash );
      const std::string base = slash == std::string::npos ? target : target.substr( slash + 1 );
      std::string temp;
      String why;
      const int fd = CreateExclusiveTemp( dir, base, ModeBits( mode ), temp, why );
      if ( fd < 0 )
         return "cannot create a temporary file next to " + path + ": " + why;

      const uint8* p = data.Begin();
      size_t left = data.Length();
      while ( left > 0 && why.IsEmpty() )
      {
         const ssize_t n = ::write( fd, p, left );
         if ( n < 0 )
         {
            if ( errno != EINTR )
               why = "write failed: " + Errno( errno );
            continue;
         }
         p += n;
         left -= size_t( n );
      }
      if ( why.IsEmpty() && ::fsync( fd ) != 0 )
         why = "fsync failed: " + Errno( errno );
      if ( ::close( fd ) != 0 && why.IsEmpty() )
         why = "close failed: " + Errno( errno );
      if ( why.IsEmpty() && s_failBeforeRename )
      {
         s_failBeforeRename = false;
         why = "injected failure before rename (self-test)";
      }
      if ( why.IsEmpty() )
      {
         // Re-check right before the swap: rename() would replace a link that
         // appeared since, never follow it, but the rule is to refuse.
         if ( ::lstat( target.c_str(), &st ) == 0 && !S_ISREG( st.st_mode ) )
            why = "the target became a symbolic link or special file";
         else if ( ::rename( temp.c_str(), target.c_str() ) != 0 )
            why = "rename failed: " + Errno( errno );
      }
      if ( !why.IsEmpty() )
      {
         ::unlink( temp.c_str() );
         return path + " not written: " + why;
      }
      return String();
   }
   catch ( const pcl::Exception& x )
   {
      return path + " not written: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return path + " not written: " + String( x.what() );
   }
   catch ( ... )
   {
      return path + " not written: unknown error";
   }
}

String SafeWriteTextFile( const String& path, const std::string& utf8, SafeFileMode mode, const SafeFileCheck& check )
{
   ByteArray b( reinterpret_cast<const uint8*>( utf8.data() ), reinterpret_cast<const uint8*>( utf8.data() ) + utf8.size() );
   return SafeWriteFile( path, b, mode, check );
}

String SafeRenderFile( const String& path, SafeFileMode mode, const SafeFileRenderer& render, const SafeFileCheck& check )
{
   std::string priv;
   try
   {
      if ( path.IsEmpty() )
         return "no output path";
      std::string tmpl = U8( File::SystemTempDirectory() ) + "/picopilot-render-XXXXXX";
      std::vector<char> name( tmpl.begin(), tmpl.end() );
      name.push_back( '\0' );
      if ( ::mkdtemp( name.data() ) == nullptr )   // mode 0700, unguessable
         return path + " not written: cannot create a private render directory: " + Errno( errno );
      priv = name.data();
      const std::string target = Native( path );
      const size_t slash = target.find_last_of( '/' );
      const std::string base = slash == std::string::npos ? target : target.substr( slash + 1 );
      const std::string rendered = priv + '/' + base;

      render( FromU8( rendered ) );

      ByteArray bytes;
      String why;
      if ( !ReadNoFollow( rendered, bytes, why ) )
      {
         RemovePrivateDir( priv );
         return path + " not written: the rendered file cannot be read (" + why + ")";
      }
      RemovePrivateDir( priv );
      priv.clear();
      return SafeWriteFile( path, bytes, mode, check );
   }
   catch ( const pcl::Exception& x )
   {
      if ( !priv.empty() ) RemovePrivateDir( priv );
      return path + " not written: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      if ( !priv.empty() ) RemovePrivateDir( priv );
      return path + " not written: " + String( x.what() );
   }
   catch ( ... )
   {
      if ( !priv.empty() ) RemovePrivateDir( priv );
      return path + " not written: unknown error";
   }
}

} // namespace pcl
