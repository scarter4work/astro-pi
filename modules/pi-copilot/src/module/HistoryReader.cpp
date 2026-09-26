// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "HistoryReader.h"
#include "EvalGuard.h"
#include "JourneyConstants.h"
#include "PjsrRunner.h"   // ScriptLiteral
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

namespace pcl
{

namespace
{

String RawValue( const XMLElement& e )
{
   return e.HasAttribute( "value" ) ? e.AttributeValue( "value" ) : e.Text();
}

// Locale-independent numeric parsing (std::from_chars never consults
// LC_NUMERIC; strtod would stop at the '.' under a comma-decimal locale). The
// whole text must be consumed; one leading '+' is accepted.
bool ParseInt64C( const std::string& s, long long& x )
{
   const char* b = s.data();
   const char* e = s.data() + s.size();
   if ( b != e && *b == '+' )
      ++b;
   if ( b == e )
      return false;
   const std::from_chars_result r = std::from_chars( b, e, x, 10 );
   return r.ec == std::errc() && r.ptr == e;
}

bool ParseDoubleC( const std::string& s, double& x )
{
   const char* b = s.data();
   const char* e = s.data() + s.size();
   if ( b != e && *b == '+' )
      ++b;
   if ( b == e )
      return false;
   const std::from_chars_result r = std::from_chars( b, e, x, std::chars_format::general );
   return r.ec == std::errc() && r.ptr == e;
}

// One XPSM value in the apply_process JSON form of parameter p.
bool TypedValue( const ProcessParameter& p, const XMLElement& e, nlohmann::json& v, std::string& note )
{
   const String raw = RawValue( e );
   const std::string id = std::string( p.Id().c_str() );
   if ( p.IsBoolean() )
   {
      const String b = raw.Trimmed();
      if ( b != "true" && b != "false" )
      {
         note = "parameter " + id + " has a non-boolean value '" + U8( b ) + "'";
         return false;
      }
      v = b == "true";
      return true;
   }
   if ( p.IsEnumeration() )
   {
      v = U8( raw.Trimmed() );
      return true;
   }
   if ( p.IsNumeric() )
   {
      const std::string s = U8( raw.Trimmed() );
      if ( p.IsInteger() )
      {
         long long x = 0;
         if ( !ParseInt64C( s, x ) )
         {
            note = "parameter " + id + " has a non-integer value '" + s + "'";
            return false;
         }
         v = x;
      }
      else
      {
         double x = 0;
         if ( !ParseDoubleC( s, x ) )
         {
            note = "parameter " + id + " has a non-numeric value '" + s + "'";
            return false;
         }
         v = x;
      }
      return true;
   }
   if ( p.IsString() )
   {
      v = U8( raw );
      return true;
   }
   note = "parameter " + id + " holds block data, which PI Copilot cannot replay";
   return false;
}

void NotReplayable( HistoryStep& step, const std::string& note )
{
   if ( step.replayable )
   {
      step.replayable = false;
      step.parseNote = note;
   }
}

} // namespace

std::string Fnv1a64Hex( const std::string& s )
{
   uint64_t h = 14695981039346656037ull;   // FNV-64 offset basis (0xcbf29ce484222325)
   for ( unsigned char c : s )
   {
      h ^= c;
      h *= 1099511628211ull;
   }
   char buf[17];
   std::snprintf( buf, sizeof buf, "%016llx", static_cast<unsigned long long>( h ) );
   return buf;
}

std::string StepIdentity( const std::string& processId, const std::string& started,
                          const nlohmann::json& parameters, const nlohmann::json& tableParameters )
{
   const nlohmann::json canonical = { { "p", parameters }, { "t", tableParameters } };   // std::map keys: sorted
   return processId + "@" + started + "#" + Fnv1a64Hex( canonical.dump() );
}

bool ParseXpsmElement( const XMLElement& root, HistoryStep& step, String& error )
{
   try
   {
      if ( root.Name() != "instance" )
      {
         error = "not an XPSM instance element: <" + root.Name() + ">";
         return false;
      }
      step.processId = U8( root.AttributeValue( "class" ) );
      if ( step.processId.empty() )
      {
         error = "XPSM instance without a class attribute";
         return false;
      }
      step.parameters = nlohmann::json::object();
      step.tableParameters = nlohmann::json::object();
      step.replayable = true;
      step.parseNote.clear();
      step.integrationImageId.clear();

      std::unique_ptr<Process> P;
      try
      {
         P.reset( new Process( IsoString( step.processId.c_str() ) ) );
      }
      catch ( ... )
      {
         NotReplayable( step, "process " + step.processId + " is not installed in this PixInsight" );
      }
      if ( step.processId == "Script" )
         NotReplayable( step, "a script step: PI Copilot never re-runs scripts; the user runs it by hand" );

      for ( const XMLElement& e : root.ChildElements() )
      {
         if ( e.Name() == "time" )
         {
            step.started = U8( e.AttributeValue( "start" ) );
            const std::string span = U8( e.AttributeValue( "span" ).Trimmed() );
            step.durationS = -1;
            if ( !span.empty() && !ParseDoubleC( span, step.durationS ) )
            {
               error = "XPSM step " + String( step.processId.c_str() ) + ": <time span=\"" + FromU8( span )
                     + "\"> is not a number";
               return false;
            }
            continue;
         }
         const std::string id = U8( e.AttributeValue( "id" ) );
         if ( e.Name() == "parameter" )
         {
            // Ruling 29: captured raw, before the read-only drop below.
            if ( id == "integrationImageId" )
               step.integrationImageId = U8( RawValue( e ).Trimmed() );
            if ( !P )
            {
               step.parameters[id] = U8( RawValue( e ) );
               continue;
            }
            std::unique_ptr<ProcessParameter> p;
            try { p.reset( new ProcessParameter( *P, IsoString( id.c_str() ) ) ); } catch ( ... ) {}
            if ( !p || p->IsNull() )
            {
               NotReplayable( step, "parameter " + id + " is unknown to the installed " + step.processId );
               continue;
            }
            if ( p->IsReadOnly() )
               continue;
            nlohmann::json v;
            std::string note;
            if ( TypedValue( *p, e, v, note ) )
               step.parameters[id] = v;
            else
               NotReplayable( step, note );
         }
         else if ( e.Name() == "table" )
         {
            nlohmann::json rows = nlohmann::json::array();
            if ( !P )
            {
               for ( const XMLElement& tr : e.ChildElements() )
               {
                  nlohmann::json row = nlohmann::json::object();
                  for ( const XMLElement& td : tr.ChildElements() )
                     row[U8( td.AttributeValue( "id" ) )] = U8( RawValue( td ) );
                  rows.push_back( row );
               }
               step.tableParameters[id] = rows;
               continue;
            }
            std::unique_ptr<ProcessParameter> t;
            try { t.reset( new ProcessParameter( *P, IsoString( id.c_str() ) ) ); } catch ( ... ) {}
            if ( !t || t->IsNull() || !t->IsTable() )
            {
               NotReplayable( step, "table " + id + " is unknown to the installed " + step.processId );
               continue;
            }
            if ( t->IsReadOnly() )
               continue;
            const ProcessParameter::parameter_list cols = t->TableColumns();
            for ( const XMLElement& tr : e.ChildElements() )
            {
               if ( tr.Name() != "tr" )
                  continue;
               nlohmann::json row = nlohmann::json::array();
               for ( const ProcessParameter& c : cols )
               {
                  const XMLElement* cell = nullptr;
                  for ( const XMLElement& td : tr.ChildElements() )
                     if ( td.AttributeValue( "id" ) == String( c.Id() ) )
                     {
                        cell = &td;
                        break;
                     }
                  nlohmann::json v;
                  std::string note;
                  if ( cell == nullptr )
                  {
                     NotReplayable( step, "table " + id + " row lacks column " + std::string( c.Id().c_str() ) );
                     v = nullptr;
                  }
                  else if ( !TypedValue( c, *cell, v, note ) )
                  {
                     NotReplayable( step, note );
                     v = nullptr;
                  }
                  row.push_back( v );
               }
               rows.push_back( row );
            }
            step.tableParameters[id] = rows;
         }
      }
      step.identity = StepIdentity( step.processId, step.started, step.parameters, step.tableParameters );
      return true;
   }
   catch ( const pcl::Exception& x )
   {
      error = "XPSM step: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      error = "XPSM step: " + String( x.what() );
   }
   return false;
}

bool ParseXpsmStep( const std::string& xpsm, HistoryStep& step, String& error )
{
   try
   {
      XMLDocument doc;
      doc.SetParserOption( XMLParserOption::IgnoreComments );
      doc.Parse( FromU8( xpsm ) );
      const XMLElement* root = doc.RootElement();
      if ( root == nullptr )
      {
         error = "XPSM step: no root element";
         return false;
      }
      if ( !ParseXpsmElement( *root, step, error ) )
         return false;
      step.xpsm = xpsm;
      return true;
   }
   catch ( const pcl::Exception& x )
   {
      error = "XPSM step: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      error = "XPSM step: " + String( x.what() );
   }
   return false;
}

HistorySnapshot ReadViewHistory( const IsoString& viewFullId, int from )
{
   HistorySnapshot s;
   s.from = std::max( 0, from );
   if ( EvaluateScriptDepth() > 0 )
   {
      s.busy = true;
      return s;
   }
   try
   {
      // All ASCII: fixed text + ScriptLiteral()s + integers.
      const std::string js = std::string(
         "(function( id, from, extraSteps, extraId, extraLeads ){"
         " var v = null;"
         " try { v = View.viewById( id ); } catch ( e ) { v = null; }"
         " if ( v == null || v.isNull ) return { error: \"no view \" + id };"
         " var ip = v.initialProcessing, p = v.processing;"
         // Ruling 27: skip the reopen's extra entry (measured in Task 1).
         " var dropAt = -1;"
         " if ( extraSteps == 1 && ip.length > 0 && v.window.filePath.length > 0 ) {"
         "   var k = extraLeads ? 0 : ip.length - 1;"
         "   if ( ip.at( k ).processId() == extraId ) dropAt = k; }"
         " var il = ip.length - (dropAt >= 0 ? 1 : 0);"
         " var r = { initialLength: il, length: p.length, historyIndex: v.historyIndex, dropped: dropAt >= 0, steps: [] };"
         " for ( var c = from; c < il + p.length; ++c ) {"
         "   var pc = c < il ? ip : p, i = c < il ? (dropAt == 0 ? c + 1 : c) : c - il;"
         "   var m = \"\"; try { m = String( pc.maskId( i ) ); } catch ( e ) { m = \"\"; }"
         "   var inv = false; try { inv = pc.maskInverted( i ) == true; } catch ( e ) { inv = false; }"
         "   r.steps.push( { xpsm: pcWell( pc.at( i ).toSource( \"XPSM 1.0\" ) ), maskId: pcWell( m ), maskInverted: inv } ); }"
         " return r; })( " )
         + ScriptLiteral( String( viewFullId ) )
         + ", " + std::to_string( s.from ) + ", " + std::to_string( PICopilotJourneyReopenExtraSteps ) + ", "
         + ScriptLiteral( String( PICopilotJourneyReopenExtraProcessId ) )
         + (PICopilotJourneyReopenExtraLeads ? ", true )" : ", false )");
      // The shared ASCII-safe path (PjsrRunner): the result crosses
      // EvaluateScript as pcAscii() text, so user data in a step (an
      // expression, a path, a FITS value) is never re-encoded on the way, and
      // it holds the EvalDepthGuard.
      const nlohmann::json j = EvaluateAsciiJson( "return pcAscii( JSON.stringify( " + js + " ) );" );
      if ( j.contains( "error" ) )
      {
         s.error = FromU8( j.at( "error" ).get<std::string>() );
         return s;
      }
      s.initialLength = j.at( "initialLength" ).get<int>();
      s.length = j.at( "length" ).get<int>();
      s.historyIndex = j.at( "historyIndex" ).get<int>();
      s.droppedReopenExtra = j.at( "dropped" ).get<bool>();
      int c = s.from;
      for ( const nlohmann::json& st : j.at( "steps" ) )
      {
         HistoryStep h;
         String e;
         if ( !ParseXpsmStep( st.at( "xpsm" ).get<std::string>(), h, e ) )
         {
            s.error = String().Format( "history step %d of ", c + 1 ) + String( viewFullId ) + ": " + e;
            s.steps.clear();
            return s;
         }
         h.combinedIndex = c++;
         h.maskId = st.at( "maskId" ).get<std::string>();
         h.maskInverted = st.at( "maskInverted" ).get<bool>();
         s.steps.push_back( std::move( h ) );
      }
      s.ok = true;
   }
   catch ( const pcl::Exception& x )
   {
      s.error = "history read of " + String( viewFullId ) + " failed: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      s.error = "history read of " + String( viewFullId ) + " failed: " + String( x.what() );
   }
   if ( !s.ok )
      s.steps.clear();
   return s;
}

int HistoryReadFrom( const std::vector<KnownStep>& known )
{
   int maxSeq = 0;
   for ( const KnownStep& k : known )
      if ( k.state != "superseded" )
         maxSeq = std::max( maxSeq, k.seq );
   return std::max( 0, maxSeq - 1 );
}

HistoryDiff DiffHistory( const std::vector<KnownStep>& known, const HistorySnapshot& snap )
{
   HistoryDiff d;
   // A busy or failed read says nothing about the history: no state change
   // (never "supersede every row"). The caller retries on its next tick.
   if ( !snap.ok )
      return d;
   std::map<int, const KnownStep*> bySeq;
   for ( const KnownStep& k : known )
      if ( k.state != "superseded" )
         bySeq[k.seq] = &k;
   const int total = snap.TotalCount();
   const int active = snap.ActiveCount();

   // PI only truncates at historyIndex and appends, so when the step re-read
   // at snap.from matches, every earlier index is unchanged (Ruling 5).
   if ( snap.from > 0 )
   {
      auto it = bySeq.find( snap.from + 1 );
      if ( snap.steps.empty() || it == bySeq.end() || it->second->identity != snap.steps.front().identity )
      {
         d.needFullRead = true;
         return d;
      }
   }

   int m = total;   // first mismatching combined index
   for ( const HistoryStep& s : snap.steps )
   {
      auto it = bySeq.find( s.combinedIndex + 1 );
      if ( it == bySeq.end() || it->second->identity != s.identity )
      {
         m = s.combinedIndex;
         break;
      }
   }

   for ( const auto& kv : bySeq )
   {
      const KnownStep& k = *kv.second;
      if ( k.seq > m )
         d.toSuperseded.push_back( k.id );
      else
      {
         const bool wantActive = k.seq <= active;
         if ( wantActive && k.state != "active" )
            d.toActive.push_back( k.id );
         else if ( !wantActive && k.state != "undone" )
            d.toUndone.push_back( k.id );
      }
   }
   for ( const HistoryStep& s : snap.steps )
      if ( s.combinedIndex >= m )
      {
         d.appended.push_back( s );
         d.appendedState.push_back( s.combinedIndex + 1 <= active ? "active" : "undone" );
      }
   return d;
}

} // namespace pcl
