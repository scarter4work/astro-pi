// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ScriptSource.h"
#include "TextSafety.h"
#include "Utf8.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace pcl
{

namespace
{

// ---------------------------------------------------------------------------
// Files

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

std::string DirOf( const std::string& p )
{
   const size_t s = p.rfind( '/' );
   return s == std::string::npos ? std::string( "." ) : p.substr( 0, s );
}

bool ReadBytes( const std::string& path, size_t maxBytes, std::string& out, std::string& why )
{
   std::ifstream f( path, std::ios::binary );
   if ( !f )
   {
      why = "cannot be opened";
      return false;
   }
   f.seekg( 0, std::ios::end );
   const std::streamoff n = f.tellg();
   if ( n < 0 )
   {
      why = "cannot be read";
      return false;
   }
   if ( size_t( n ) > maxBytes )
   {
      why = "is larger than " + std::to_string( maxBytes ) + " bytes";
      return false;
   }
   f.seekg( 0, std::ios::beg );
   out.assign( size_t( n ), '\0' );
   if ( n > 0 && !f.read( &out[0], n ) )
   {
      why = "cannot be read";
      return false;
   }
   return true;
}

// Text from the script as valid UTF-8 (an invalid byte sequence becomes U+FFFD).
std::string CleanUtf8( const std::string& s )
{
   return U8( FromU8( s ) );
}

std::string Trim( const std::string& s )
{
   size_t a = 0, b = s.size();
   while ( a < b && std::isspace( uint8( s[a] ) ) )
      ++a;
   while ( b > a && std::isspace( uint8( s[b-1] ) ) )
      --b;
   return s.substr( a, b - a );
}

bool IsIdStart( char c )
{
   return std::isalpha( uint8( c ) ) || c == '_' || c == '$';
}

bool IsIdChar( char c )
{
   return std::isalnum( uint8( c ) ) || c == '_' || c == '$';
}

// ---------------------------------------------------------------------------
// One source file: directive lines blanked, comments blanked (strings kept),
// and a per-byte mask of string / template / regex literal content.

struct SrcFile
{
   std::string       rel;       // relative to the tree root
   std::string       path;      // real path
   std::string       code;      // directives + comments replaced by spaces; newlines kept
   std::vector<char> literal;   // 1 = inside a string / template / regex literal (delimiters included)
   std::vector<size_t> lineStart;

   int LineOf( size_t pos ) const
   {
      const auto it = std::upper_bound( lineStart.begin(), lineStart.end(), pos );
      return int( it - lineStart.begin() );
   }
};

struct Directive
{
   std::string name;    // "define", "include", "feature-id", ...
   std::string value;   // the rest of the (joined) line
   int         line = 0;
};

// Splits raw text into lines, collects the preprocessor directives (a line
// whose first non-blank character is '#'; a trailing backslash continues it)
// and blanks them in `code`.
void ExtractDirectives( const std::string& raw, std::string& code, std::vector<Directive>& dirs )
{
   code = raw;
   size_t i = 0;
   int line = 1;
   while ( i < raw.size() )
   {
      size_t eol = raw.find( '\n', i );
      if ( eol == std::string::npos )
         eol = raw.size();
      size_t a = i;
      while ( a < eol && (raw[a] == ' ' || raw[a] == '\t') )
         ++a;
      if ( a < eol && raw[a] == '#' )
      {
         const int startLine = line;
         std::string text;
         size_t lineBegin = i, lineEnd = eol;
         for ( ;; )
         {
            std::string part = raw.substr( lineBegin, lineEnd - lineBegin );
            if ( !part.empty() && part.back() == '\r' )
               part.pop_back();
            for ( size_t k = lineBegin; k < lineEnd; ++k )
               code[k] = ' ';
            const bool cont = !part.empty() && part.back() == '\\';
            if ( cont )
               part.pop_back();
            text += part;
            if ( !cont || lineEnd >= raw.size() )
               break;
            text += ' ';
            lineBegin = lineEnd + 1;
            ++line;
            lineEnd = raw.find( '\n', lineBegin );
            if ( lineEnd == std::string::npos )
               lineEnd = raw.size();
         }
         eol = lineEnd;
         std::string t = Trim( text ).substr( 1 );   // after '#'
         size_t n = 0;
         while ( n < t.size() && (IsIdChar( t[n] ) || t[n] == '-') )
            ++n;
         Directive d;
         d.name = t.substr( 0, n );
         d.value = Trim( t.substr( n ) );
         d.line = startLine;
         dirs.push_back( d );
      }
      i = eol + 1;
      ++line;
   }
}

// Comments -> spaces; strings, template literals and regex literals kept and
// marked. The regex heuristic is the usual one: a '/' where an expression
// may start (after an operator, '(' '[' ',' ':' ';' '{' '}' '!' '?', or a
// keyword such as return) starts a regex literal.
void StripComments( SrcFile& f )
{
   std::string& s = f.code;
   f.literal.assign( s.size(), 0 );
   enum { Code, Line, Block, Sq, Dq, Tpl, Rx } st = Code;
   bool rxClass = false;
   char prevSig = 0;    // previous significant code character
   std::string word;    // the identifier/keyword prevSig ended, if any
   auto regexCanStart = [&]()
   {
      if ( prevSig == 0 )
         return true;
      if ( IsIdChar( prevSig ) )
         return word == "return" || word == "typeof" || word == "case" || word == "in" || word == "of"
             || word == "delete" || word == "void" || word == "throw" || word == "new" || word == "else";
      return std::strchr( "(,=:[!&|?{};+-*%<>~^", prevSig ) != nullptr;
   };
   for ( size_t i = 0; i < s.size(); ++i )
   {
      const char c = s[i];
      const char n = i+1 < s.size() ? s[i+1] : '\0';
      switch ( st )
      {
      case Code:
         if ( c == '/' && n == '/' )
         {
            st = Line;
            s[i] = ' ';
         }
         else if ( c == '/' && n == '*' )
         {
            st = Block;
            s[i] = ' ';
         }
         else if ( c == '\'' || c == '"' || c == '`' )
         {
            st = c == '\'' ? Sq : c == '"' ? Dq : Tpl;
            f.literal[i] = 1;
         }
         else if ( c == '/' && regexCanStart() )
         {
            st = Rx;
            rxClass = false;
            f.literal[i] = 1;
         }
         else if ( !std::isspace( uint8( c ) ) )
         {
            if ( IsIdChar( c ) )
            {
               if ( i == 0 || !IsIdChar( s[i-1] ) )
                  word.clear();
               word += c;
            }
            prevSig = c;
         }
         break;
      case Line:
         if ( c == '\n' )
            st = Code;
         else
            s[i] = ' ';
         break;
      case Block:
         if ( c == '*' && n == '/' )
         {
            s[i] = ' ';
            s[i+1] = ' ';
            ++i;
            st = Code;
         }
         else if ( c != '\n' )
            s[i] = ' ';
         break;
      case Sq:
      case Dq:
      case Tpl:
         f.literal[i] = 1;
         if ( c == '\\' && i+1 < s.size() )
         {
            f.literal[++i] = 1;
         }
         else if ( (st == Sq && c == '\'') || (st == Dq && c == '"') || (st == Tpl && c == '`') )
         {
            st = Code;
            prevSig = '"';   // an operand: a following '/' is a division
            word.clear();
         }
         else if ( c == '\n' && st != Tpl )
            st = Code;   // unterminated: never swallow the rest of the file
         break;
      case Rx:
         f.literal[i] = 1;
         if ( c == '\\' && i+1 < s.size() )
            f.literal[++i] = 1;
         else if ( c == '[' )
            rxClass = true;
         else if ( c == ']' )
            rxClass = false;
         else if ( c == '/' && !rxClass )
         {
            st = Code;
            prevSig = '"';
            word.clear();
         }
         else if ( c == '\n' )
            st = Code;
         break;
      }
   }
   f.lineStart.clear();
   f.lineStart.push_back( 0 );
   for ( size_t i = 0; i < s.size(); ++i )
      if ( s[i] == '\n' )
         f.lineStart.push_back( i + 1 );
}

// ---------------------------------------------------------------------------
// Literals

// Parses a JavaScript string literal at s[i] ('...' or "..."). Simple
// escapes only; \u and \x escapes are decoded. Returns false when s[i] does
// not start a complete literal on its line.
bool ParseStringLiteral( const std::string& s, size_t& i, std::string& out )
{
   if ( i >= s.size() || (s[i] != '"' && s[i] != '\'') )
      return false;
   const char q = s[i];
   std::u16string u;
   size_t k = i + 1;
   std::string bytes;
   while ( k < s.size() && s[k] != q )
   {
      if ( s[k] == '\n' )
         return false;
      if ( s[k] == '\\' && k+1 < s.size() )
      {
         const char e = s[k+1];
         auto hex = [&]( size_t from, size_t count, unsigned& v )
         {
            v = 0;
            if ( from + count > s.size() )
               return false;
            for ( size_t h = 0; h < count; ++h )
            {
               const char d = s[from+h];
               if ( !std::isxdigit( uint8( d ) ) )
                  return false;
               v = v*16 + unsigned( std::isdigit( uint8( d ) ) ? d - '0' : (std::tolower( uint8( d ) ) - 'a' + 10) );
            }
            return true;
         };
         unsigned v = 0;
         if ( e == 'u' && hex( k+2, 4, v ) )
         {
            bytes += U8( String( char16_type( v ) ) );
            k += 6;
            continue;
         }
         if ( e == 'x' && hex( k+2, 2, v ) )
         {
            bytes += U8( String( char16_type( v ) ) );
            k += 4;
            continue;
         }
         switch ( e )
         {
         case 'n': bytes += '\n'; break;
         case 't': bytes += '\t'; break;
         case 'r': bytes += '\r'; break;
         case '0': bytes += '\0'; break;
         default:  bytes += e; break;
         }
         k += 2;
         continue;
      }
      bytes += s[k++];
   }
   if ( k >= s.size() )
      return false;
   out = CleanUtf8( bytes );
   i = k + 1;
   return true;
}

// A numeric literal at s[i]: decimal (optionally signed, fraction, exponent)
// or hexadecimal. The value as JSON (integer when it has no fraction/exponent).
bool ParseNumberLiteral( const std::string& s, size_t& i, nlohmann::json& out )
{
   size_t k = i;
   bool neg = false;
   if ( k < s.size() && (s[k] == '-' || s[k] == '+') )
   {
      neg = s[k] == '-';
      ++k;
      while ( k < s.size() && (s[k] == ' ' || s[k] == '\t') )
         ++k;
   }
   if ( k >= s.size() || !(std::isdigit( uint8( s[k] ) ) || (s[k] == '.' && k+1 < s.size() && std::isdigit( uint8( s[k+1] ) ))) )
      return false;
   const size_t start = k;
   if ( s[k] == '0' && k+1 < s.size() && (s[k+1] == 'x' || s[k+1] == 'X') )
   {
      k += 2;
      const size_t h = k;
      while ( k < s.size() && std::isxdigit( uint8( s[k] ) ) )
         ++k;
      if ( k == h || (k < s.size() && IsIdChar( s[k] )) )
         return false;
      const long long v = std::strtoll( s.substr( h, k - h ).c_str(), nullptr, 16 );
      out = neg ? -v : v;
      i = k;
      return true;
   }
   bool real = false;
   while ( k < s.size() && std::isdigit( uint8( s[k] ) ) )
      ++k;
   if ( k < s.size() && s[k] == '.' )
   {
      real = true;
      ++k;
      while ( k < s.size() && std::isdigit( uint8( s[k] ) ) )
         ++k;
   }
   if ( k < s.size() && (s[k] == 'e' || s[k] == 'E') )
   {
      size_t e = k + 1;
      if ( e < s.size() && (s[e] == '+' || s[e] == '-') )
         ++e;
      if ( e < s.size() && std::isdigit( uint8( s[e] ) ) )
      {
         real = true;
         k = e;
         while ( k < s.size() && std::isdigit( uint8( s[k] ) ) )
            ++k;
      }
   }
   if ( k < s.size() && IsIdChar( s[k] ) )
      return false;
   const std::string text = s.substr( start, k - start );
   if ( real )
   {
      const double v = std::strtod( text.c_str(), nullptr );
      out = neg ? -v : v;
   }
   else
   {
      const long long v = std::strtoll( text.c_str(), nullptr, 10 );
      out = neg ? -v : v;
   }
   i = k;
   return true;
}

struct Defines
{
   std::map<std::string, std::string> raw;   // NAME -> value text (first definition wins, as written)

   // A #define resolved to a literal (string, number, true/false), following
   // define-to-define chains (depth-bounded). false when not a simple constant.
   bool Literal( const std::string& name, nlohmann::json& out, int depth = 0 ) const
   {
      const auto it = raw.find( name );
      if ( it == raw.end() || depth > 8 )
         return false;
      std::string v = Trim( it->second );
      // A trailing // comment after the value.
      size_t i = 0;
      std::string str;
      if ( !v.empty() && (v[0] == '"' || v[0] == '\'') )
      {
         if ( !ParseStringLiteral( v, i, str ) )
            return false;
         const std::string rest = Trim( v.substr( i ) );
         if ( !rest.empty() && rest.compare( 0, 2, "//" ) != 0 )
            return false;
         out = str;
         return true;
      }
      const size_t cut = v.find( "//" );
      if ( cut != std::string::npos )
         v = Trim( v.substr( 0, cut ) );
      while ( v.size() >= 2 && v.front() == '(' && v.back() == ')' )
         v = Trim( v.substr( 1, v.size() - 2 ) );
      if ( v == "true" || v == "false" )
      {
         out = v == "true";
         return true;
      }
      i = 0;
      nlohmann::json num;
      if ( ParseNumberLiteral( v, i, num ) && Trim( v.substr( i ) ).empty() )
      {
         out = num;
         return true;
      }
      bool ident = !v.empty() && IsIdStart( v[0] );
      for ( char c : v )
         ident = ident && IsIdChar( c );
      return ident && Literal( v, out, depth + 1 );
   }
};

// A literal at s[i] in code: string, number, true/false, or a #define name of
// a literal. The literal must END the expression (next significant char is
// one of ; , } ) ] or a line end), so "0.5*k" is not taken for 0.5.
bool ParseValueAt( const std::string& s, size_t& i, const Defines& defs, nlohmann::json& out )
{
   size_t k = i;
   while ( k < s.size() && (s[k] == ' ' || s[k] == '\t') )
      ++k;
   nlohmann::json v;
   std::string str;
   if ( k < s.size() && (s[k] == '"' || s[k] == '\'') )
   {
      if ( !ParseStringLiteral( s, k, str ) )
         return false;
      v = str;
   }
   else if ( ParseNumberLiteral( s, k, v ) )
   {
   }
   else if ( k < s.size() && IsIdStart( s[k] ) )
   {
      const size_t a = k;
      while ( k < s.size() && IsIdChar( s[k] ) )
         ++k;
      const std::string w = s.substr( a, k - a );
      if ( w == "true" || w == "false" )
         v = w == "true";
      else if ( !defs.Literal( w, v ) )
         return false;
   }
   else
      return false;
   size_t e = k;
   while ( e < s.size() && (s[e] == ' ' || s[e] == '\t' || s[e] == '\r') )
      ++e;
   if ( e < s.size() && std::strchr( ";,})]\n", s[e] ) == nullptr )
      return false;
   out = v;
   i = k;
   return true;
}

// ---------------------------------------------------------------------------
// The scan

const char* const kGetters[] = { "getBoolean", "getInteger", "getInt64", "getInt", "getUInt", "getUint64", "getUint",
                                 "getReal", "getString", "get", "has" };

std::string GetterType( const std::string& g )
{
   if ( g == "getBoolean" ) return "boolean";
   if ( g == "getInteger" || g == "getInt" ) return "integer";
   if ( g == "getUInt" || g == "getUint" ) return "unsigned";
   if ( g == "getInt64" ) return "int64";
   if ( g == "getUint64" ) return "uint64";
   if ( g == "getReal" ) return "real";
   if ( g == "getString" || g == "get" ) return "string";
   return std::string();   // has()
}

std::string Ref( const ScriptSourceRef& r )
{
   return r.file + ":" + std::to_string( r.line );
}

struct Reader
{
   std::string              root;      // the script tree (real path)
   std::string              sysRoot;   // PixInsight's include directory (real path; "" = not followed)
   std::vector<SrcFile>     files;
   std::set<std::string>    seen;
   size_t                   total = 0;
   ScriptDescription&       d;
   Defines                  defs;
   std::map<std::string, std::string> mainDefines;   // the main file's own #defines

   explicit Reader( ScriptDescription& out ) : d( out ) {}

   std::string Rel( const std::string& path ) const
   {
      if ( IsInside( path, root ) )
         return path.substr( root.size() + 1 );
      if ( !sysRoot.empty() && IsInside( path, sysRoot ) )
         return "<" + path.substr( sysRoot.size() + 1 ) + ">";
      return path;
   }

   // Reads `path` (a real path inside the tree) and, depth-first, its quoted
   // #include files inside the tree.
   bool Read( const std::string& path, bool isMain )
   {
      if ( seen.count( path ) > 0 )
         return true;
      if ( files.size() >= PICopilotScriptMaxFiles )
      {
         d.limitHit = true;
         return !isMain;
      }
      seen.insert( path );
      std::string raw, why;
      if ( !ReadBytes( path, PICopilotScriptMaxFileBytes, raw, why ) || total + raw.size() > PICopilotScriptMaxTotalBytes )
      {
         if ( why.empty() )
         {
            why = "the script and its includes exceed " + std::to_string( PICopilotScriptMaxTotalBytes ) + " bytes";
            d.limitHit = true;
         }
         if ( isMain )
            d.error = "the script file " + why;
         else
            d.includesNotFollowed.push_back( { { "include", Rel( path ) }, { "reason", "the file " + why } } );
         return !isMain;
      }
      total += raw.size();
      SrcFile f;
      f.path = path;
      f.rel = Rel( path );
      std::vector<Directive> dirs;
      ExtractDirectives( raw, f.code, dirs );
      StripComments( f );
      const size_t index = files.size();
      files.push_back( f );
      for ( const Directive& dir : dirs )
      {
         if ( dir.name == "define" )
         {
            size_t n = 0;
            while ( n < dir.value.size() && IsIdChar( dir.value[n] ) )
               ++n;
            const std::string name = dir.value.substr( 0, n );
            if ( !name.empty() && defs.raw.count( name ) == 0 )
               defs.raw[name] = dir.value.substr( n );
            if ( isMain && !name.empty() && mainDefines.count( name ) == 0 )
               mainDefines[name] = dir.value.substr( n );
         }
         else if ( isMain && dir.name == "feature-id" )
         {
            const std::string v = CleanUtf8( dir.value );
            const size_t colon = v.find( ':' );
            if ( colon != std::string::npos )
            {
               d.featureId = Trim( v.substr( 0, colon ) );
               d.menu = Trim( v.substr( colon + 1 ) );
            }
            else
               d.menu = Trim( v );
         }
         else if ( isMain && dir.name == "feature-info" )
         {
            // HTML tags out, whitespace collapsed.
            std::string t, v = CleanUtf8( dir.value );
            bool tag = false;
            for ( char c : v )
            {
               if ( c == '<' )
                  tag = true;
               else if ( c == '>' && tag )
               {
                  tag = false;
                  t += ' ';
               }
               else if ( !tag )
                  t += std::isspace( uint8( c ) ) ? ' ' : c;
            }
            std::string collapsed;
            for ( char c : t )
               if ( !(c == ' ' && (collapsed.empty() || collapsed.back() == ' ')) )
                  collapsed += c;
            d.info = Trim( collapsed );
         }
         else if ( isMain && dir.name == "script-id" )
            d.scriptIdDirective = Trim( CleanUtf8( dir.value ) );
         else if ( dir.name == "include" )
         {
            const std::string v = Trim( dir.value );
            const std::string from = f.rel + ":" + std::to_string( dir.line );
            if ( v.size() >= 2 && v.front() == '<' )
            {
               // A PixInsight system header (<pjsr/...>): part of the PixInsight
               // distribution; read too, since some read parameters for the script.
               const size_t close = v.find( '>' );
               const std::string inner = close == std::string::npos ? std::string() : v.substr( 1, close - 1 );
               const std::string target = sysRoot.empty() || inner.empty() ? std::string() : RealPath8( sysRoot + "/" + inner );
               if ( target.empty() )
                  d.includesNotFollowed.push_back( { { "include", CleanUtf8( v ) }, { "from", from },
                                                     { "reason", "a PixInsight system header that was not found" } } );
               else if ( !IsInside( target, sysRoot ) )
                  d.includesNotFollowed.push_back( { { "include", CleanUtf8( v ) }, { "from", from },
                                                     { "reason", "outside PixInsight's include directory (not read)" } } );
               else
                  Read( target, false );
               continue;
            }
            size_t i = 0;
            std::string inc;
            if ( !ParseStringLiteral( v, i, inc ) )
            {
               d.includesNotFollowed.push_back( { { "include", CleanUtf8( v ) }, { "from", from },
                                                  { "reason", "not a quoted file name" } } );
               continue;
            }
            const std::string target = RealPath8( inc.size() > 0 && inc[0] == '/' ? inc : DirOf( path ) + "/" + inc );
            if ( target.empty() )
               d.includesNotFollowed.push_back( { { "include", inc }, { "from", from }, { "reason", "not found" } } );
            else if ( !IsInside( target, root ) )
               d.includesNotFollowed.push_back( { { "include", inc }, { "from", from },
                                                  { "reason", "outside the script tree (not read)" } } );
            else
               Read( target, false );
         }
      }
      (void)index;
      return true;
   }

   ScriptSourceRef At( const SrcFile& f, size_t pos ) const
   {
      ScriptSourceRef r;
      r.file = f.rel;
      r.line = f.LineOf( pos );
      return r;
   }

   static size_t SkipWs( const std::string& s, size_t i )
   {
      while ( i < s.size() && std::isspace( uint8( s[i] ) ) )
         ++i;
      return i;
   }

   // The matching ')' for the '(' at s[open] (literals skipped); npos if none.
   static size_t Close( const SrcFile& f, size_t open )
   {
      int depth = 0;
      for ( size_t i = open; i < f.code.size(); ++i )
      {
         if ( f.literal[i] )
            continue;
         const char c = f.code[i];
         if ( c == '(' || c == '[' || c == '{' )
            ++depth;
         else if ( c == ')' || c == ']' || c == '}' )
            if ( --depth == 0 )
               return i;
      }
      return std::string::npos;
   }

   // The assignment target just before `pos` ("LHS = Parameters.getX(...)"),
   // or "" when the call is not the whole right-hand side of an assignment.
   static std::string AssignedTo( const SrcFile& f, size_t pos )
   {
      const std::string& s = f.code;
      size_t i = pos;
      while ( i > 0 && std::isspace( uint8( s[i-1] ) ) )
         --i;
      if ( i == 0 || s[i-1] != '=' )
         return std::string();
      if ( i >= 2 && std::strchr( "=!<>+-*/%&|^", s[i-2] ) != nullptr )
         return std::string();   // ==, !=, <=, >=, +=, ...
      --i;
      while ( i > 0 && std::isspace( uint8( s[i-1] ) ) )
         --i;
      const size_t end = i;
      while ( i > 0 && (IsIdChar( s[i-1] ) || s[i-1] == '.') && !f.literal[i-1] )
         --i;
      std::string lhs = s.substr( i, end - i );
      if ( lhs.empty() || !IsIdStart( lhs[0] ) || lhs.back() == '.' )
         return std::string();
      // A declaration ("let x = ...") assigns a fresh local: no earlier default.
      size_t j = i;
      while ( j > 0 && std::isspace( uint8( s[j-1] ) ) )
         --j;
      size_t w = j;
      while ( w > 0 && IsIdChar( s[w-1] ) )
         --w;
      const std::string kw = s.substr( w, j - w );
      if ( kw == "var" || kw == "let" || kw == "const" )
         return std::string();
      return lhs;
   }

   // Every "LHS = <literal>" in the files read, plus "X: <literal>" object
   // members when LHS is "this.X" (the documented parameters-object pattern).
   void FindInitialValues( const std::string& lhs, std::vector<std::pair<nlohmann::json, ScriptSourceRef>>& found ) const
   {
      const bool thisMember = lhs.compare( 0, 5, "this." ) == 0 && lhs.find( '.', 5 ) == std::string::npos;
      const std::string member = thisMember ? lhs.substr( 5 ) : std::string();
      for ( const SrcFile& f : files )
      {
         const std::string& s = f.code;
         for ( size_t p = s.find( lhs ); p != std::string::npos; p = s.find( lhs, p + 1 ) )
         {
            if ( f.literal[p] || (p > 0 && (IsIdChar( s[p-1] ) || s[p-1] == '.')) )
               continue;
            size_t e = p + lhs.size();
            if ( e < s.size() && IsIdChar( s[e] ) )
               continue;
            e = SkipWs( s, e );
            if ( e >= s.size() || s[e] != '=' || (e+1 < s.size() && (s[e+1] == '=' || s[e+1] == '>')) )
               continue;
            size_t v = e + 1;
            nlohmann::json val;
            if ( ParseValueAt( s, v, defs, val ) )
               found.push_back( { val, At( f, p ) } );
         }
         if ( thisMember )
            for ( size_t p = s.find( member ); p != std::string::npos; p = s.find( member, p + 1 ) )
            {
               if ( f.literal[p] || (p > 0 && (IsIdChar( s[p-1] ) || s[p-1] == '.')) )
                  continue;
               size_t e = p + member.size();
               if ( e < s.size() && IsIdChar( s[e] ) )
                  continue;
               e = SkipWs( s, e );
               if ( e >= s.size() || s[e] != ':' )
                  continue;
               size_t b = p;
               while ( b > 0 && std::isspace( uint8( s[b-1] ) ) )
                  --b;
               if ( b == 0 || (s[b-1] != '{' && s[b-1] != ',') )
                  continue;   // not an object member ("a ? x : 1" is skipped)
               size_t v = e + 1;
               nlohmann::json val;
               if ( ParseValueAt( s, v, defs, val ) )
                  found.push_back( { val, At( f, p ) } );
            }
      }
   }

   void Scan()
   {
      std::map<std::string, size_t> byName;
      std::map<std::string, std::vector<std::string>> lhsByName;   // assignment targets per parameter
      std::map<std::string, std::pair<nlohmann::json, ScriptSourceRef>> ternaryDefault;
      for ( const SrcFile& f : files )
      {
         const std::string& s = f.code;
         for ( size_t p = s.find( "Parameters" ); p != std::string::npos; p = s.find( "Parameters", p + 1 ) )
         {
            if ( f.literal[p] || (p > 0 && (IsIdChar( s[p-1] ) || s[p-1] == '.')) )
               continue;
            size_t i = p + 10;
            if ( i < s.size() && IsIdChar( s[i] ) )
               continue;
            i = SkipWs( s, i );
            if ( i >= s.size() || s[i] != '.' )
               continue;
            i = SkipWs( s, i + 1 );
            const size_t m0 = i;
            while ( i < s.size() && IsIdChar( s[i] ) )
               ++i;
            const std::string member = s.substr( m0, i - m0 );
            const ScriptSourceRef at = At( f, p );
            if ( member == "isViewTarget" || member == "targetView" )
            {
               d.viewTargetAt.push_back( at );
               continue;
            }
            if ( member == "isGlobalTarget" )
            {
               d.globalTargetAt.push_back( at );
               continue;
            }
            bool getter = false;
            for ( const char* g : kGetters )
               getter = getter || member == g;
            if ( !getter )
               continue;
            const size_t open = SkipWs( s, i );
            if ( open >= s.size() || s[open] != '(' )
               continue;
            const size_t close = Close( f, open );
            size_t a = SkipWs( s, open + 1 );
            std::string key, viaDefine;
            bool resolved = false;
            if ( a < s.size() && (s[a] == '"' || s[a] == '\'') )
            {
               size_t k = a;
               std::string lit;
               if ( ParseStringLiteral( s, k, lit ) )
               {
                  k = SkipWs( s, k );
                  if ( k < s.size() && (s[k] == ',' || s[k] == ')') )
                  {
                     key = lit;
                     resolved = true;
                  }
               }
            }
            else if ( a < s.size() && IsIdStart( s[a] ) )
            {
               size_t k = a;
               while ( k < s.size() && IsIdChar( s[k] ) )
                  ++k;
               const std::string ident = s.substr( a, k - a );
               k = SkipWs( s, k );
               nlohmann::json v;
               if ( k < s.size() && (s[k] == ',' || s[k] == ')') && defs.Literal( ident, v ) && v.is_string() )
               {
                  key = v.get<std::string>();
                  viaDefine = ident;
                  resolved = true;
               }
            }
            if ( !resolved || key.empty() )
            {
               ScriptUnresolvedRead u;
               u.getter = member;
               const size_t end = close == std::string::npos ? std::min( s.size(), open + 81 ) : close;
               std::string expr = Trim( s.substr( open + 1, end > open + 1 ? end - open - 1 : 0 ) );
               for ( char& c : expr )
                  if ( c == '\n' || c == '\r' || c == '\t' )
                     c = ' ';
               if ( expr.size() > 80 )
                  expr = expr.substr( 0, 77 ) + "...";
               u.expression = CleanUtf8( expr );
               u.at = at;
               d.unresolved.push_back( u );
               continue;
            }
            size_t idx;
            const auto it = byName.find( key );
            if ( it == byName.end() )
            {
               idx = d.parameters.size();
               byName[key] = idx;
               ScriptParameter sp;
               sp.name = key;
               d.parameters.push_back( sp );
            }
            else
               idx = it->second;
            ScriptParameter& sp = d.parameters[idx];
            if ( std::find( sp.getters.begin(), sp.getters.end(), member ) == sp.getters.end() )
               sp.getters.push_back( member );
            if ( sp.viaDefine.empty() )
               sp.viaDefine = viaDefine;
            sp.at.push_back( at );
            if ( member != "has" )
            {
               const std::string lhs = AssignedTo( f, p );
               if ( !lhs.empty() )
               {
                  std::vector<std::string>& l = lhsByName[key];
                  if ( std::find( l.begin(), l.end(), lhs ) == l.end() )
                     l.push_back( lhs );
               }
               // Parameters.has( k ) ? Parameters.getX( k ) : <literal>
               if ( close != std::string::npos )
               {
                  size_t b = p;
                  while ( b > 0 && std::isspace( uint8( s[b-1] ) ) )
                     --b;
                  size_t c = SkipWs( s, close + 1 );
                  if ( b > 0 && s[b-1] == '?' && c < s.size() && s[c] == ':' )
                  {
                     size_t v = c + 1;
                     nlohmann::json val;
                     if ( ParseValueAt( s, v, defs, val ) && ternaryDefault.count( key ) == 0 )
                        ternaryDefault[key] = { val, At( f, c ) };
                  }
               }
            }
         }
      }

      // Types and initial values.
      for ( ScriptParameter& sp : d.parameters )
      {
         std::set<std::string> types;
         for ( const std::string& g : sp.getters )
         {
            const std::string t = GetterType( g );
            if ( !t.empty() )
               types.insert( t );
         }
         if ( types.size() > 1 )
            types.erase( "string" );   // get()/getString() next to a typed getter: the typed one says what it is
         sp.type = types.empty() ? "presence" : types.size() == 1 ? *types.begin() : "mixed";

         std::vector<std::pair<nlohmann::json, ScriptSourceRef>> cands;
         const auto td = ternaryDefault.find( sp.name );
         if ( td != ternaryDefault.end() )
            cands.push_back( td->second );
         else
            for ( const std::string& lhs : lhsByName[sp.name] )
               FindInitialValues( lhs, cands );
         if ( cands.empty() )
            continue;
         bool same = true;
         for ( const auto& c : cands )
            same = same && c.first == cands.front().first;
         if ( !same )
         {
            std::string list;
            for ( size_t i = 0; i < cands.size() && i < 4; ++i )
               list += (i ? ", " : "") + cands[i].first.dump() + " at " + Ref( cands[i].second );
            sp.defaultNote = "not statically obvious: the source sets it to different values (" + list + ")";
            continue;
         }
         const nlohmann::json& v = cands.front().first;
         const bool typeOk =
              sp.type == "boolean" ? v.is_boolean()
            : (sp.type == "integer" || sp.type == "int64") ? v.is_number_integer()
            : (sp.type == "unsigned" || sp.type == "uint64") ? (v.is_number_integer() && v.get<long long>() >= 0)
            : sp.type == "real" ? v.is_number()
            : sp.type == "string" ? v.is_string()
            : true;
         if ( !typeOk )
         {
            sp.defaultNote = "the initial value in the source (" + v.dump() + " at " + Ref( cands.front().second )
                           + ") does not match the getter's type";
            continue;
         }
         sp.hasDefault = true;
         sp.defaultValue = v;
         sp.defaultAt = cands.front().second;
      }

      // Interaction evidence.
      for ( const SrcFile& f : files )
      {
         const std::string& s = f.code;
         for ( size_t p = s.find( "execute" ); p != std::string::npos; p = s.find( "execute", p + 1 ) )
         {
            if ( f.literal[p] || p == 0 )
               continue;
            size_t b = p;
            while ( b > 0 && std::isspace( uint8( s[b-1] ) ) )
               --b;
            if ( b == 0 || s[b-1] != '.' )
               continue;
            size_t e = p + 7;
            if ( e < s.size() && IsIdChar( s[e] ) )
               continue;   // executeOn, executeGlobal: processes, not dialogs
            e = SkipWs( s, e );
            if ( e >= s.size() || s[e] != '(' )
               continue;
            e = SkipWs( s, e + 1 );
            if ( e < s.size() && s[e] == ')' )
               d.executeCallsAt.push_back( At( f, p ) );
         }
         for ( size_t p = s.find( "MessageBox" ); p != std::string::npos; p = s.find( "MessageBox", p + 1 ) )
         {
            if ( f.literal[p] || (p > 0 && IsIdChar( s[p-1] )) || (p + 10 < s.size() && IsIdChar( s[p+10] )) )
               continue;
            size_t b = p;
            while ( b > 0 && std::isspace( uint8( s[b-1] ) ) )
               --b;
            if ( b >= 3 && s.compare( b - 3, 3, "new" ) == 0 && (b == 3 || !IsIdChar( s[b-4] )) )
               d.messageBoxesAt.push_back( At( f, p ) );
         }
         for ( size_t p = s.find( "Settings" ); p != std::string::npos; p = s.find( "Settings", p + 1 ) )
         {
            if ( f.literal[p] || (p > 0 && (IsIdChar( s[p-1] ) || s[p-1] == '.')) )
               continue;
            size_t i = SkipWs( s, p + 8 );
            if ( i < s.size() && s[i] == '.' )
            {
               i = SkipWs( s, i + 1 );
               if ( s.compare( i, 4, "read" ) == 0 && !(i + 4 < s.size() && IsIdChar( s[i+4] ) && s[i+4] != 'G') )
                  d.settingsReadAt.push_back( At( f, p ) );
            }
         }
      }
   }
};

nlohmann::json Refs( const std::vector<ScriptSourceRef>& v, size_t max = 6 )
{
   nlohmann::json a = nlohmann::json::array();
   for ( size_t i = 0; i < v.size() && i < max; ++i )
      a.push_back( Ref( v[i] ) );
   if ( v.size() > max )
      a.push_back( "... " + std::to_string( v.size() - max ) + " more" );
   return a;
}

// A JSON number as the text Parameters.getReal() parses: the shortest
// round-trip form nlohmann writes ("0.8", "1e-05", "3.0" for a double 3).
std::string NumberText( const nlohmann::json& v )
{
   return v.dump();
}

bool ValueFits( const std::string& type, const nlohmann::json& v, std::string& text )
{
   if ( type == "boolean" )
   {
      if ( !v.is_boolean() )
         return false;
      text = v.get<bool>() ? "true" : "false";
      return true;
   }
   if ( type == "integer" || type == "int64" )
   {
      if ( !v.is_number_integer() )
         return false;
      if ( v.is_number_unsigned() && v.get<unsigned long long>() > (unsigned long long)LLONG_MAX )
         return false;
      const long long x = v.get<long long>();
      if ( type == "integer" && (x < INT_MIN || x > INT_MAX) )
         return false;
      text = std::to_string( x );
      return true;
   }
   if ( type == "unsigned" || type == "uint64" )
   {
      if ( !v.is_number_integer() || (v.is_number_integer() && !v.is_number_unsigned() && v.get<long long>() < 0) )
         return false;
      const unsigned long long x = v.get<unsigned long long>();
      if ( type == "unsigned" && x > 4294967295ull )
         return false;
      text = std::to_string( x );
      return true;
   }
   if ( type == "real" )
   {
      if ( !v.is_number() || (v.is_number_float() && !std::isfinite( v.get<double>() )) )
         return false;
      text = NumberText( v );
      return true;
   }
   if ( type == "string" )
   {
      if ( !v.is_string() )
         return false;
      text = v.get<std::string>();
      return true;
   }
   // presence: has() only -- any scalar, as text.
   if ( v.is_string() )
      text = v.get<std::string>();
   else if ( v.is_boolean() )
      text = v.get<bool>() ? "true" : "false";
   else if ( v.is_number() )
      text = NumberText( v );
   else
      return false;
   return true;
}

const char* TypeWords( const std::string& t )
{
   if ( t == "boolean" ) return "a JSON boolean (true/false)";
   if ( t == "integer" ) return "a whole number (32-bit)";
   if ( t == "int64" ) return "a whole number (64-bit)";
   if ( t == "unsigned" ) return "a non-negative whole number (32-bit)";
   if ( t == "uint64" ) return "a non-negative whole number (64-bit)";
   if ( t == "real" ) return "a number";
   if ( t == "string" ) return "a string";
   if ( t == "mixed" ) return "a value matching one of its getters";
   return "a string, number or boolean (the script only checks that it is present)";
}

} // namespace

ScriptDescription DescribeScriptSource( const String& mainFile, const String& treeRoot, const String& systemIncludeDir )
{
   ScriptDescription d;
   try
   {
      Reader r( d );
      r.root = U8( treeRoot );
      r.sysRoot = systemIncludeDir.IsEmpty() ? std::string() : RealPath8( U8( systemIncludeDir ) );
      const std::string main = U8( mainFile );
      if ( !IsInside( main, r.root ) )
      {
         d.error = "the script is not inside the script tree";
         return d;
      }
      if ( !r.Read( main, true ) || r.files.empty() )
      {
         if ( d.error.empty() )
            d.error = "the script file could not be read";
         return d;
      }
      // TITLE / VERSION: the main file's own #defines (a header must not name the script).
      Defines own = r.defs;
      for ( const auto& kv : r.mainDefines )
         own.raw[kv.first] = kv.second;
      nlohmann::json v;
      if ( r.mainDefines.count( "TITLE" ) > 0 && own.Literal( "TITLE", v ) && v.is_string() )
         d.title = v.get<std::string>();
      if ( r.mainDefines.count( "VERSION" ) > 0 && own.Literal( "VERSION", v ) )
         d.version = v.is_string() ? v.get<std::string>() : v.dump();
      for ( const SrcFile& f : r.files )
         d.files.push_back( f.rel );
      r.Scan();
      d.ok = true;
   }
   catch ( const std::exception& x )
   {
      d.ok = false;
      d.error = std::string( "the source could not be read: " ) + x.what();
   }
   catch ( ... )
   {
      d.ok = false;
      d.error = "the source could not be read";
   }
   return d;
}

std::string ScriptInteraction( const ScriptDescription& d )
{
   const bool targeted = d.SupportsView() || d.SupportsGlobal();
   if ( targeted )
      return d.parameters.empty() ? "headless-when-targeted" : "honours-parameters-when-targeted";
   if ( !d.executeCallsAt.empty() || !d.messageBoxesAt.empty() )
      return "always-interactive";
   return "no-dialog";
}

nlohmann::json ScriptDescriptionJson( const ScriptDescription& d )
{
   nlohmann::json j;
   if ( !d.ok )
   {
      j["error"] = d.error;
      return j;
   }
   if ( !d.title.empty() ) j["title"] = d.title;
   j["featureId"] = d.featureId;
   j["menu"] = d.menu;
   if ( !d.version.empty() ) j["version"] = d.version;
   if ( !d.scriptIdDirective.empty() ) j["scriptIdDirective"] = d.scriptIdDirective;
   if ( !d.info.empty() )
      j["info"] = d.info.size() > 1200 ? d.info.substr( 0, 1200 ) + "..." : d.info;

   nlohmann::json ctx;
   ctx["view"] = d.SupportsView();
   ctx["global"] = d.SupportsGlobal();
   nlohmann::json ev = nlohmann::json::array();
   for ( const ScriptSourceRef& r : d.viewTargetAt )
      if ( ev.size() < 4 ) ev.push_back( Ref( r ) + " Parameters.isViewTarget/targetView" );
   for ( const ScriptSourceRef& r : d.globalTargetAt )
      if ( ev.size() < 8 ) ev.push_back( Ref( r ) + " Parameters.isGlobalTarget" );
   ctx["evidence"] = ev;
   ctx["note"] = d.SupportsView() || d.SupportsGlobal()
      ? "PI Copilot starts a script on a view (Parameters.isViewTarget = true) or globally (Parameters.isGlobalTarget = true); "
        "this script checks which one it got."
      : "The script does not check how it was started; it works like a run from the Script menu (usually on the active image).";
   j["executionContext"] = ctx;

   const std::string kind = ScriptInteraction( d );
   nlohmann::json in;
   in["kind"] = kind;
   std::string summary;
   if ( kind == "honours-parameters-when-targeted" )
      summary = "It checks how it was started and reads its parameters: run by PI Copilot (always with a view or "
                "global target) it is written to use the arguments passed and skip its main dialog. Message boxes it "
                "shows (errors, warnings) still wait for the user.";
   else if ( kind == "headless-when-targeted" )
      summary = "It checks how it was started but reads no parameters: on a target it runs with its own settings, "
                "without its main dialog. Message boxes it shows still wait for the user.";
   else if ( kind == "always-interactive" )
      summary = "It does not check how it was started: expect its dialog to open and wait for the user; arguments "
                "cannot configure it.";
   else
      summary = "No dialog or message box was found in its source: it runs to the end without asking.";
   in["summary"] = summary;
   in["executeCalls"] = d.executeCallsAt.size();
   in["executeCallsAt"] = Refs( d.executeCallsAt );
   in["messageBoxes"] = d.messageBoxesAt.size();
   in["note"] = "Inferred from the source text: '.execute()' is how a Dialog or MessageBox is shown (some scripts also "
                "name their own methods execute()).";
   j["interaction"] = in;

   nlohmann::json ps = nlohmann::json::array();
   for ( const ScriptParameter& p : d.parameters )
   {
      nlohmann::json e = { { "name", CleanUtf8( p.name ) }, { "type", p.type }, { "getters", p.getters },
                           { "readAt", Refs( p.at, 4 ) } };
      if ( !p.viaDefine.empty() )
         e["keyFromDefine"] = p.viaDefine;
      if ( p.hasDefault )
      {
         e["default"] = p.defaultValue;
         e["defaultFrom"] = Ref( p.defaultAt );
      }
      else if ( !p.defaultNote.empty() )
         e["defaultNote"] = p.defaultNote;
      ps.push_back( e );
   }
   j["parameters"] = ps;
   nlohmann::json un = nlohmann::json::array();
   for ( const ScriptUnresolvedRead& u : d.unresolved )
      un.push_back( { { "getter", u.getter }, { "key", "unresolved" }, { "expression", u.expression }, { "at", Ref( u.at ) } } );
   j["unresolvedParameterReads"] = un;
   if ( !d.settingsReadAt.empty() )
      j["keepsLastUsedValues"] = "It reads its own saved settings (Settings.read at " + Ref( d.settingsReadAt.front() )
                               + "): a parameter you do not pass may take the user's last-used value rather than the "
                                 "default shown.";
   j["files"] = d.files;
   if ( !d.includesNotFollowed.empty() )
      j["includesNotFollowed"] = d.includesNotFollowed;
   if ( d.limitHit )
      j["limit"] = "The script and its includes are larger than PI Copilot reads; the description may be incomplete.";
   j["method"] = "Read from the script's source text (comments ignored, its #include files in the script tree followed, "
                 "simple #define constants resolved). Nothing was executed. Keys built at run time are listed as "
                 "unresolved, never guessed.";
   return j;
}

// Measured (self-test S5b, PixInsight 1.9.5 build 1706): every printable
// ASCII character arrives intact EXCEPT the double quote (it ends the value:
// x" -p="inj,1 adds a parameter "inj"; an odd count leaves the core's console
// in an unterminated string, and the instance then hangs) and the backslash
// (a trailing one escapes the closing quote: "Unterminated string literal").
// $ is escaped by the core itself; ; | ` ' & % are literal. The allowlist keeps
// what file names and text need, each character proven in S5b on every run.
const char* const kSafeArgumentPunctuation = "._-+:/,=@#()[]{}!~^'&%;";

bool ScriptArgumentCharAllowed( uint32 c )
{
   if ( c < 0x80 )
      return std::isalnum( int( c ) ) || c == ' ' || (c != 0 && std::strchr( kSafeArgumentPunctuation, int( c ) ) != nullptr);
   // Non-ASCII: ordinary visible characters of the Basic Multilingual Plane
   // (accented letters, CJK, ...); never controls, format/bidi/invisible
   // characters or surrogates. Not above U+FFFF: PCL re-encodes those wrongly
   // on the way (measured in S5b: U+1F4F7 arrived as U+1F4FD).
   return c <= 0xFFFF && UnsafeDisplayKind( c ) == nullptr;
}

String ValidateScriptArguments( const ScriptDescription& d, const nlohmann::json& args,
                                StringKeyValueList& out, nlohmann::json& shown )
{
   out.Clear();
   shown = nlohmann::json::object();
   if ( args.is_null() )
      return String();
   if ( !args.is_object() )
      return "arguments must be an object {\"parameterName\": value, ...}";
   if ( args.size() > 64 )
      return "at most 64 arguments";
   std::string valid;
   for ( const ScriptParameter& p : d.parameters )
      valid += (valid.empty() ? "" : ", ") + p.name + " (" + p.type + ")";
   for ( auto it = args.begin(); it != args.end(); ++it )
   {
      const std::string& key = it.key();
      const ScriptParameter* p = nullptr;
      for ( const ScriptParameter& q : d.parameters )
         if ( q.name == key )
            p = &q;
      if ( p == nullptr )
         return FromU8( "the script reads no parameter named \"" + CleanUtf8( key ) + "\". "
                        + (valid.empty() ? std::string( "It reads no parameters PI Copilot could find in its source" )
                                         : "Valid parameters: " + valid)
                        + (d.unresolved.empty() ? std::string()
                                                : ". It also reads " + std::to_string( d.unresolved.size() )
                                                  + " parameter(s) whose names are built at run time; those cannot be passed")
                        + ". Nothing ran." );
      std::string text;
      bool fits = false;
      if ( p->type == "mixed" )
      {
         for ( const std::string& g : p->getters )
         {
            const std::string t = GetterType( g );
            if ( !t.empty() && ValueFits( t, it.value(), text ) )
            {
               fits = true;
               break;
            }
         }
      }
      else
         fits = ValueFits( p->type, it.value(), text );
      if ( !fits )
         return FromU8( "argument \"" + key + "\" must be " + TypeWords( p->type ) + " (the script reads it with "
                        + p->getters.back() + "()); got " + it.value().dump() + ". Nothing ran." );
      // PixInsight hands script arguments to its script runner as a command
      // line (measured: run -x -p="key,value" ... "<file>"). ALLOWLIST, never a
      // blocklist: the name is [A-Za-z0-9_] (every installed script's are), and
      // every character of the value is one ScriptArgumentCharAllowed() admits
      // -- each of those is proven by self-test S5b to arrive in Parameters
      // unchanged, in any position, without disturbing the other arguments.
      for ( char ch : key )
         if ( !(std::isalnum( uint8( ch ) ) || ch == '_') )
            return FromU8( "the script's parameter name \"" + CleanUtf8( key ) + "\" has characters PixInsight cannot pass "
                           "safely on its script command line; it cannot be set from PI Copilot. Nothing ran." );
      if ( text.size() > 4096 )
         return FromU8( "argument \"" + key + "\" is longer than 4096 characters. Nothing ran." );
      const String t16 = FromU8( text );
      for ( size_type i = 0; i < t16.Length(); )
      {
         size_type units = 1;
         const uint32 c = CodePointAt( t16, i, units );
         if ( !ScriptArgumentCharAllowed( c ) )
            return FromU8( "argument \"" + key + "\" contains " ) + DisplayCharacter( c )
                 + String( ", which PI Copilot does not pass to a script: PixInsight hands arguments over on a command "
                           "line, so only letters, digits, spaces and " ) + String( kSafeArgumentPunctuation )
                 + " are allowed. Nothing ran.";
         i += units;
      }
      out << StringKeyValue( FromU8( key ), t16 );
      shown[key] = it.value();
   }
   return String();
}

} // namespace pcl
