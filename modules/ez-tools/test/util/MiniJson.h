// EZTools tests -- a minimal JSON reader for the golden files (objects, arrays, numbers, strings,
// true/false/null). Test-only; throws std::runtime_error on malformed input.

#ifndef EZTOOLS_TEST_MINIJSON_H
#define EZTOOLS_TEST_MINIJSON_H

#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace eztools::test
{

struct Json
{
   enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
   bool b = false;
   double num = 0;
   std::string str;
   std::vector<Json> arr;
   std::map<std::string, Json> obj;

   const Json& operator[]( const std::string& k ) const
   {
      auto it = obj.find( k );
      if ( kind != Object || it == obj.end() )
         throw std::runtime_error( "json: no key '" + k + "'" );
      return it->second;
   }
   const Json& operator[]( size_t i ) const
   {
      if ( kind != Array || i >= arr.size() )
         throw std::runtime_error( "json: bad index" );
      return arr[i];
   }
   size_t size() const { return kind == Array ? arr.size() : obj.size(); }
   double Num() const { return num; }
   std::vector<double> Nums() const
   {
      std::vector<double> v;
      for ( const auto& e : arr )
         v.push_back( e.num );
      return v;
   }
};

class JsonParser
{
public:
   explicit JsonParser( const std::string& s ) : m_s( s ) {}
   Json Parse()
   {
      Json j = Value();
      Ws();
      if ( m_p != m_s.size() )
         Fail( "trailing data" );
      return j;
   }

private:
   const std::string& m_s;
   size_t m_p = 0;

   [[noreturn]] void Fail( const char* what ) const
   {
      throw std::runtime_error( std::string( "json: " ) + what + " at " + std::to_string( m_p ) );
   }
   void Ws()
   {
      while ( m_p < m_s.size() && ( m_s[m_p] == ' ' || m_s[m_p] == '\n' || m_s[m_p] == '\r' || m_s[m_p] == '\t' ) )
         ++m_p;
   }
   bool Eat( char c )
   {
      Ws();
      if ( m_p < m_s.size() && m_s[m_p] == c )
      {
         ++m_p;
         return true;
      }
      return false;
   }
   std::string Str()
   {
      if ( !Eat( '"' ) )
         Fail( "expected string" );
      std::string out;
      while ( m_p < m_s.size() && m_s[m_p] != '"' )
      {
         char c = m_s[m_p++];
         if ( c == '\\' && m_p < m_s.size() )
         {
            char e = m_s[m_p++];
            switch ( e )
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'u': m_p += 4; out += '?'; break;   // not used by the goldens
            default:  out += e; break;
            }
         }
         else
            out += c;
      }
      if ( m_p >= m_s.size() )
         Fail( "unterminated string" );
      ++m_p;
      return out;
   }
   Json Value()
   {
      Ws();
      if ( m_p >= m_s.size() )
         Fail( "unexpected end" );
      Json j;
      char c = m_s[m_p];
      if ( c == '{' )
      {
         ++m_p;
         j.kind = Json::Object;
         if ( Eat( '}' ) )
            return j;
         do
         {
            std::string k = Str();
            if ( !Eat( ':' ) )
               Fail( "expected ':'" );
            j.obj[k] = Value();
         }
         while ( Eat( ',' ) );
         if ( !Eat( '}' ) )
            Fail( "expected '}'" );
      }
      else if ( c == '[' )
      {
         ++m_p;
         j.kind = Json::Array;
         if ( Eat( ']' ) )
            return j;
         do
            j.arr.push_back( Value() );
         while ( Eat( ',' ) );
         if ( !Eat( ']' ) )
            Fail( "expected ']'" );
      }
      else if ( c == '"' )
      {
         j.kind = Json::String;
         j.str = Str();
      }
      else if ( m_s.compare( m_p, 4, "true" ) == 0 )
      {
         m_p += 4;
         j.kind = Json::Bool;
         j.b = true;
      }
      else if ( m_s.compare( m_p, 5, "false" ) == 0 )
      {
         m_p += 5;
         j.kind = Json::Bool;
      }
      else if ( m_s.compare( m_p, 4, "null" ) == 0 )
         m_p += 4;
      else
      {
         const char* begin = m_s.c_str() + m_p;
         char* end = nullptr;
         j.kind = Json::Number;
         j.num = std::strtod( begin, &end );
         if ( end == begin )
            Fail( "bad value" );
         m_p += size_t( end - begin );
      }
      return j;
   }
};

inline Json LoadJson( const std::string& path )
{
   std::ifstream f( path );
   if ( !f )
      throw std::runtime_error( "cannot open " + path );
   std::stringstream ss;
   ss << f.rdbuf();
   std::string s = ss.str();
   return JsonParser( s ).Parse();
}

} // namespace eztools::test

#endif
