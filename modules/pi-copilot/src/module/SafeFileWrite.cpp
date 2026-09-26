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

String SafeCheckJpeg( const ByteArray& data )
{
   if ( data.Length() < 4 || data[0] != 0xFF || data[1] != 0xD8 )
      return "not a JPEG";
   return String();
}

String SafeWriteFile( const String& path, const ByteArray& data, const SafeFileCheck& check )
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

      // Sibling temp in the same directory, so rename() is atomic.
      const size_t slash = target.find_last_of( '/' );
      const std::string dir = slash == std::string::npos ? std::string( "." ) : target.substr( 0, slash );
      const std::string base = slash == std::string::npos ? target : target.substr( slash + 1 );
      std::string tmpl = dir + "/." + base + ".tmp-XXXXXX";
      std::vector<char> name( tmpl.begin(), tmpl.end() );
      name.push_back( '\0' );
      const int fd = ::mkstemp( name.data() );   // O_CREAT|O_EXCL, mode 0600
      if ( fd < 0 )
         return "cannot create a temporary file next to " + path + ": " + Errno( errno );
      const std::string temp( name.data() );

      String why;
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
      if ( why.IsEmpty() )
      {
         const mode_t mask = ::umask( 0 );
         ::umask( mask );
         if ( ::fchmod( fd, 0666 & ~mask ) != 0 )
            why = "chmod failed: " + Errno( errno );
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

String SafeWriteTextFile( const String& path, const std::string& utf8, const SafeFileCheck& check )
{
   ByteArray b( reinterpret_cast<const uint8*>( utf8.data() ), reinterpret_cast<const uint8*>( utf8.data() ) + utf8.size() );
   return SafeWriteFile( path, b, check );
}

String SafeRenderFile( const String& path, const SafeFileRenderer& render, const SafeFileCheck& check )
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
      return SafeWriteFile( path, bytes, check );
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
