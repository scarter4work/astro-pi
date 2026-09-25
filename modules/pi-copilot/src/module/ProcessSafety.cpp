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

// A "version" value: empty, or 2-4 dot-separated groups of ASCII digits
// (e.g. 3.0.2). Hand-written on purpose: no regex engine (std::regex inside
// PixInsight aborts the process on a malformed pattern instead of throwing).
bool IsVersionText( const std::string& v )
{
   if ( v.empty() )
      return true;
   int groups = 0;
   size_t i = 0;
   while ( true )
   {
      const size_t start = i;
      while ( i < v.size() && v[i] >= '0' && v[i] <= '9' )
         ++i;
      if ( i == start )
         return false;
      ++groups;
      if ( i == v.size() )
         break;
      if ( v[i] != '.' )
         return false;
      ++i;
   }
   return groups >= 2 && groups <= 4;
}

// 1 = allowed, 0 = not allowed, -1 = malformed rule. A rule is
// {"values": [<string>...], "reason"} or {"format": "version", "reason"}.
int ValueAllowed( const nlohmann::json& rule, const std::string& value )
{
   if ( !rule.is_object() || rule.value( "reason", std::string() ).empty()
        || rule.contains( "values" ) == rule.contains( "format" ) )
      return -1;
   if ( rule.contains( "values" ) )
   {
      if ( !rule["values"].is_array() || rule["values"].empty() )
         return -1;
      for ( const nlohmann::json& v : rule["values"] )
      {
         if ( !v.is_string() )
            return -1;
         if ( v.get_ref<const std::string&>() == value )   // exact, length-aware
            return 1;
      }
      return 0;
   }
   if ( rule["format"] != "version" )
      return -1;
   return IsVersionText( value ) ? 1 : 0;
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
      StringList reasons;
      // globalConfirm: reviewed, and a GLOBAL run always asks (a run on a view
      // is judged by the other sections, below).
      const nlohmann::json& globalConfirm = Section( policy, "globalConfirm" );
      if ( run == SafetyRunKind::Global && globalConfirm.contains( id ) )
         reasons << "a global run: " + S16( globalConfirm[id].get<std::string>() );
      const nlohmann::json& when = Section( policy, "confirmWhen" );
      if ( when.contains( id ) && when[id].is_array() )
      {
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
      }
      else if ( !Section( policy, "reviewedSafe" ).contains( id ) )
      {
         // Not in the policy (a newer PixInsight, a third-party module): the
         // coverage self-tests only protect the machine they ran on, so the
         // same rules decide at runtime.
         const nlohmann::json hits = SideEffectParameterIds( *P );
         if ( !hits.empty() )
         {
            String ids;
            for ( size_t i = 0; i < hits.size(); ++i )
               ids += (i > 0 ? String( ", " ) : String()) + S16( hits[i].get<std::string>() );
            reasons << "it has not been reviewed and has file/output-like parameters: " + ids;
         }
      }
      // A global run can change application-wide state (e.g. the default RGB
      // working space) that no parameter name reveals, and reviewedSafe /
      // confirmWhen reviewed runs on views: EVERY global-capable process
      // outside deny/confirmAlways needs its own global review.
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

bool NoInstanceBeforeApproval( const IsoString& processId )
{
   std::unique_ptr<Process> P;
   try
   {
      P.reset( new Process( processId ) );
   }
   catch ( ... )
   {
      return false;   // unknown process id: nothing can be built; the executor names it
   }
   try
   {
      const std::string id( P->Id().c_str() );
      const nlohmann::json& policy = CompiledProcessSafety();
      return Section( policy, "deny" ).contains( id ) || Section( policy, "confirmAlways" ).contains( id );
   }
   catch ( ... )
   {
      return true;
   }
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
      if ( Section( policy, "deny" ).contains( id ) || Section( policy, "confirmAlways" ).contains( id )
           || globalSafe.contains( id ) || globalConfirm.contains( id ) )
         continue;
      nlohmann::json row = { { "process", id }, { "canProcessViews", P.CanProcessViews() } };
      for ( const char* s : { "confirmWhen", "reviewedSafe" } )
         if ( Section( policy, s ).contains( id ) )
            row["alsoIn"] = s;   // reviewed for runs on views, not yet for global runs
      out.push_back( row );
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
                           "fileTables", "pinnedParameters", "parameterValues" } )
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
   // globalSafe/globalConfirm review GLOBAL runs; they sit beside
   // reviewedSafe/confirmWhen (which review runs on views), but beside
   // deny/confirmAlways (which already decide every run) or each other they
   // are dead weight that hides which rule applies.
   for ( const char* g : { "globalSafe", "globalConfirm" } )
   {
      const nlohmann::json& section = Section( policy, g );
      for ( auto it = section.begin(); it != section.end(); ++it )
         if ( Section( policy, "deny" ).contains( it.key() ) || Section( policy, "confirmAlways" ).contains( it.key() )
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
   // parameterValues: writable scalar string parameters under their
   // canonical id, a well-formed rule, and a process default the rule
   // accepts (else every default run would be refused).
   const nlohmann::json& valueRules = Section( policy, "parameterValues" );
   for ( auto it = valueRules.begin(); it != valueRules.end(); ++it )
      if ( it.value().is_object() )
         for ( auto q = it.value().begin(); q != it.value().end(); ++q )
         {
            const std::string where = "parameterValues:" + it.key() + "." + q.key();
            try
            {
               const Process P( IsoString( it.key().c_str() ) );
               const ProcessParameter p( P, IsoString( q.key().c_str() ) );
               if ( !p.IsString() || p.IsReadOnly() || std::string( p.Id().c_str() ) != q.key() )
                  out.push_back( where + " (not a writable string parameter under its canonical id)" );
               else
               {
                  // The default INSTANCE's value (row 0): some modules cannot
                  // report a metadata default (GraXpert.correction:
                  // "GetParameterDefaultValue(): API function error").
                  const ProcessInstance d( P );
                  const int a = ValueAllowed( q.value(), U8( d.ParameterValue( p, 0 ).ToString() ) );
                  if ( a < 0 )
                     out.push_back( where + " (needs a reason and either values [strings] or format \"version\")" );
                  else if ( a == 0 )
                     out.push_back( where + " (the process default is not allowed by the rule)" );
               }
            }
            catch ( const pcl::Exception& x )
            {
               out.push_back( where + " (" + U8( x.Message() ) + ")" );
            }
            catch ( const std::exception& x )
            {
               out.push_back( where + " (" + x.what() + ")" );
            }
            catch ( ... )
            {
               out.push_back( where );
            }
         }
      else
         out.push_back( "parameterValues:" + it.key() + " (not an object)" );
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

namespace
{

// The policy's pinned entry for canonical process id `id`: "" and entry =
// nullptr when the process pins nothing; "" and the entry when it is an
// object; else an "internal: ..." refusal (fail CLOSED: a malformed entry
// never means "nothing is pinned").
String PinnedEntry( const std::string& id, const nlohmann::json*& entry )
{
   entry = nullptr;
   const nlohmann::json& pinned = Section( CompiledProcessSafety(), "pinnedParameters" );
   const auto it = pinned.find( id );
   if ( it == pinned.end() )
      return String();
   if ( !it->is_object() )
      return "internal: the safety policy's pinnedParameters entry for " + S16( id ) + " is not an object, so "
             + S16( id ) + " cannot be run safely";
   for ( auto q = it->begin(); q != it->end(); ++q )
      if ( !q.value().is_object() )
         return "internal: the safety policy's pinned parameter " + S16( id + "." + q.key() ) + " is not an object, so "
                + S16( id ) + " cannot be run safely";
   entry = &*it;
   return String();
}

// The model never chooses a pinned value: ANY key that resolves to one (its
// id or an alias -- the lookup cuts at an embedded NUL exactly as the
// executor's does), in either object, whatever the value, is refused.
String PinnedKeyProblem( const Process& P, const nlohmann::json& entry, const nlohmann::json& parameters,
                         const nlohmann::json& tableParameters )
{
   for ( const nlohmann::json* given : { &parameters, &tableParameters } )
      if ( given->is_object() )
         for ( auto it = given->begin(); it != given->end(); ++it )
         {
            std::string canonical;
            try
            {
               canonical = ProcessParameter( P, IsoString( it.key().c_str() ) ).Id().c_str();
            }
            catch ( ... )
            {
               continue;   // unknown key: SetParameters() reports it
            }
            if ( entry.contains( canonical ) )
               return String( P.Id() ) + "." + S16( canonical ) + " is set by PI Copilot from "
                      + S16( entry[canonical].value( "from", std::string( "a trusted setting" ) ) ) + "; omit it";
         }
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
      const nlohmann::json* entry = nullptr;
      {
         const String e = PinnedEntry( id, entry );
         if ( !e.IsEmpty() || entry == nullptr )
            return e;
      }
      {
         const String e = PinnedKeyProblem( *P, *entry, parameters, tableParameters );
         if ( !e.IsEmpty() )
            return e;
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
   catch ( ... )
   {
      out.clear();
      return "internal: pinned parameters could not be resolved";
   }
   return String();
}

String CheckResolvedPinnedParameters( const IsoString& processId, const nlohmann::json& parameters,
                                      const nlohmann::json& tableParameters, const std::vector<PinnedParameter>& resolved )
{
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
      const nlohmann::json* entry = nullptr;
      {
         const String e = PinnedEntry( id, entry );
         if ( !e.IsEmpty() )
            return e;
      }
      if ( entry != nullptr )
      {
         const String e = PinnedKeyProblem( *P, *entry, parameters, tableParameters );
         if ( !e.IsEmpty() )
            return e;
      }
      // Exactly the policy's pinned parameters, each once.
      std::set<std::string> want, got;
      if ( entry != nullptr )
         for ( auto q = entry->begin(); q != entry->end(); ++q )
            want.insert( q.key() );
      for ( const PinnedParameter& q : resolved )
         if ( !got.insert( q.parameter ).second )
            return "internal: pinned parameter " + S16( id + "." + q.parameter ) + " was resolved twice";
      if ( want != got )
         return "internal: the pinned parameters of " + S16( id ) + " were not resolved before this run";
   }
   catch ( const pcl::Exception& x )
   {
      return "internal: pinned parameters could not be checked: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return "internal: pinned parameters could not be checked: " + String( x.what() );
   }
   catch ( ... )
   {
      return "internal: pinned parameters could not be checked";
   }
   return String();
}

String ParameterValueProblem( const IsoString& processId, const IsoString& parameterId, const String& value )
{
   try
   {
      const nlohmann::json& rules = Section( CompiledProcessSafety(), "parameterValues" );
      const std::string pid( processId.c_str() ), param( parameterId.c_str() );
      const auto entry = rules.find( pid );
      if ( entry == rules.end() )
         return String();
      const String name = S16( pid + "." + param );
      if ( !entry->is_object() )
         return "internal: the safety policy's parameterValues entry for " + S16( pid ) + " is not an object";
      const auto rule = entry->find( param );
      if ( rule == entry->end() )
         return String();
      const int a = ValueAllowed( *rule, U8( value ) );
      if ( a < 0 )
         return "internal: the safety policy's value rule for " + name + " is malformed";
      if ( a > 0 )
         return String();
      return name + ": '" + value + "' is not allowed: use " + S16( rule->value( "reason", std::string() ) );
   }
   catch ( const std::exception& x )
   {
      return "internal: the safety policy's value rule for " + String( processId ) + "." + String( parameterId )
             + " could not be checked: " + String( x.what() );
   }
   catch ( ... )
   {
      return "internal: the safety policy's value rule for " + String( processId ) + "." + String( parameterId )
             + " could not be checked";
   }
}

void AnnotatePolicyParameters( nlohmann::json& description )
{
   if ( !description.is_object() || !description.contains( "id" ) || !description["id"].is_string()
        || !description.contains( "parameters" ) || !description["parameters"].is_array() )
      return;
   const std::string id = description["id"].get<std::string>();
   const nlohmann::json& policy = CompiledProcessSafety();
   const nlohmann::json& pinned = Section( policy, "pinnedParameters" );
   const nlohmann::json& patterns = Section( policy, "parameterValues" );
   const nlohmann::json empty = nlohmann::json::object();
   const nlohmann::json& pin = (pinned.contains( id ) && pinned[id].is_object()) ? pinned[id] : empty;
   const nlohmann::json& pat = (patterns.contains( id ) && patterns[id].is_object()) ? patterns[id] : empty;
   for ( nlohmann::json& p : description["parameters"] )
   {
      if ( !p.is_object() || !p.contains( "id" ) || !p["id"].is_string() )
         continue;
      const std::string pid = p["id"].get<std::string>();
      if ( pin.contains( pid ) && pin[pid].is_object() )
         p["setBy"] = "PI Copilot, from " + pin[pid].value( "from", std::string( "a trusted setting" ) )
                      + ": do not pass it";
      if ( pat.contains( pid ) && pat[pid].is_object() )
      {
         const nlohmann::json& rule = pat[pid];
         if ( rule.contains( "values" ) )
            p["allowedValues"] = rule["values"];
         else
            p["format"] = rule.value( "format", std::string() );
         p["allowedNote"] = "use " + rule.value( "reason", std::string() );
      }
   }
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
