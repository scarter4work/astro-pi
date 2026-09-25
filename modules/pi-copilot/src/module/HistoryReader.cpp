// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "HistoryReader.h"
#include "EvalGuard.h"
#include "JourneyConstants.h"
#include "PICopilotModule.h"
#include "PjsrRunner.h"   // ScriptLiteral
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>

#include <algorithm>
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

// One XPSM value in the apply_process JSON form of parameter p.
bool TypedValue( const ProcessParameter& p, const XMLElement& e, nlohmann::json& v, std::string& note )
{
   const String raw = RawValue( e );
   const std::string id = std::string( p.Id().c_str() );
   if ( p.IsBoolean() )
   {
      v = raw.Trimmed() == "true";
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
      char* end = nullptr;
      if ( p.IsInteger() )
      {
         const long long x = std::strtoll( s.c_str(), &end, 10 );
         if ( end == s.c_str() || *end != '\0' )
         {
            note = "parameter " + id + " has a non-integer value '" + s + "'";
            return false;
         }
         v = x;
      }
      else
      {
         const double x = std::strtod( s.c_str(), &end );
         if ( end == s.c_str() || *end != '\0' )
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
            const std::string span = U8( e.AttributeValue( "span" ) );
            step.durationS = span.empty() ? -1 : std::strtod( span.c_str(), nullptr );
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
      const String js = String(
         "(function( id, from, extraSteps, extraId, extraLeads ){"
         " var v = null;"
         " try { v = View.viewById( id ); } catch ( e ) { v = null; }"
         " if ( v == null || v.isNull ) return JSON.stringify( { error: \"no view \" + id } );"
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
         "   r.steps.push( { xpsm: pc.at( i ).toSource( \"XPSM 1.0\" ), maskId: m, maskInverted: inv } ); }"
         " return JSON.stringify( r ); })( " )
         + String( ScriptLiteral( String( viewFullId ) ).c_str() )
         + String().Format( ", %d, %d, ", s.from, PICopilotJourneyReopenExtraSteps )
         + String( ScriptLiteral( String( PICopilotJourneyReopenExtraProcessId ) ).c_str() )
         + (PICopilotJourneyReopenExtraLeads ? ", true )" : ", false )");
      String r;
      {
         EvalDepthGuard guard;
         r = ThePICopilotModule->EvaluateScript( js, "JavaScript" ).ToString();
      }
      const nlohmann::json j = nlohmann::json::parse( U8( r ) );
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
