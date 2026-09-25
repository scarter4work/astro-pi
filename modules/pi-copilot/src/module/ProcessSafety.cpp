// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessSafety.h"
#include "GlobalRunFiles.h"
#include "ProcessCatalog.h"
#include "ProcessSafetyData.h"
#include "Utf8.h"

#include <pcl/Exception.h>
#include <pcl/Settings.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/StringList.h>
#include <pcl/Variant.h>

#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <regex>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace pcl
{

namespace
{

const nlohmann::json* g_override = nullptr;

GlobalSettingReader g_settingReader;   // empty: Settings::ReadGlobal

const char* const kSections[] = { "deny", "confirmAlways", "confirmWhen", "reviewedSafe" };

// Length-aware (Utf8.h): never a silent cut at an embedded NUL.
String S16( const std::string& s )
{
   return FromU8( s );
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
      // std::string compare: a value with an embedded NUL matches nothing.
      const std::string& want = v.get_ref<const std::string&>();
      for ( const ProcessParameter::EnumerationElement& e : elements )
      {
         if ( want == e.id.c_str() )
            return std::string( e.id.c_str() );
         for ( const IsoString& a : e.aliases )
            if ( want == a.Trimmed().c_str() )
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
                                  const nlohmann::json& /*tableParameters*/, SafetyRunKind run )
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
      // globalConfirm: reviewed, and a GLOBAL run always asks (a run on a view
      // is judged like any unlisted process, below).
      const nlohmann::json& globalConfirm = Section( policy, "globalConfirm" );
      if ( run == SafetyRunKind::Global && globalConfirm.contains( id ) )
      {
         v.kind = SafetyVerdict::Confirm;
         v.reason = "a global run: " + S16( globalConfirm[id].get<std::string>() );
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
      // coverage self-tests only protect the machine they ran on, so the same
      // rules decide at runtime.
      StringList reasons;
      const nlohmann::json hits = SideEffectParameterIds( *P );
      if ( !hits.empty() )
      {
         String ids;
         for ( size_t i = 0; i < hits.size(); ++i )
            ids += (i > 0 ? String( ", " ) : String()) + S16( hits[i].get<std::string>() );
         reasons << "it has not been reviewed and has file/output-like parameters: " + ids;
      }
      // A global run can change application-wide state (e.g. the default RGB
      // working space) that no parameter name reveals.
      if ( run == SafetyRunKind::Global && P->CanProcessGlobal() && !Section( policy, "globalSafe" ).contains( id )
           && !globalConfirm.contains( id ) )
         reasons << String( "it has not been reviewed for global runs, which can change PixInsight-wide settings "
                            "rather than create new images" );
      if ( !reasons.IsEmpty() )
      {
         v.kind = SafetyVerdict::Confirm;
         for ( size_type i = 0; i < reasons.Length(); ++i )
            v.reason += (i > 0 ? String( "; " ) : String()) + reasons[i];
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

nlohmann::json UnreviewedGlobalProcesses()
{
   const nlohmann::json& policy = CompiledProcessSafety();
   const nlohmann::json& globalSafe = Section( policy, "globalSafe" );
   const nlohmann::json& globalConfirm = Section( policy, "globalConfirm" );
   nlohmann::json out = nlohmann::json::array();
   for ( const Process& P : Process::AllProcesses() )
   {
      if ( !P.CanProcessGlobal() )
         continue;
      const std::string id( P.Id().c_str() );
      if ( Classified( policy, id ) || globalSafe.contains( id ) || globalConfirm.contains( id ) )
         continue;
      out.push_back( { { "process", id }, { "canProcessViews", P.CanProcessViews() } } );
   }
   return out;
}

String ValidateProcessFilePaths( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters )
{
   return ValidateGlobalRunFilePaths( processId, Section( CompiledProcessSafety(), "fileTables" ),
                                      parameters, tableParameters );
}

bool HasFileTables( const IsoString& processId )
{
   return DeclaresFileTables( processId, Section( CompiledProcessSafety(), "fileTables" ) );
}

nlohmann::json UnknownPolicyProcessIds()
{
   nlohmann::json out = nlohmann::json::array();
   const nlohmann::json& policy = CompiledProcessSafety();
   for ( const char* s : { "deny", "confirmAlways", "confirmWhen", "reviewedSafe", "globalSafe", "globalConfirm",
                           "fileTables", "pinnedParameters" } )
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
   // globalSafe is for processes in no other section (else it is dead weight
   // that hides which rule applies).
   for ( const char* g : { "globalSafe", "globalConfirm" } )
   {
      const nlohmann::json& section = Section( policy, g );
      for ( auto it = section.begin(); it != section.end(); ++it )
         if ( Classified( policy, it.key() )
              || Section( policy, std::string( g ) == "globalSafe" ? "globalConfirm" : "globalSafe" ).contains( it.key() ) )
            out.push_back( std::string( g ) + ":" + it.key() + " (also in another section)" );
   }
   // fileTables: canonical table ids that are tables, with real columns.
   const nlohmann::json& tables = Section( policy, "fileTables" );
   for ( auto it = tables.begin(); it != tables.end(); ++it )
      if ( it.value().is_object() )
         for ( auto t = it.value().begin(); t != it.value().end(); ++t )
         {
            const std::string where = "fileTables:" + it.key() + "." + t.key();
            try
            {
               const Process P( IsoString( it.key().c_str() ) );
               const ProcessParameter tp( P, IsoString( t.key().c_str() ) );
               if ( !tp.IsTable() || std::string( tp.Id().c_str() ) != t.key() )
               {
                  out.push_back( where + " (not a table under its canonical id)" );
                  continue;
               }
               std::set<std::string> cols;
               for ( const ProcessParameter& c : tp.TableColumns() )
                  cols.insert( std::string( c.Id().c_str() ) );
               const std::string enabled = t.value().value( "enabledColumn", std::string() );
               if ( !enabled.empty() && !cols.count( enabled ) )
                  out.push_back( where + ".enabledColumn " + enabled );
               if ( t.value().contains( "columns" ) && t.value()["columns"].is_object() )
                  for ( auto c = t.value()["columns"].begin(); c != t.value()["columns"].end(); ++c )
                     if ( !cols.count( c.key() ) || !(c.value() == "image" || c.value() == "optionalFile") )
                        out.push_back( where + ".columns." + c.key() );
            }
            catch ( ... )
            {
               out.push_back( where );
            }
         }
   // pinnedParameters: real string parameters under their canonical id, with
   // a source and a kind this build implements.
   const nlohmann::json& pinned = Section( policy, "pinnedParameters" );
   for ( auto it = pinned.begin(); it != pinned.end(); ++it )
      if ( it.value().is_object() )
         for ( auto q = it.value().begin(); q != it.value().end(); ++q )
         {
            const std::string where = "pinnedParameters:" + it.key() + "." + q.key();
            try
            {
               const Process P( IsoString( it.key().c_str() ) );
               const ProcessParameter p( P, IsoString( q.key().c_str() ) );
               const nlohmann::json& rule = q.value();
               if ( !p.IsString() || std::string( p.Id().c_str() ) != q.key() )
                  out.push_back( where + " (not a string parameter under its canonical id)" );
               else if ( !rule.is_object() || rule.value( "source", std::string() ) != "globalSetting"
                         || rule.value( "key", std::string() ).rfind( "/", 0 ) != 0
                         || rule.value( "kind", std::string() ) != "executable"
                         || rule.value( "from", std::string() ).empty() || rule.value( "setupHint", std::string() ).empty() )
                  out.push_back( where + " (needs source \"globalSetting\", key \"/Module/...\", kind \"executable\", "
                                         "from, setupHint)" );
            }
            catch ( ... )
            {
               out.push_back( where );
            }
         }
      else
         out.push_back( "pinnedParameters:" + it.key() + " (not an object)" );
   return out;
}

void SetGlobalSettingReaderForSelfTest( GlobalSettingReader reader )
{
   g_settingReader = std::move( reader );
}

bool ReadGlobalSetting( const IsoString& key, String& value )
{
   if ( g_settingReader )
      return g_settingReader( key, value );
   try
   {
      return Settings::ReadGlobal( key, value );
   }
   catch ( ... )
   {
      return false;
   }
}

namespace
{

// "" when `path` is an absolute path to an existing, regular, executable file
// (symbolic links followed); then `canonical` is its realpath(). Else why not.
String ExecutableProblem( const String& path, String& canonical )
{
   if ( !path.StartsWith( '/' ) )
      return "is not an absolute path";
   const IsoString p = path.ToUTF8();
   char buf[PATH_MAX];
   if ( ::realpath( p.c_str(), buf ) == nullptr )
      return "does not exist (or cannot be resolved)";
   struct stat st;
   if ( ::stat( buf, &st ) != 0 )
      return "cannot be examined";
   if ( !S_ISREG( st.st_mode ) )
      return "is not a file";
   if ( ::access( buf, X_OK ) != 0 )
      return "is not executable";
   canonical = FromU8( std::string( buf ) );
   return String();
}

} // namespace

String ResolvePinnedParameters( const IsoString& processId, const nlohmann::json& parameters,
                                const nlohmann::json& tableParameters, std::vector<PinnedParameter>& out )
{
   out.clear();
   std::unique_ptr<Process> P;
   try
   {
      P.reset( new Process( processId ) );
   }
   catch ( ... )
   {
      return String();   // unknown process: the executor reports it precisely
   }
   try
   {
      const std::string id( P->Id().c_str() );
      const String pname( P->Id() );
      const nlohmann::json& pinned = Section( CompiledProcessSafety(), "pinnedParameters" );
      const auto entry = pinned.find( id );
      if ( entry == pinned.end() || !entry->is_object() )
         return String();

      // The model never chooses a pinned value: ANY key that resolves to it
      // (its id or an alias, in either object), whatever the value, is refused.
      for ( const nlohmann::json* given : { &parameters, &tableParameters } )
         if ( given->is_object() )
            for ( auto it = given->begin(); it != given->end(); ++it )
            {
               std::string canonical;
               try
               {
                  canonical = ProcessParameter( *P, IsoString( it.key().c_str() ) ).Id().c_str();
               }
               catch ( ... )
               {
                  continue;   // unknown key: SetParameters() reports it
               }
               if ( entry->contains( canonical ) )
                  return pname + "." + S16( canonical ) + " is set by PI Copilot from "
                         + S16( (*entry)[canonical].value( "from", std::string( "a trusted setting" ) ) ) + "; omit it";
            }

      for ( auto q = entry->begin(); q != entry->end(); ++q )
      {
         const nlohmann::json& rule = q.value();
         const String name = pname + "." + S16( q.key() );
         const String from = S16( rule.value( "from", std::string() ) );
         const String hint = " Ask the user to " + S16( rule.value( "setupHint", std::string() ) ) + ", then try again.";
         if ( rule.value( "source", std::string() ) != "globalSetting" || rule.value( "kind", std::string() ) != "executable" )
            return "internal: the safety policy's pinned parameter " + name + " has an unknown source or kind";
         const IsoString key( rule.value( "key", std::string() ).c_str() );
         String value;
         if ( !ReadGlobalSetting( key, value ) || value.Trimmed().IsEmpty() )
            return name + " cannot be set: there is no value for it in " + from + "." + hint;
         String canonical;
         const String problem = ExecutableProblem( value, canonical );
         if ( !problem.IsEmpty() )
            return name + " cannot be set: '" + value + "' (from " + from + ") " + problem + "." + hint;
         out.push_back( PinnedParameter{ q.key(), canonical, from } );
      }
   }
   catch ( const pcl::Exception& x )
   {
      out.clear();
      return "internal: pinned parameters could not be resolved: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      out.clear();
      return "internal: pinned parameters could not be resolved: " + String( x.what() );
   }
   return String();
}

String DescribePinnedParameters( const std::vector<PinnedParameter>& pinned )
{
   String s;
   for ( const PinnedParameter& p : pinned )
      s += (s.IsEmpty() ? String() : String( "\n" )) + S16( p.parameter ) + " = " + p.value
         + " (set by PI Copilot from " + p.from + ")";
   return s;
}

} // namespace pcl
