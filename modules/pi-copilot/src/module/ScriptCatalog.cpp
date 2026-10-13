// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ScriptCatalog.h"
#include "ModuleScript.h"
#include "TextSafety.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/GlobalSettings.h>
#include <pcl/Thread.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sys/stat.h>

namespace pcl
{

namespace
{

std::string RealPath8( const std::string& p )
{
   char* r = ::realpath( p.c_str(), nullptr );
   if ( r == nullptr )
      return std::string();
   std::string s( r );
   ::free( r );
   return s;
}

bool IsInside( const std::string& path, const std::string& root )
{
   return path.size() > root.size() + 1 && path.compare( 0, root.size(), root ) == 0 && path[root.size()] == '/';
}

bool EndsWith( const std::string& s, const char* suffix )
{
   const size_t n = std::strlen( suffix );
   return s.size() >= n && s.compare( s.size() - n, n, suffix ) == 0;
}

std::string Trim( const std::string& s )
{
   size_t a = 0, b = s.size();
   while ( a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r') )
      ++a;
   while ( b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r') )
      --b;
   return s.substr( a, b - a );
}

// A path with a character that must not appear raw in a confirmation dialog
// or a message (control, bidi, invisible): the reason, else "".
std::string UnsafePathChar( const String& p )
{
   for ( size_type i = 0; i < p.Length(); )
   {
      size_type units = 1;
      const uint32 c = CodePointAt( p, i, units );
      if ( c == 0 )
         return "the path contains a NUL character";
      if ( c != ' ' && UnsafeDisplayKind( c ) != nullptr )
         return U8( "the path contains " + DisplayCharacter( c ) );
      i += units;
   }
   return std::string();
}

struct Stamp
{
   long long size = -1, mtimeNs = 0;
   bool operator ==( const Stamp& x ) const { return size == x.size && mtimeNs == x.mtimeNs; }
};

Stamp StampOf( const std::string& p )
{
   Stamp s;
   struct stat st;
   if ( ::stat( p.c_str(), &st ) == 0 )
   {
      s.size = (long long)st.st_size;
      s.mtimeNs = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
   }
   return s;
}

bool IsRegularFile( const std::string& p )
{
   struct stat st;
   return ::stat( p.c_str(), &st ) == 0 && S_ISREG( st.st_mode );
}

struct Header
{
   bool        featured = false;
   std::string featureId, menu;
};

// The #feature-id directive of a main script (first 4 MB at most).
Header ReadHeader( const std::string& path )
{
   Header h;
   std::ifstream f( path, std::ios::binary );
   std::string line;
   size_t read = 0;
   while ( read < (4u << 20) && std::getline( f, line ) )
   {
      read += line.size() + 1;
      const std::string t = Trim( line );
      if ( t.compare( 0, 11, "#feature-id" ) == 0 && (t.size() == 11 || t[11] == ' ' || t[11] == '\t') )
      {
         const std::string v = U8( FromU8( Trim( t.substr( 11 ) ) ) );
         const size_t colon = v.find( ':' );
         if ( colon != std::string::npos )
         {
            h.featureId = Trim( v.substr( 0, colon ) );
            h.menu = Trim( v.substr( colon + 1 ) );
         }
         else
            h.menu = v;
         h.featured = true;
         break;
      }
   }
   return h;
}

std::string MenuCategory( const std::string& menu )
{
   const size_t gt = menu.find( '>' );
   return Trim( gt == std::string::npos ? menu : menu.substr( 0, gt ) );
}

struct SigResult
{
   bool        valid = false;
   bool        certified = false;
   std::string developerId, developerName, scriptId, timestamp, error;
};

// PixInsight's own verification, for several real paths at once, through the
// module's fixed script: the paths enter only as one JSON data literal.
std::vector<SigResult> VerifySignatures( const std::vector<std::string>& paths )
{
   std::vector<SigResult> r( paths.size() );
   if ( paths.empty() )
      return r;
   nlohmann::json list = nlohmann::json::array();
   for ( const std::string& p : paths )
      list.push_back( p );
   const nlohmann::json j = EvaluateAsciiJson(
      "var paths = JSON.parse( " + ScriptLiteral( FromU8( list.dump() ) ) + " ), devs = {}, out = [];"
      " var cd = Security.certifiedDevelopers();"
      " for ( var i = 0; i < cd.length; ++i ) devs[\"#\" + cd[i].id] = String( cd[i].name || \"\" );"
      " for ( var i = 0; i < paths.length; ++i ) { var o = {};"
      "  try { var s = Security.getScriptSignature( paths[i] );"
      "   o.valid = s.valid === true; o.developerId = pcWell( String( s.developerId || \"\" ) );"
      "   o.scriptId = pcWell( String( s.scriptId || \"\" ) ); o.timestamp = pcWell( String( s.timestamp || \"\" ) );"
      "   o.certified = o.developerId.length > 0 && devs.hasOwnProperty( \"#\" + o.developerId );"
      "   o.developerName = o.certified ? pcWell( devs[\"#\" + o.developerId] ) : \"\"; }"
      "  catch ( e ) { o.valid = false; o.error = pcWell( pcCut( String( e ), 300 ) ); }"
      "  out.push( o ); }"
      " return pcAscii( JSON.stringify( out ) );" );
   if ( !j.is_array() || j.size() != paths.size() )
      throw Error( "the signature check returned an unexpected result" );
   for ( size_t i = 0; i < paths.size(); ++i )
   {
      const nlohmann::json& o = j[i];
      r[i].valid = o.value( "valid", false );
      r[i].certified = o.value( "certified", false );
      r[i].developerId = o.value( "developerId", std::string() );
      r[i].developerName = o.value( "developerName", std::string() );
      r[i].scriptId = o.value( "scriptId", std::string() );
      r[i].timestamp = o.value( "timestamp", std::string() );
      r[i].error = o.value( "error", std::string() );
   }
   return r;
}

// Why a verified signature does not make the file eligible; "" when it does.
std::string SignatureProblem( const SigResult& s )
{
   if ( !s.error.empty() )
      return "its signature could not be verified: " + s.error;
   if ( !s.valid )
      return "its signature does not match the file (the script, or a file it includes, was changed after signing)";
   if ( s.developerId.empty() )
      return "its signature names no developer";
   if ( !s.certified )
      return "it is signed by '" + s.developerId + "', who is not in PixInsight's database of certified developers";
   if ( s.scriptId.empty() )
      return "its signature names no script id";
   return std::string();
}

// Rules 1-2 for a given path (not the signature). On success `real` is the
// real path and `h` its header; else the reason.
std::string PathProblem( const String& given, const std::string& root, std::string& real, Header& h )
{
   if ( given.IsEmpty() )
      return "no path";
   const std::string unsafe = UnsafePathChar( given );
   if ( !unsafe.empty() )
      return unsafe;
   const std::string g = U8( given );
   if ( g[0] != '/' )
      return "the path is not absolute";
   if ( g.find( "/../" ) != std::string::npos || EndsWith( g, "/.." ) || g.find( "/./" ) != std::string::npos )
      return "the path contains '..' or '.' components";
   if ( !EndsWith( g, ".js" ) )
      return "not a .js script";
   real = RealPath8( g );
   if ( real.empty() )
      return "the file does not exist";
   if ( !IsInside( real, root ) )
   {
      struct stat st;
      const bool link = ::lstat( g.c_str(), &st ) == 0 && S_ISLNK( st.st_mode );
      return link ? "it is a symbolic link that leads outside " + root + " (to " + real + ")"
                  : "it is outside " + root + ", the PixInsight script directory";
   }
   if ( !IsRegularFile( real ) )
      return "not a regular file";
   if ( !EndsWith( real, ".js" ) )
      return "it resolves to a file that is not a .js script";
   h = ReadHeader( real );
   if ( !h.featured )
      return "not a featured script (no #feature-id directive)";
   if ( MenuCategory( h.menu ) == "Development" )
      return "a developer tool (Script > Development: code signing, signing keys, CPD submission, documentation) is "
             "never offered";
   const std::string xsgn = real.substr( 0, real.size() - 3 ) + ".xsgn";
   if ( !IsRegularFile( xsgn ) )
      return "not signed (no " + xsgn.substr( xsgn.rfind( '/' ) + 1 ) + " signature file next to it)";
   return std::string();
}

struct CacheEntry
{
   Stamp     js, xsgn;
   SigResult sig;
};

std::map<std::string, CacheEntry>& SignatureCache()   // root thread only
{
   static std::map<std::string, CacheEntry> cache;
   return cache;
}

// Catalog ids: the signature's script id when no other eligible script has it.
// PixInsight signs some scripts under a shared id (NoiseEvaluation.js and
// NoiseEvaluationCFA.js are both "NoiseEvaluation"; AdP/ and AnnotateImage/
// both hold an "AnnotateImage"), so a shared id becomes, in order of
// preference, the file name (when those are distinct and taken by no other
// id), "<id>@<folder>", or "<id>@<file path>". Deterministic for a given tree.
void AssignCatalogIds( std::vector<ScriptEntry>& v, const std::map<std::string, int>& idCount )
{
   auto stem = []( const std::string& rel )
   {
      const size_t slash = rel.rfind( '/' );
      const std::string f = rel.substr( slash == std::string::npos ? 0 : slash + 1 );
      return f.size() > 3 ? f.substr( 0, f.size() - 3 ) : f;
   };
   auto dir = []( const std::string& rel )
   {
      const size_t slash = rel.rfind( '/' );
      return slash == std::string::npos ? std::string( "." ) : rel.substr( 0, slash );
   };
   std::set<std::string> taken;
   for ( const ScriptEntry& e : v )
      if ( idCount.at( e.scriptId ) == 1 )
         taken.insert( e.scriptId );
   for ( const auto& kv : idCount )
   {
      if ( kv.second < 2 )
         continue;
      std::vector<ScriptEntry*> group;
      for ( ScriptEntry& e : v )
         if ( e.scriptId == kv.first )
            group.push_back( &e );
      for ( int scheme = 0; scheme < 3; ++scheme )
      {
         std::set<std::string> ids;
         bool ok = true;
         for ( ScriptEntry* e : group )
         {
            const std::string id = scheme == 0 ? stem( e->rel )
                                 : scheme == 1 ? kv.first + "@" + dir( e->rel )
                                 : kv.first + "@" + e->rel;
            ok = ok && ids.insert( id ).second && taken.count( id ) == 0;
         }
         if ( ok || scheme == 2 )
         {
            for ( ScriptEntry* e : group )
            {
               e->id = scheme == 0 ? stem( e->rel ) : scheme == 1 ? kv.first + "@" + dir( e->rel ) : kv.first + "@" + e->rel;
               taken.insert( e->id );
            }
            break;
         }
      }
   }
   for ( ScriptEntry& e : v )
      if ( idCount.at( e.scriptId ) == 1 )
         e.id = e.scriptId;
}

void Walk( const std::string& dir, int depth, std::vector<std::string>& out )
{
   if ( depth > 24 )
      return;
   DIR* d = ::opendir( dir.c_str() );
   if ( d == nullptr )
      return;
   std::vector<std::string> names;
   while ( const dirent* e = ::readdir( d ) )
   {
      const std::string n = e->d_name;
      if ( n != "." && n != ".." )
         names.push_back( n );
   }
   ::closedir( d );
   std::sort( names.begin(), names.end() );
   for ( const std::string& n : names )
   {
      const std::string p = dir + "/" + n;
      struct stat st;
      if ( ::lstat( p.c_str(), &st ) != 0 )
         continue;
      if ( S_ISDIR( st.st_mode ) )
         Walk( p, depth + 1, out );          // never a linked directory (lstat)
      else if ( (S_ISREG( st.st_mode ) || S_ISLNK( st.st_mode )) && EndsWith( n, ".js" ) )
         out.push_back( p );
   }
}

} // namespace

String ProductionScriptsRoot( String& error )
{
   error.Clear();
   const String src = PixInsightSettings::GlobalString( "Application/SrcDirectory" );
   if ( src.IsEmpty() )
   {
      error = "PixInsight did not report its src directory (Application/SrcDirectory)";
      return String();
   }
   const std::string real = RealPath8( U8( src ) + "/scripts" );
   struct stat st;
   if ( real.empty() || ::stat( real.c_str(), &st ) != 0 || !S_ISDIR( st.st_mode ) )
   {
      error = "the PixInsight script directory " + src + "/scripts does not exist";
      return String();
   }
   return FromU8( real );
}

String PixInsightIncludeDirectory()
{
   return PixInsightSettings::GlobalString( "Application/IncludeDirectory" );
}

bool CheckScriptEligibility( const String& path, const String& root, ScriptEntry& entry, String& reason )
{
   entry = ScriptEntry();
   reason.Clear();
   try
   {
      if ( !Thread::IsRootThread() )
         throw Error( "root thread only" );
      const std::string root8 = U8( root );
      if ( root8.empty() || root8[0] != '/' )
      {
         reason = "no script directory";
         return false;
      }
      std::string real;
      Header h;
      const std::string p = PathProblem( path, root8, real, h );
      if ( !p.empty() )
      {
         reason = FromU8( p );
         return false;
      }
      const SigResult s = VerifySignatures( { real } ).front();
      const std::string sp = SignatureProblem( s );
      if ( !sp.empty() )
      {
         reason = FromU8( sp );
         return false;
      }
      entry.id = s.scriptId;
      entry.path = real;
      entry.rel = real.substr( root8.size() + 1 );
      entry.scriptId = s.scriptId;
      entry.featureId = h.featureId;
      entry.menu = h.menu;
      entry.developerId = s.developerId;
      entry.developerName = s.developerName;
      entry.signedAt = s.timestamp;
      return true;
   }
   catch ( const pcl::Exception& x )
   {
      reason = "the eligibility check failed: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      reason = String( "the eligibility check failed: " ) + String( x.what() );
   }
   return false;
}

ScriptCatalog BuildScriptCatalog( const String& root, bool useCache )
{
   ScriptCatalog c;
   const auto t0 = std::chrono::steady_clock::now();
   try
   {
      if ( !Thread::IsRootThread() )
         throw Error( "root thread only" );
      c.root = root;
      const std::string root8 = U8( root );
      std::vector<std::string> files;
      Walk( root8, 0, files );

      struct Pending { std::string rel, real; Header h; Stamp js, xsgn; };
      std::vector<Pending> pending;
      for ( const std::string& f : files )
      {
         std::string real;
         Header h;
         const std::string rel = f.substr( root8.size() + 1 );
         const std::string p = PathProblem( FromU8( f ), root8, real, h );
         if ( !p.empty() )
         {
            if ( h.featured || p.find( "signature" ) != std::string::npos || p.find( "signed" ) != std::string::npos
              || p.find( "symbolic link" ) != std::string::npos || p.find( "Development" ) != std::string::npos )
               c.refused.push_back( { U8( FromU8( rel ) ), p } );   // a library file (no #feature-id) is not listed
            continue;
         }
         pending.push_back( { U8( FromU8( rel ) ), real, h, StampOf( real ), StampOf( real.substr( 0, real.size() - 3 ) + ".xsgn" ) } );
      }

      // Signatures: cached results for unchanged files, one batch for the rest.
      std::map<std::string, CacheEntry>& cache = SignatureCache();
      std::vector<std::string> toVerify;
      for ( const Pending& p : pending )
      {
         const auto it = cache.find( p.real );
         if ( !useCache || it == cache.end() || !(it->second.js == p.js) || !(it->second.xsgn == p.xsgn) )
            toVerify.push_back( p.real );
      }
      const std::vector<SigResult> fresh = VerifySignatures( toVerify );
      for ( size_t i = 0; i < toVerify.size(); ++i )
      {
         CacheEntry e;
         e.js = StampOf( toVerify[i] );
         e.xsgn = StampOf( toVerify[i].substr( 0, toVerify[i].size() - 3 ) + ".xsgn" );
         e.sig = fresh[i];
         cache[toVerify[i]] = e;
      }

      std::map<std::string, int> idCount;
      for ( const Pending& p : pending )
      {
         const SigResult& s = cache[p.real].sig;
         const std::string sp = SignatureProblem( s );
         if ( !sp.empty() )
         {
            c.refused.push_back( { p.rel, sp } );
            continue;
         }
         ScriptEntry e;
         e.path = p.real;
         e.rel = p.rel;
         e.scriptId = s.scriptId;
         e.featureId = p.h.featureId;
         e.menu = p.h.menu;
         e.developerId = s.developerId;
         e.developerName = s.developerName;
         e.signedAt = s.timestamp;
         c.eligible.push_back( e );
         ++idCount[s.scriptId];
      }
      AssignCatalogIds( c.eligible, idCount );
      c.ok = true;
   }
   catch ( const pcl::Exception& x )
   {
      c.ok = false;
      c.error = "the script catalog could not be built: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      c.ok = false;
      c.error = String( "the script catalog could not be built: " ) + String( x.what() );
   }
   c.ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
   return c;
}

String ScriptEntry::PathString() const
{
   return FromU8( path );
}

const ScriptEntry* FindScript( const ScriptCatalog& c, const std::string& id )
{
   for ( const ScriptEntry& e : c.eligible )
      if ( e.id == id )
         return &e;
   return nullptr;
}

} // namespace pcl
