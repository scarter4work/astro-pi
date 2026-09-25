// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessSafety.h"
#include "ProcessCatalog.h"
#include "ProcessSafetyData.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/StringList.h>
#include <pcl/Variant.h>

#include <cctype>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace pcl
{

namespace
{

const nlohmann::json* g_override = nullptr;

const char* const kSections[] = { "deny", "confirmAlways", "confirmWhen", "reviewedSafe" };

String S16( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

const nlohmann::json& Section( const nlohmann::json& policy, const char* name )
{
   static const nlohmann::json empty = nlohmann::json::object();
   const auto it = policy.find( name );
   return (it != policy.end() && it->is_object()) ? *it : empty;
}

// Words of a camelCase / snake_case / digit-separated id, lower-cased:
// "outputXEPHFilePath" -> output, xeph, file, path.
std::vector<std::string> IdWords( const std::string& id )
{
   std::vector<std::string> words;
   std::string w;
   auto flush = [&]() { if ( !w.empty() ) { words.push_back( w ); w.clear(); } };
   for ( size_t i = 0; i < id.size(); ++i )
   {
      const char c = id[i];
      if ( !std::isalpha( static_cast<unsigned char>( c ) ) )
      {
         flush();
         continue;
      }
      if ( std::isupper( static_cast<unsigned char>( c ) ) && !w.empty() )
      {
         const bool prevLower = std::islower( static_cast<unsigned char>( id[i-1] ) ) != 0;
         const bool nextLower = i+1 < id.size() && std::islower( static_cast<unsigned char>( id[i+1] ) ) != 0;
         if ( prevLower || nextLower )   // "fileP|ath", "XEP|H|File"
            flush();
      }
      w += char( std::tolower( static_cast<unsigned char>( c ) ) );
   }
   flush();
   return words;
}

// THE side-effect heuristic (one definition: the coverage self-test and the
// runtime check for unreviewed processes both use it). Substrings are
// unambiguous anywhere in an id; the short words only match as whole words
// ("app" must not match "mapping", "move" is covered by "remove" only as a word).
bool LooksLikeSideEffect( const std::string& id )
{
   static const std::regex re( "output|directory|dir$|file|path|overwrite|write|save|log|cache|delete|remove|close|"
                               "script|command|url|exec|folder|destination|export|rename|database|server|program|launch",
                               std::regex::icase );
   if ( std::regex_search( id, re ) )
      return true;
   static const char* const kWords[] = { "dir", "dest", "move", "copy", "db", "host", "app", "application" };
   for ( const std::string& w : IdWords( id ) )
      for ( const char* k : kWords )
         if ( w == k )
            return true;
   return false;
}

// Parameter and table-column ids ("table.column") of P that match the heuristic.
nlohmann::json SideEffectParameterIds( const Process& P )
{
   nlohmann::json hits = nlohmann::json::array();
   for ( const ProcessParameter& p : P.Parameters() )
   {
      const std::string pid( p.Id().c_str() );
      if ( LooksLikeSideEffect( pid ) )
         hits.push_back( pid );
      if ( p.IsTable() )
         for ( const ProcessParameter& c : p.TableColumns() )
         {
            const std::string cid( c.Id().c_str() );
            if ( LooksLikeSideEffect( cid ) )
               hits.push_back( pid + "." + cid );
         }
   }
   return hits;
}

bool Classified( const nlohmann::json& policy, const std::string& id )
{
   for ( const char* s : kSections )
      if ( Section( policy, s ).contains( id ) )
         return true;
   return false;
}

// " (id = value)" / " (id = value, the process default)" for a confirm reason.
String Trigger( const std::string& param, const nlohmann::json& value, bool fromDefault )
{
   return S16( " (" + param + " = " + value.dump() + (fromDefault ? ", the process default)" : ")") );
}

// An enumeration value (element id string, element alias string, or element
// integer value) as its canonical element id; the value itself when it names
// no element (ApplyProcess then rejects it precisely).
nlohmann::json CanonicalEnum( const ProcessParameter& p, const nlohmann::json& v )
{
   const ProcessParameter::enumeration_element_list& elements = EnumerationInfoOf( p ).elements;
   if ( v.is_string() )
   {
      const IsoString want( v.get<std::string>().c_str() );
      for ( const ProcessParameter::EnumerationElement& e : elements )
      {
         if ( e.id == want )
            return std::string( e.id.c_str() );
         for ( const IsoString& a : e.aliases )
            if ( a.Trimmed() == want )
               return std::string( e.id.c_str() );
      }
   }
   else if ( v.is_number_integer() )
   {
      for ( const ProcessParameter::EnumerationElement& e : elements )
         if ( e.value == v.get<int64_t>() )
            return std::string( e.id.c_str() );
   }
   return v;
}

// The values a run will use for the parameter `rule` names: every given value
// whose key the core resolves to that parameter (its id or an alias), else the
// process default. Throws when `rule` is not a parameter of P.
nlohmann::json EffectiveValues( const Process& P, const std::string& rule, const nlohmann::json& parameters,
                                bool& fromDefault )
{
   fromDefault = false;
   const ProcessParameter p( P, IsoString( rule.c_str() ) );
   const IsoString canonical = p.Id();
   nlohmann::json values = nlohmann::json::array();
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
      {
         try
         {
            if ( ProcessParameter( P, IsoString( it.key().c_str() ) ).Id() != canonical )
               continue;
         }
         catch ( ... )
         {
            continue;   // unknown parameter: ApplyProcess reports it precisely
         }
         values.push_back( p.IsEnumeration() ? CanonicalEnum( p, it.value() ) : it.value() );
      }
   if ( !values.empty() )
      return values;
   fromDefault = true;

   ProcessInstance d( P );
   const Variant v = d.ParameterValue( p, 0 );   // row 0, never ~0 (inc-4 Task 1 modal)
   if ( p.IsBoolean() )
      values.push_back( v.ToBoolean() );
   else if ( p.IsEnumeration() )
      values.push_back( CanonicalEnum( p, nlohmann::json( int64_t( v.ToInt() ) ) ) );
   else if ( p.IsNumeric() )
      values.push_back( v.ToDouble() );
   else if ( p.IsString() )
      values.push_back( U8( v.ToString() ) );
   else
      values.push_back( nullptr );
   return values;
}

bool Matches( const nlohmann::json& got, const nlohmann::json& equals )
{
   if ( got == equals )
      return true;
   return got.is_number() && equals.is_number() && got.get<double>() == equals.get<double>();
}

} // namespace

const nlohmann::json& CompiledProcessSafety()
{
   static const nlohmann::json policy = nlohmann::json::parse( kProcessSafetyJson );
   return g_override != nullptr ? *g_override : policy;
}

void SetProcessSafetyPolicyForSelfTest( const nlohmann::json* policy )
{
   g_override = policy;
}

SafetyVerdict CheckProcessSafety( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& /*tableParameters*/ )
{
   SafetyVerdict v;
   std::unique_ptr<Process> P;
   try
   {
      P.reset( new Process( processId ) );
   }
   catch ( ... )
   {
      return v;   // unknown process id: Allow here; the executor's own error names it precisely
   }
   try
   {
      const std::string id( P->Id().c_str() );
      const nlohmann::json& policy = CompiledProcessSafety();
      const nlohmann::json& deny = Section( policy, "deny" );
      if ( deny.contains( id ) )
      {
         v.kind = SafetyVerdict::Deny;
         v.reason = S16( deny[id].get<std::string>() );
         return v;
      }
      const nlohmann::json& always = Section( policy, "confirmAlways" );
      if ( always.contains( id ) )
      {
         v.kind = SafetyVerdict::Confirm;
         v.reason = S16( always[id].get<std::string>() );
         return v;
      }
      const nlohmann::json& when = Section( policy, "confirmWhen" );
      if ( when.contains( id ) && when[id].is_array() )
      {
         StringList reasons;
         for ( const nlohmann::json& rule : when[id] )
         {
            const std::string param = rule.value( "parameter", std::string() );
            // "equals": ask when the value is this; "notEquals": ask when it is anything else.
            const bool negated = rule.contains( "notEquals" );
            const nlohmann::json equals = negated ? rule["notEquals"] : rule.value( "equals", nlohmann::json() );
            nlohmann::json values;
            bool fromDefault = false;
            try
            {
               values = EffectiveValues( *P, param, parameters, fromDefault );
            }
            catch ( ... )
            {
               // A rule naming a parameter this process does not have is a
               // policy error: ask rather than run unchecked.
               reasons << "the safety policy names an unknown parameter " + S16( id + "." + param )
                          + ", so this run cannot be checked";
               continue;
            }
            for ( const nlohmann::json& got : values )
               if ( Matches( got, equals ) != negated )
               {
                  reasons << S16( rule.value( "reason", std::string( "a parameter with side effects is set" ) ) )
                             + Trigger( param, got, fromDefault );
                  break;
               }
         }
         if ( !reasons.IsEmpty() )
         {
            v.kind = SafetyVerdict::Confirm;
            for ( size_type i = 0; i < reasons.Length(); ++i )
               v.reason += (i > 0 ? String( "; " ) : String()) + reasons[i];
         }
         return v;
      }
      if ( Section( policy, "reviewedSafe" ).contains( id ) )
         return v;

      // Not in the policy (a newer PixInsight, a third-party module): the
      // coverage self-test only protects the machine it ran on, so the same
      // heuristic decides at runtime.
      const nlohmann::json hits = SideEffectParameterIds( *P );
      if ( !hits.empty() )
      {
         String ids;
         for ( size_t i = 0; i < hits.size(); ++i )
            ids += (i > 0 ? String( ", " ) : String()) + S16( hits[i].get<std::string>() );
         v.kind = SafetyVerdict::Confirm;
         v.reason = "it has not been reviewed and has file/output-like parameters: " + ids;
      }
   }
   catch ( ... )
   {
      // Fail CLOSED: a run that could not be checked is asked about.
      v.kind = SafetyVerdict::Confirm;
      v.reason = "this run could not be checked";
   }
   return v;
}

nlohmann::json UnclassifiedSideEffectCandidates()
{
   const nlohmann::json& policy = CompiledProcessSafety();
   nlohmann::json out = nlohmann::json::array();
   for ( const Process& P : Process::AllProcesses() )
   {
      const std::string id( P.Id().c_str() );
      if ( Classified( policy, id ) )
         continue;
      const nlohmann::json hits = SideEffectParameterIds( P );
      if ( !hits.empty() )
         out.push_back( { { "process", id }, { "parameters", hits },
                          { "canProcessViews", P.CanProcessViews() }, { "canProcessGlobal", P.CanProcessGlobal() } } );
   }
   return out;
}

nlohmann::json UnknownPolicyProcessIds()
{
   nlohmann::json out = nlohmann::json::array();
   const nlohmann::json& policy = CompiledProcessSafety();
   for ( const char* s : { "deny", "confirmAlways", "confirmWhen", "reviewedSafe", "fileTables" } )
   {
      const nlohmann::json& section = Section( policy, s );
      for ( auto it = section.begin(); it != section.end(); ++it )
      {
         bool installed = false;
         try
         {
            // Must be the canonical id: a policy keyed by an alias would never match P.Id().
            installed = Process( IsoString( it.key().c_str() ) ).Id() == IsoString( it.key().c_str() );
         }
         catch ( ... )
         {
         }
         if ( !installed )
            out.push_back( std::string( s ) + ":" + it.key() );
      }
   }
   // confirmWhen rules must name real parameters of their process.
   const nlohmann::json& when = Section( policy, "confirmWhen" );
   for ( auto it = when.begin(); it != when.end(); ++it )
      if ( it.value().is_array() )
         for ( const nlohmann::json& rule : it.value() )
         {
            const std::string param = rule.value( "parameter", std::string() );
            try
            {
               const Process P( IsoString( it.key().c_str() ) );
               const ProcessParameter p( P, IsoString( param.c_str() ) );
            }
            catch ( ... )
            {
               out.push_back( "confirmWhen:" + it.key() + "." + param );
            }
         }
   return out;
}

} // namespace pcl
