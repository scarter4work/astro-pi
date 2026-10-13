// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotScriptSelfTest.h"
#include "AgentTools.h"
#include "AnthropicClient.h"
#include "ChatThread.h"
#include "CopilotSettings.h"
#include "DataConsent.h"
#include "HistoryReader.h"
#include "JourneyExport.h"   // ManualWhy
#include "JourneyStore.h"   // RemoveDirectoryTree
#include "ToolHelpers.h"    // TextBlock
#include "JourneyTools.h"    // JourneyToolHost
#include "JourneyTracker.h"
#include "ModuleScript.h"
#include "PICopilotModule.h"   // ThePICopilotModule
#include "ProcessApply.h"   // OpenMainViewIds
#include "ScriptCatalog.h"
#include "ScriptSource.h"
#include "ScriptTools.h"
#include "SelfTestTiming.h"
#include "SystemPrompt.h"
#include "TextSafety.h"   // CodePointAt
#include "Utf8.h"
#include "ViewContext.h"

#include <pcl/AutoViewLock.h>
#include <pcl/ByteArray.h>
#include <pcl/Console.h>
#include <pcl/Exception.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/GlobalSettings.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Settings.h>
#include <pcl/Thread.h>

#include <chrono>
#include <memory>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace pcl
{

namespace
{

std::string ScratchDir()
{
   const char* s = std::getenv( "PICOPILOT_SELFTEST_SCRATCH" );
   if ( s == nullptr || *s == '\0' )
      throw Error( "PICOPILOT_SELFTEST_SCRATCH not set" );
   return s;
}

std::string RealPath8( const std::string& p )
{
   char* r = ::realpath( p.c_str(), nullptr );
   if ( r == nullptr )
      return std::string();
   std::string s( r );
   ::free( r );
   return s;
}

void MakeDir( const std::string& p )
{
   ::mkdir( p.c_str(), 0700 );
}

void WriteText( const std::string& path, const std::string& text )
{
   std::ofstream f( path, std::ios::binary | std::ios::trunc );
   f << text;
   if ( !f )
      throw Error( FromU8( "cannot write " + path ) );
}

std::string ReadText( const std::string& path )
{
   std::ifstream f( path, std::ios::binary );
   std::stringstream ss;
   ss << f.rdbuf();
   return ss.str();
}

void CopyFile8( const std::string& to, const std::string& from )
{
   WriteText( to, ReadText( from ) );
}

std::string Text0( const ToolOutcome& o )
{
   return o.content.empty() ? std::string() : o.content[0].value( "text", std::string() );
}

nlohmann::json Json0( const ToolOutcome& o )
{
   try
   {
      const nlohmann::json j = nlohmann::json::parse( Text0( o ) );
      if ( j.is_object() )
         return j;
   }
   catch ( ... )
   {
   }
   return nlohmann::json::object( { { "notJson", Text0( o ) } } );
}

bool Has( const std::string& s, const std::string& what )
{
   return s.find( what ) != std::string::npos;
}

// A visible float RGB window with one constant per channel; force-closed on destruction.
class RgbWindow
{
public:

   RgbWindow( const char* id, double r, double g, double b, int w = 64, int h = 64 )
      : m_window( w, h, 3, 32, true/*float*/, true/*color*/, false, IsoString( id ) )
   {
      if ( m_window.IsNull() )
         throw Error( "RgbWindow: null window" );
      View v = m_window.MainView();
      {
         AutoViewLock lock( v );
         ImageVariant iv = v.Image();
         Image& img = static_cast<Image&>( *iv );
         img.Fill( float( r ), Rect( 0 ), 0, 0 );
         img.Fill( float( g ), Rect( 0 ), 1, 1 );
         img.Fill( float( b ), Rect( 0 ), 2, 2 );
      }
      m_window.Show();
   }

   ~RgbWindow()
   {
      try { if ( !m_window.IsNull() ) m_window.ForceClose(); } catch ( ... ) {}
   }

   RgbWindow( const RgbWindow& ) = delete;
   RgbWindow& operator =( const RgbWindow& ) = delete;

   View MainView() const { return m_window.MainView(); }
   ImageWindow& Window() { return m_window; }

   // Deterministic pseudo-random noise around `level` (a script that measures noise needs some).
   void AddNoise( double level, double sigma )
   {
      View v = m_window.MainView();
      AutoViewLock lock( v );
      ImageVariant iv = v.Image();
      Image& img = static_cast<Image&>( *iv );
      uint32 x = 12345;
      for ( int c = 0; c < img.NumberOfChannels(); ++c )
         for ( Image::sample_iterator i( img, c ); i; ++i )
         {
            x = x*1664525u + 1013904223u;
            *i = float( level + sigma*((x >> 8)/double( 1 << 24 ) - 0.5) );
         }
   }

   double Mean( int c ) const
   {
      View v = m_window.MainView();
      AutoViewLock lock( v );
      ImageVariant iv = v.Image();
      return iv.Mean( Rect( 0 ), c, c );
   }

private:

   ImageWindow m_window;
};

void CloseNewWindows( const std::set<std::string>& before )
{
   for ( const std::string& id : OpenMainViewIds() )
      if ( before.count( id ) == 0 )
         try
         {
            ImageWindow w = ImageWindow::WindowById( IsoString( id.c_str() ) );
            if ( !w.IsNull() )
               w.ForceClose();
         }
         catch ( ... )
         {
         }
}

ToolContext BaseContext( const IsoString& viewId, int& asks, bool answer, String* html = nullptr )
{
   ToolContext ctx;
   ctx.mode = AgentMode::Copilot;
   ctx.turnViewId = viewId;
   ctx.confirmRunScript = [&asks, answer, html]( const String& h ) { ++asks; if ( html != nullptr ) *html = h; return answer; };
   return ctx;
}

} // namespace

bool RunScriptSelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- S1: run_pjsr is gone ------------------------------------------------
   SelfTestSectionMark( "S1 run_pjsr removed" );
   {
      nlohmann::json d;
      bool ok = false;
      try
      {
         bool defsOk = true, promptsOk = true;
         for ( AgentMode m : { AgentMode::Copilot, AgentMode::Guided, AgentMode::Advisor } )
         {
            std::vector<std::string> names;
            for ( const nlohmann::json& t : ToolDefinitions( m ) )
               names.push_back( t.at( "name" ).get<std::string>() );
            auto has = [&]( const char* n ) { return std::find( names.begin(), names.end(), n ) != names.end(); };
            defsOk = defsOk && !has( "run_pjsr" ) && has( "list_scripts" ) && has( "describe_script" )
                   && has( "run_script" ) == (m != AgentMode::Advisor);
            const String p = BuildSystemPrompt( m );
            promptsOk = promptsOk && !p.Contains( "run_pjsr" ) && !p.Contains( "PJSR script" )
                      && p.Contains( "list_scripts" ) && p.Contains( "run_script {" ) == (m != AgentMode::Advisor);
            d[m == AgentMode::Copilot ? "copilotTools" : m == AgentMode::Guided ? "guidedTools" : "advisorTools"] = names;
         }
         int asks = 0;
         ToolContext ctx = BaseContext( IsoString(), asks, true );
         const ToolOutcome o = ExecuteTool( ToolCall{ "x1", "run_pjsr", { { "purpose", "p" }, { "code", "return 1;" } } }, ctx );
         const bool unknownOk = o.isError && Has( Text0( o ), "unknown tool" ) && asks == 0;
         d["runPjsrCall"] = Text0( o ).substr( 0, 300 );

         // The retired setting is removed through the Settings API.
         Settings::Write( "PICopilot/RunPjsrEnabled", true );
         const StringList removed = CopilotSettings::RemoveRetiredSettings();
         bool still = false;
         const bool stillThere = Settings::Read( "PICopilot/RunPjsrEnabled", still );
         const StringList again = CopilotSettings::RemoveRetiredSettings();
         const bool settingOk = removed.Length() == 1 && removed[0] == "PICopilot/RunPjsrEnabled" && !stillThere && again.IsEmpty();
         d["defsOk"] = defsOk; d["promptsOk"] = promptsOk; d["unknownOk"] = unknownOk; d["retiredSettingOk"] = settingOk;
         ok = defsOk && promptsOk && unknownOk && settingOk;
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      out["runPjsrRemovedDetail"] = d;
      out["runPjsrRemovedOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S2: data enters the module's own scripts only as literals ------------
   SelfTestSectionMark( "S2 data literals" );
   {
      nlohmann::json cases = nlohmann::json::array();
      bool ok = true;
      try
      {
         auto u16 = []( std::initializer_list<unsigned> units ) { String s; for ( unsigned u : units ) s += char16_type( u ); return s; };
         auto codes = []( const String& s )
         {
            nlohmann::json a = nlohmann::json::array();
            for ( size_type i = 0; i < s.Length(); ++i )
            {
               unsigned u = s[i];
               if ( u >= 0xD800 && u <= 0xDFFF )
               {
                  const bool hi = u <= 0xDBFF && i+1 < s.Length() && s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF;
                  const bool lo = u >= 0xDC00 && i > 0 && s[i-1] >= 0xD800 && s[i-1] <= 0xDBFF;
                  if ( !hi && !lo )
                     u = 0xFFFD;
               }
               a.push_back( u );
            }
            return a;
         };
         const String sentinel = "new ImageWindow( 8, 8, 1, 32, true, false, \"PCLitBreak\" );";
         const std::vector<std::pair<const char*, String>> list = {   // pcl-move-ok: const, read only
            { "quote",         "a\" + (" + sentinel + ") + \"" },
            { "backslash",     "\\\\\"; " + sentinel + " //" },
            { "commentClose",  "*/ " + sentinel + " /*" },
            { "scriptTag",     "</script><script>" + sentinel + "</script>" },
            { "newlines",      "a\r\n" + sentinel + "\n\"\n" },
            { "nulControl",    "a" + u16( { 0 } ) + "b" + u16( { 1, 0x1f, 0x7f } ) + sentinel },
            { "lineSep2028",   "x" + u16( { 0x2028 } ) + sentinel },
            { "paraSep2029",   "x" + u16( { 0x2029 } ) + sentinel },
            { "surrogates",    u16( { 0xD83D, 0xDE00 } ) + sentinel },
            { "loneSurrogate", u16( { 0xD83D } ) + sentinel },
            { "fnClose",       "\" }); " + sentinel + " (function(){ \"" },
            { "unicodeEscape", "\\u0022); " + sentinel + " //" },
            { "hostilePath",   "/opt/PixInsight/src/scripts/x\"); " + sentinel + " (\".js" },
         };
         for ( const auto& c : list )
         {
            const std::string lit = ScriptLiteral( c.second );
            bool ascii = true;
            for ( unsigned char ch : lit )
               ascii = ascii && ch >= 0x20 && ch < 0x7f;
            const nlohmann::json back = EvaluateAsciiJson(
               "var s = " + lit + ", a = []; for ( var i = 0; i < s.length; ++i ) a.push( s.charCodeAt( i ) );"
               " return pcAscii( JSON.stringify( a ) );" );
            const bool faithful = back == codes( c.second );
            const bool ran = !ImageWindow::WindowById( IsoString( "PCLitBreak" ) ).IsNull();
            if ( ran )
               ImageWindow::WindowById( IsoString( "PCLitBreak" ) ).ForceClose();
            const bool caseOk = ascii && faithful && !ran;
            cases.push_back( { { "name", c.first }, { "asciiOnly", ascii }, { "faithful", faithful }, { "sentinelRan", ran }, { "ok", caseOk } } );
            ok = ok && caseOk;
         }
      }
      catch ( const pcl::Exception& x ) { cases.push_back( { { "error", U8( x.Message() ) } } ); ok = false; }
      catch ( const std::exception& x ) { cases.push_back( { { "error", x.what() } } ); ok = false; }
      out["dataLiteralCases"] = cases;
      out["dataLiteralOk"] = ok;
      allOk = allOk && ok;
   }

   String productionRoot, rootError;
   productionRoot = ProductionScriptsRoot( rootError );

   // ---- S3: the static script reader ------------------------------------------
   SelfTestSectionMark( "S3 static script reader" );
   {
      nlohmann::json d;
      bool ok = false;
      try
      {
         const std::string base = ScratchDir() + "/script-src";
         MakeDir( base );
         MakeDir( base + "/T" );
         WriteText( base + "/T/main.js",
            "#feature-id PCFix : PICopilotTest > Fixture\n"
            "#feature-info Line one <b>bold</b>\\\n"
            "   line two\n"
            "#define TITLE \"Fixture\"\n"
            "#define VERSION \"1.2.3\"\n"
            "#define KEY_GAIN \"gain\"\n"
            "#define DEF_N 7\n"
            "#include \"lib.jsh\"\n"
            "#include \"../../outside.jsh\"\n"
            "// Parameters.getReal( \"commented\" )\n"
            "/* Parameters.getBoolean( \"blockcommented\" ) */\n"
            "var s = \"Parameters.getInteger( 'instring' )\";\n"
            "var re = /\"Parameters.getReal('inregex')\"/;\n"
            "function P() { this.gain = 2.5; this.mode = \"fast\"; }\n"
            "P.prototype.load = function() {\n"
            "   if ( Parameters.has( KEY_GAIN ) ) this.gain = Parameters.getReal( KEY_GAIN );\n"
            "   if ( Parameters.has( \"mode\" ) ) this.mode = Parameters.getString( \"mode\" );\n"
            "   this.n = Parameters.has( \"n\" ) ? Parameters.getUInt( \"n\" ) : DEF_N;\n"
            "   for ( var k in this ) if ( Parameters.has( \"x_\" + k ) ) this[k] = Parameters.get( \"x_\" + k );\n"
            "};\n"
            "if ( Parameters.isViewTarget ) { (new P).load(); } else { (new Dialog).execute(); }\n" );
         WriteText( base + "/T/lib.jsh", "function libLoad( o ) { o.flag = Parameters.getBoolean( \"flag\" ); }\n" );
         WriteText( ScratchDir() + "/outside.jsh", "var leak = Parameters.getReal( \"leak\" );\n" );   // outside the tree: never read
         const std::string root = RealPath8( base );
         const ScriptDescription fx = DescribeScriptSource( FromU8( root + "/T/main.js" ), FromU8( root ) );
         const nlohmann::json fj = ScriptDescriptionJson( fx );
         d["fixture"] = fj;
         auto param = [&]( const ScriptDescription& sd, const char* n ) -> const ScriptParameter*
         {
            for ( const ScriptParameter& p : sd.parameters )
               if ( p.name == n )
                  return &p;
            return nullptr;
         };
         const ScriptParameter* gain = param( fx, "gain" );
         const ScriptParameter* mode = param( fx, "mode" );
         const ScriptParameter* n = param( fx, "n" );
         const ScriptParameter* flag = param( fx, "flag" );
         const bool fixtureOk = fx.ok && fx.parameters.size() == 4
            && gain && gain->type == "real" && gain->hasDefault && gain->defaultValue == 2.5 && gain->viaDefine == "KEY_GAIN"
            && mode && mode->type == "string" && mode->hasDefault && mode->defaultValue == "fast"
            && n && n->type == "unsigned" && n->hasDefault && n->defaultValue == 7
            && flag && flag->type == "boolean" && !flag->hasDefault
            && fx.unresolved.size() == 2
            && !param( fx, "commented" ) && !param( fx, "blockcommented" ) && !param( fx, "instring" ) && !param( fx, "inregex" )
            && !param( fx, "leak" )
            && fx.title == "Fixture" && fx.version == "1.2.3" && fx.featureId == "PCFix" && fx.menu == "PICopilotTest > Fixture"
            && fx.info == "Line one bold line two" && fx.files.size() == 2
            && fx.SupportsView() && !fx.SupportsGlobal() && fx.executeCallsAt.size() == 1
            && ScriptInteraction( fx ) == "honours-parameters-when-targeted"
            && fj["includesNotFollowed"].size() == 1 && Has( fj["includesNotFollowed"][0].value( "reason", std::string() ), "outside" );
         // Argument validation against the description.
         StringKeyValueList kv;
         nlohmann::json shown;
         const String good = ValidateScriptArguments( fx, { { "gain", 1.5 }, { "mode", "slow" }, { "n", 3 }, { "flag", true } }, kv, shown );
         bool kvOk = good.IsEmpty() && kv.Length() == 4;
         for ( const StringKeyValue& e : kv )
            kvOk = kvOk && ((e.key == "gain" && e.value == "1.5") || (e.key == "mode" && e.value == "slow")
                         || (e.key == "n" && e.value == "3") || (e.key == "flag" && e.value == "true"));
         const String unknown = ValidateScriptArguments( fx, { { "bogus", 1 } }, kv, shown );
         const String neg = ValidateScriptArguments( fx, { { "n", -1 } }, kv, shown );
         const String typ = ValidateScriptArguments( fx, { { "gain", "x" } }, kv, shown );
         const String bidi = ValidateScriptArguments( fx, { { "mode", U8( String( "a" ) + char16_type( 0x202E ) + "b" ) } }, kv, shown );
         const bool validateOk = kvOk && Has( U8( unknown ), "Valid parameters: gain (real), mode (string), n (unsigned), flag (boolean)" )
                              && Has( U8( unknown ), "names are built at run time" )
                              && Has( U8( neg ), "non-negative" ) && Has( U8( typ ), "must be a number" ) && Has( U8( bidi ), "U+202E" );
         d["validate"] = { { "good", U8( good ) }, { "unknown", U8( unknown ) }, { "negative", U8( neg ) }, { "type", U8( typ ) }, { "bidi", U8( bidi ) } };

         // A real PixInsight script, checked by hand against its source:
         // CorrectMagentaStars.js 2.0, lines 178-184 and 215-234.
         bool cmsOk = false, neOk = false;
         if ( !productionRoot.IsEmpty() )
         {
            const ScriptDescription cms = DescribeScriptSource( productionRoot + "/CorrectMagentaStars/CorrectMagentaStars.js",
                                                                productionRoot, PixInsightIncludeDirectory() );
            d["correctMagentaStars"] = ScriptDescriptionJson( cms );
            struct Want { const char* name; const char* type; nlohmann::json def; };
            const Want want[] = { { "CMSAmount", "real", 0.8 }, { "protectionMethod", "integer", 0 }, { "useAutoMask", "boolean", true },
                                  { "showAutoMask", "boolean", true }, { "autodetectLinearity", "boolean", true },
                                  { "linearImage", "boolean", true }, { "CMSPresLight", "boolean", true } };
            cmsOk = cms.ok && cms.parameters.size() == 7 && cms.unresolved.empty() && cms.SupportsView() && cms.SupportsGlobal()
                 && cms.version == "2.0" && ScriptInteraction( cms ) == "honours-parameters-when-targeted";
            for ( const Want& w : want )
            {
               const ScriptParameter* p = param( cms, w.name );
               cmsOk = cmsOk && p != nullptr && p->type == w.type && p->hasDefault && p->defaultValue == w.def;
            }
            const ScriptDescription ne = DescribeScriptSource( productionRoot + "/NoiseEvaluation/NoiseEvaluation.js",
                                                               productionRoot, PixInsightIncludeDirectory() );
            neOk = ne.ok && ne.parameters.empty() && ScriptInteraction( ne ) == "no-dialog";
         }
         d["fixtureOk"] = fixtureOk; d["validateOk"] = validateOk; d["cmsOk"] = cmsOk; d["noiseEvaluationOk"] = neOk;
         ok = fixtureOk && validateOk && cmsOk && neOk;
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      out["scriptSourceDetail"] = d;
      out["scriptSourceOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S4: run_script eligibility: the four proofs ----------------------------
   SelfTestSectionMark( "S4 run_script eligibility proofs" );
   {
      nlohmann::json d;
      bool ok = false, runOk = false;
      const std::set<std::string> windowsBefore = OpenMainViewIds();
      try
      {
         if ( productionRoot.IsEmpty() )
            throw Error( "no PixInsight script directory: " + rootError );
         d["productionRoot"] = U8( productionRoot );
         const bool rootOk = U8( productionRoot ) == RealPath8( U8( PixInsightSettings::GlobalString( "Application/SrcDirectory" ) ) + "/scripts" );

         const ScriptCatalog cat = BuildScriptCatalog( productionRoot, false );
         const ScriptCatalog cached = BuildScriptCatalog( productionRoot, true );
         d["catalog"] = { { "ok", cat.ok }, { "eligible", cat.eligible.size() }, { "refused", cat.refused.size() },
                          { "msFresh", cat.ms }, { "msCached", cached.ms } };
         const ScriptEntry* ne = FindScript( cat, "NoiseEvaluation" );
         const ScriptEntry* cms = FindScript( cat, "CorrectMagentaStars" );
         nlohmann::json refusedSample = nlohmann::json::array();
         bool rcRefused = false, devRefused = false;
         for ( const ScriptRefusal& r : cat.refused )
         {
            if ( r.rel == "RCAstro/RCAstroBXT.js" )
               rcRefused = Has( r.reason, "not signed" );
            if ( r.rel == "CodeSign/CodeSign.js" )
               devRefused = Has( r.reason, "Development" );
            if ( refusedSample.size() < 12 )
               refusedSample.push_back( { { "file", r.rel }, { "reason", r.reason } } );
         }
         d["refusedSample"] = refusedSample;
         std::set<std::string> ids;
         bool idsUnique = true;
         nlohmann::json shared = nlohmann::json::array();
         for ( const ScriptEntry& e : cat.eligible )
         {
            idsUnique = idsUnique && ids.insert( e.id ).second;
            if ( e.id != e.scriptId )
               shared.push_back( { { "id", e.id }, { "scriptId", e.scriptId }, { "file", e.rel } } );
         }
         d["sharedSignatureIds"] = shared;
         const ScriptEntry* cfa = FindScript( cat, "NoiseEvaluationCFA" );
         const bool catalogOk = rootOk && cat.ok && idsUnique && ne != nullptr && ne->developerId == "PTeam"
                             && ne->rel == "NoiseEvaluation/NoiseEvaluation.js"
                             && cfa != nullptr && cfa->scriptId == "NoiseEvaluation" && cms != nullptr
                             && cms->developerId == "PTeam" && devRefused && cached.eligible.size() == cat.eligible.size();

         // (i) an eligible signed PixInsight-distribution script runs (through the core).
         RgbWindow t1( "PCScriptT1", 0.2, 0.3, 0.4 );
         t1.AddNoise( 0.2, 0.05 );
         int asks = 0;
         String html;
         ToolContext ctx = BaseContext( t1.MainView().FullId(), asks, true, &html );
         const ToolOutcome r1 = ExecuteTool( ToolCall{ "s1", "run_script", { { "script_id", "NoiseEvaluation" }, { "reason", "measure noise" } } }, ctx );
         const nlohmann::json j1 = Json0( r1 );
         d["proofEligibleRuns"] = { { "isError", r1.isError }, { "result", j1 }, { "asks", asks }, { "log", U8( r1.logLine ) },
                                    { "confirmHtml", U8( html ).substr( 0, 1500 ) } };
         const std::string con = j1.value( "console", std::string() );
         // A script that throws is reported as an error (Console::ExecuteScript* itself only reports load
         // failures): NoiseEvaluation on a constant image stops with "Zero or insignificant data".
         RgbWindow flat( "PCScriptFlat", 0.2, 0.2, 0.2 );
         ToolContext fctx0 = BaseContext( flat.MainView().FullId(), asks, true );
         const ToolOutcome rf = ExecuteTool( ToolCall{ "s1f", "run_script", { { "script_id", "NoiseEvaluation" } } }, fctx0 );
         d["scriptErrorReported"] = Json0( rf );
         const std::string ferr = Json0( rf ).value( "error", std::string() );
         const bool errorOk = rf.isError && Has( ferr, "*** Error" ) && Has( ferr, "NoiseEvaluation.js" )
                           && !Has( ferr, U8( productionRoot ) );
         // File names only reach the model: the install folder is scrubbed from the console text.
         const bool foldersOk = !Has( con, U8( productionRoot ) ) && Has( con, "NoiseEvaluation.js" );
         const bool proof1 = !r1.isError && Has( con, "Scaled Noise Evaluation Script" ) && Has( con, "PCScriptT1" )
                          && !Has( con, "*** Error" ) && errorOk && foldersOk && asks == 2
                          && Has( U8( html ), "PTeam" ) && Has( U8( html ), "NoiseEvaluation.js" ) && Has( U8( html ), "PCScriptT1" );

         // (ii) an unsigned script in src/scripts is refused by the module.
         ScriptEntry e2;
         String why2;
         const std::string rc = U8( productionRoot ) + "/RCAstro/RCAstroBXT.js";
         const bool rcExists = !RealPath8( rc ).empty();
         const bool elig2 = rcExists && CheckScriptEligibility( FromU8( rc ), productionRoot, e2, why2 );
         const int asksBefore2 = asks;   // 2: the noisy and the flat run
         const ToolOutcome r2 = ExecuteTool( ToolCall{ "s2", "run_script", { { "script_id", "RCAstroBXT" } } }, ctx );
         d["proofUnsigned"] = { { "file", rc }, { "exists", rcExists }, { "eligible", elig2 }, { "reason", U8( why2 ) },
                                { "inCatalogRefused", rcRefused }, { "tool", Text0( r2 ) }, { "asked", asks - asksBefore2 } };
         const bool proof2 = rcExists && !elig2 && Has( U8( why2 ), "not signed" ) && rcRefused
                          && r2.isError && Has( Text0( r2 ), "not signed" ) && asks == asksBefore2;

         // A private script tree for the tampering and escape cases (the PixInsight install is never written).
         const std::string fx = ScratchDir() + "/scripts-fixture";
         MakeDir( fx );
         MakeDir( fx + "/Good" );
         MakeDir( fx + "/Tampered" );
         MakeDir( fx + "/CommentOnly" );
         MakeDir( fx + "/Link" );
         const std::string outside = ScratchDir() + "/outside";
         MakeDir( outside );
         const std::string srcJs = U8( productionRoot ) + "/NoiseEvaluation/NoiseEvaluation.js";
         const std::string srcSig = U8( productionRoot ) + "/NoiseEvaluation/NoiseEvaluation.xsgn";
         const std::string code = ReadText( srcJs );
         for ( const char* dir : { "/Good", "/Tampered", "/CommentOnly" } )
            CopyFile8( fx + dir + "/NoiseEvaluation.xsgn", srcSig );
         CopyFile8( fx + "/Good/NoiseEvaluation.js", srcJs );
         std::string tampered = code;
         const size_t at = tampered.find( "Scaled Noise Evaluation Script v" );
         if ( at == std::string::npos )
            throw Error( "NoiseEvaluation.js changed: the tamper anchor is gone" );
         tampered.replace( at, 6, "Forged" );   // a code change: a string literal the script prints
         WriteText( fx + "/Tampered/NoiseEvaluation.js", tampered );
         WriteText( fx + "/CommentOnly/NoiseEvaluation.js", code + "\n// a comment added after signing\n" );
         CopyFile8( outside + "/NoiseEvaluation.js", srcJs );
         CopyFile8( outside + "/NoiseEvaluation.xsgn", srcSig );
         ::unlink( (fx + "/Link/Escape.js").c_str() );
         if ( ::symlink( (outside + "/NoiseEvaluation.js").c_str(), (fx + "/Link/Escape.js").c_str() ) != 0 )
            throw Error( "cannot create the escape symlink" );
         const String fxRoot = FromU8( RealPath8( fx ) );

         // (iii) a tampered copy of a signed script (a code edit after signing) is refused by the module.
         ScriptEntry eg, et, ec;
         String whyG, whyT, whyC;
         const bool good = CheckScriptEligibility( fxRoot + "/Good/NoiseEvaluation.js", fxRoot, eg, whyG );
         const bool tamp = CheckScriptEligibility( fxRoot + "/Tampered/NoiseEvaluation.js", fxRoot, et, whyT );
         const bool comm = CheckScriptEligibility( fxRoot + "/CommentOnly/NoiseEvaluation.js", fxRoot, ec, whyC );
         int fxAsks = 0;
         ToolContext fctx = BaseContext( t1.MainView().FullId(), fxAsks, true );
         fctx.scriptsRootOverride = fxRoot;
         const ToolOutcome l3 = ExecuteTool( ToolCall{ "s3", "list_scripts", { { "query", "noise" } } }, fctx );
         const nlohmann::json lj = Json0( l3 );
         bool tamperedListedRefused = false;
         for ( const nlohmann::json& x : lj.value( "notRunnable", nlohmann::json::array() ) )
            if ( x.value( "file", std::string() ) == "Tampered/NoiseEvaluation.js" )
               tamperedListedRefused = Has( x.value( "whyNot", std::string() ), "does not match" );
         d["proofTampered"] = { { "controlCopyEligible", good }, { "controlReason", U8( whyG ) },
                                { "tamperedEligible", tamp }, { "tamperedReason", U8( whyT ) },
                                { "commentOnlyEligible", comm }, { "commentOnlyReason", U8( whyC ) },
                                { "listScripts", lj }, { "refusedBy", "the module (Security.getScriptSignature valid=false), before any confirmation" } };
         const bool proof3 = good && !tamp && Has( U8( whyT ), "does not match" ) && tamperedListedRefused && fxAsks == 0;

         // (iv) a script outside src/scripts is refused (a valid signed copy; a link escaping the tree; "..").
         ScriptEntry eo, el, ed, er;
         String whyO, whyL, whyD, whyR;
         const bool outsideOk = CheckScriptEligibility( FromU8( outside + "/NoiseEvaluation.js" ), productionRoot, eo, whyO );
         const bool linkOk = CheckScriptEligibility( fxRoot + "/Link/Escape.js", fxRoot, el, whyL );
         const bool dotsOk = CheckScriptEligibility( fxRoot + "/Good/../Good/NoiseEvaluation.js", fxRoot, ed, whyD );
         const bool relOk = CheckScriptEligibility( "Good/NoiseEvaluation.js", fxRoot, er, whyR );
         // Control: the same copy IS validly signed (with its own folder as the tree it is eligible), so
         // the refusal above is the location rule alone.
         ScriptEntry eSame;
         String whySame;
         const bool outsideItselfValid = CheckScriptEligibility( FromU8( outside + "/NoiseEvaluation.js" ),
                                                                 FromU8( RealPath8( ScratchDir() ) ), eSame, whySame );
         d["proofOutside"] = { { "signedCopyOutside", outsideOk }, { "outsideReason", U8( whyO ) },
                               { "sameCopyEligibleWhenItsOwnDirIsTheRoot", outsideItselfValid },
                               { "escapeLink", linkOk }, { "escapeReason", U8( whyL ) },
                               { "dotDot", dotsOk }, { "dotDotReason", U8( whyD ) },
                               { "relative", relOk }, { "relativeReason", U8( whyR ) } };
         const bool proof4 = !outsideOk && Has( U8( whyO ), "outside" ) && outsideItselfValid
                          && !linkOk && Has( U8( whyL ), "symbolic link" ) && !dotsOk && Has( U8( whyD ), "'..'" )
                          && !relOk && Has( U8( whyR ), "not absolute" );

         // Advisor never runs a script; an unknown id never asks.
         int advAsks = 0;
         ToolContext actx = BaseContext( t1.MainView().FullId(), advAsks, true );
         actx.mode = AgentMode::Advisor;
         const ToolOutcome adv = ExecuteTool( ToolCall{ "s4", "run_script", { { "script_id", "NoiseEvaluation" } } }, actx );
         const ToolOutcome unk = ExecuteTool( ToolCall{ "s5", "run_script", { { "script_id", "NoSuchScript" } } }, ctx );
         const bool gatesOk = adv.isError && Has( Text0( adv ), "Advisor" ) && advAsks == 0 && unk.isError
                           && Has( Text0( unk ), "list_scripts" );
         d["advisor"] = Text0( adv );
         d["unknownId"] = Text0( unk );

         d["catalogOk"] = catalogOk;
         d["proofs"] = { { "i_eligibleRuns", proof1 }, { "ii_unsignedRefused", proof2 }, { "iii_tamperedRefused", proof3 },
                         { "iv_outsideRefused", proof4 } };
         ok = catalogOk && proof2 && proof3 && proof4;
         runOk = proof1 && gatesOk;
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      CloseNewWindows( windowsBefore );
      out["scriptEligibilityDetail"] = d;
      out["scriptEligibilityOk"] = ok;
      out["runScriptOk"] = runOk;
      allOk = allOk && ok && runOk;
   }

   // ---- S4c: nothing the script tools say names the user's home or account -------
   // A PixInsight install can live under /home/<user>. A private script tree is built
   // under $HOME/.cache (removed afterwards) with: a validly signed copy of a PixInsight
   // script (it runs, and its console names its file path), an unsigned script whose
   // #feature-info, #include and Parameters key name absolute home paths, a link leading
   // out of the tree, and a tampered copy. Every result list_scripts, describe_script and
   // run_script hand the model is scanned for "/home/" and the user name.
   SelfTestSectionMark( "S4c script tools leak no home path" );
   {
      nlohmann::json d;
      bool ok = false;
      const std::set<std::string> windowsBefore = OpenMainViewIds();
      std::string base;
      try
      {
         const char* home = std::getenv( "HOME" );
         if ( home == nullptr || *home == '\0' )
            throw Error( "HOME is not set" );
         const std::string homeReal = RealPath8( home );
         std::string user = homeReal.substr( homeReal.rfind( '/' ) + 1 );
         if ( const char* u = std::getenv( "USER" ) )
            if ( *u != '\0' )
               user = u;
         MakeDir( homeReal + "/.cache" );
         std::string tmpl = homeReal + "/.cache/picopilot-selftest-XXXXXX";
         std::vector<char> buf( tmpl.begin(), tmpl.end() );
         buf.push_back( '\0' );
         if ( ::mkdtemp( buf.data() ) == nullptr )
            throw Error( "cannot create a private folder under ~/.cache" );
         base = buf.data();
         const std::string root = base + "/PixInsight/src/scripts";
         MakeDir( base + "/PixInsight" );
         MakeDir( base + "/PixInsight/src" );
         MakeDir( root );
         for ( const char* sub : { "/Good", "/Plain", "/Tampered", "/Link" } )
            MakeDir( root + sub );
         const std::string srcJs = U8( productionRoot ) + "/NoiseEvaluation/NoiseEvaluation.js";
         const std::string srcSig = U8( productionRoot ) + "/NoiseEvaluation/NoiseEvaluation.xsgn";
         CopyFile8( root + "/Good/NoiseEvaluation.js", srcJs );
         CopyFile8( root + "/Good/NoiseEvaluation.xsgn", srcSig );
         std::string tampered = ReadText( srcJs );
         tampered.replace( tampered.find( "Scaled Noise Evaluation Script v" ), 6, "Forged" );
         WriteText( root + "/Tampered/NoiseEvaluation.js", tampered );
         CopyFile8( root + "/Tampered/NoiseEvaluation.xsgn", srcSig );
         const std::string homeData = homeReal + "/astro/M31/session1";
         WriteText( root + "/Plain/Plain.js",
            "#feature-id PCPlain : PICopilotTest > Plain\n"
            "#feature-info Reads " + homeData + "/light_001.fits and file://" + homeReal + "/astro/x.xisf\n"
            "#include \"" + homeReal + "/lib/helpers.jsh\"\n"
            "var k = Parameters.get( \"" + homeReal + "/\" + 'x' );\n"
            "if ( Parameters.isViewTarget ) console.writeln( '" + homeData + "' );\n" );
         ::unlink( (root + "/Link/Escape.js").c_str() );
         if ( ::symlink( (base + "/PixInsight/Escape.js").c_str(), (root + "/Link/Escape.js").c_str() ) != 0 )
            throw Error( "cannot create the escape link" );
         WriteText( base + "/PixInsight/Escape.js", "#feature-id PCEscape : PICopilotTest > Escape\n" );
         CopyFile8( base + "/PixInsight/Escape.xsgn", srcSig );
         const String fxRoot = FromU8( RealPath8( root ) );

         RgbWindow hw( "PCHomeLeak", 0.2, 0.3, 0.4 );
         hw.AddNoise( 0.2, 0.05 );
         int asks = 0;
         ToolContext ctx = BaseContext( hw.MainView().FullId(), asks, true );
         ctx.scriptsRootOverride = fxRoot;
         std::vector<std::pair<std::string, ToolOutcome>> calls;   // pcl-move-ok: push_back only, read in order
         calls.push_back( { "list", ExecuteTool( ToolCall{ "h1", "list_scripts", nlohmann::json::object() }, ctx ) } );
         calls.push_back( { "listPlain", ExecuteTool( ToolCall{ "h2", "list_scripts", { { "query", "plain" } } }, ctx ) } );
         calls.push_back( { "listEscape", ExecuteTool( ToolCall{ "h3", "list_scripts", { { "query", "link" } } }, ctx ) } );
         calls.push_back( { "listNoise", ExecuteTool( ToolCall{ "h4", "list_scripts", { { "query", "noise" } } }, ctx ) } );
         calls.push_back( { "describe", ExecuteTool( ToolCall{ "h5", "describe_script", { { "script_id", "NoiseEvaluation" } } }, ctx ) } );
         calls.push_back( { "describePlain", ExecuteTool( ToolCall{ "h6", "describe_script", { { "script_id", "PCPlain" } } }, ctx ) } );
         calls.push_back( { "runPlain", ExecuteTool( ToolCall{ "h7", "run_script", { { "script_id", "Plain" } } }, ctx ) } );
         calls.push_back( { "run", ExecuteTool( ToolCall{ "h8", "run_script", { { "script_id", "NoiseEvaluation" },
                                                                                 { "arguments", nlohmann::json::object() } } }, ctx ) } );
         // The unsigned script's own description (never offered to the model, since it is not
         // eligible) through the same choke point: info, includes and the key expression.
         ToolOutcome plainDesc;
         plainDesc.content.push_back( TextBlock( ScriptDescriptionJson(
            DescribeScriptSource( fxRoot + "/Plain/Plain.js", fxRoot, PixInsightIncludeDirectory() ) ).dump() ) );
         const std::string rawPlain = Text0( plainDesc );
         ScriptToolResultForModel( plainDesc );
         calls.push_back( { "plainDescriptionChokePoint", plainDesc } );

         bool clean = true;
         nlohmann::json texts = nlohmann::json::object();
         for ( const auto& c : calls )
         {
            std::string all;
            for ( const nlohmann::json& b : c.second.content )
               if ( b.value( "type", std::string() ) == "text" )
                  all += b.value( "text", std::string() );
            const bool leak = Has( all, "/home/" ) || Has( all, homeReal ) || Has( all, user );
            clean = clean && !leak;
            texts[c.first] = { { "leak", leak }, { "text", all.size() > 1500 ? all.substr( 0, 1500 ) : all } };
         }
         d["user"] = user.empty() ? "?" : "(set)";
         d["results"] = texts;
         const nlohmann::json run = Json0( calls[7].second );
         // The fixture really exercised the leaks: the raw description and console named the home folder.
         const bool exercised = Has( rawPlain, homeReal ) && !calls[7].second.isError
                             && Has( run.value( "console", std::string() ), "NoiseEvaluation.js" )
                             && Has( Text0( calls[2].second ), "symbolic link" ) && Has( Text0( calls[3].second ), "does not match" );
         d["exercised"] = exercised;
         ok = clean && exercised && asks == 1;
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      CloseNewWindows( windowsBefore );
      if ( !base.empty() )
         try { RemoveDirectoryTree( FromU8( base ) ); } catch ( ... ) {}
      out["scriptHomeLeakDetail"] = d;
      out["scriptHomeLeakOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S5: run_script arguments arrive in Parameters ----------------------------
   SelfTestSectionMark( "S5 script arguments reach Parameters" );
   {
      nlohmann::json d;
      bool ok = false;
      const std::set<std::string> windowsBefore = OpenMainViewIds();
      try
      {
         // A scratch probe script (unsigned, never in the catalog): this proves the
         // plumbing Console::ExecuteScript* -> Parameters, separately from eligibility.
         const std::string dir = ScratchDir() + "/args";
         MakeDir( dir );
         const std::string outGlobal = dir + "/global.json", outView = dir + "/view.json";
         auto probe = [&]( const std::string& outPath )
         {
            return "#feature-id PCArgsProbe : PICopilotTest > ArgsProbe\n"
                   "var o = { isGlobal: Parameters.isGlobalTarget, isView: Parameters.isViewTarget,\n"
                   "  target: Parameters.targetView ? Parameters.targetView.fullId : null,\n"
                   "  s: Parameters.get( \"s\" ), b: Parameters.getBoolean( \"b\" ), i: Parameters.getInteger( \"i\" ),\n"
                   "  r: Parameters.getReal( \"r\" ), u: Parameters.getUInt( \"u\" ), hasMissing: Parameters.has( \"missing\" ),\n"
                   "  rawB: Parameters.getString( \"b\" ), rawR: Parameters.getString( \"r\" ) };\n"
                   "File.writeTextFile( " + ScriptLiteral( FromU8( outPath ) ) + ", JSON.stringify( o ) );\n";
         };
         WriteText( dir + "/ArgsProbeG.js", probe( outGlobal ) );
         WriteText( dir + "/ArgsProbeV.js", probe( outView ) );
         const std::string root = RealPath8( dir );
         const ScriptDescription pd = DescribeScriptSource( FromU8( root + "/ArgsProbeG.js" ), FromU8( root ) );
         StringKeyValueList args;
         nlohmann::json shown;
         const nlohmann::json typed = { { "s", "h\xC3\xA9llo = w\xC3\xB6rld" }, { "b", true }, { "i", -42 }, { "r", 0.8 }, { "u", 7 } };
         const String verr = ValidateScriptArguments( pd, typed, args, shown );
         d["validate"] = U8( verr );
         d["description"] = ScriptDescriptionJson( pd );
         ::unlink( outGlobal.c_str() );
         ::unlink( outView.c_str() );
         String gErr, vErr;
         try { Console().ExecuteScriptGlobal( FromU8( root + "/ArgsProbeG.js" ), args ); }
         catch ( const pcl::Exception& x ) { gErr = x.Message(); }
         RgbWindow pv( "PCArgsView", 0.1, 0.1, 0.1 );
         try { Console().ExecuteScriptOn( pv.MainView(), FromU8( root + "/ArgsProbeV.js" ), args ); }
         catch ( const pcl::Exception& x ) { vErr = x.Message(); }
         nlohmann::json g, v;
         try { g = nlohmann::json::parse( ReadText( outGlobal ) ); } catch ( ... ) {}
         try { v = nlohmann::json::parse( ReadText( outView ) ); } catch ( ... ) {}
         d["global"] = g; d["view"] = v; d["globalError"] = U8( gErr ); d["viewError"] = U8( vErr );
         auto valuesOk = []( const nlohmann::json& o )
         {
            return o.is_object() && o.value( "s", std::string() ) == "h\xC3\xA9llo = w\xC3\xB6rld" && o.value( "b", false ) == true
                && o.value( "i", 0 ) == -42 && std::fabs( o.value( "r", 0.0 ) - 0.8 ) < 1e-12 && o.value( "u", 0 ) == 7
                && o.value( "hasMissing", true ) == false && o.value( "rawB", std::string() ) == "true"
                && o.value( "rawR", std::string() ) == "0.8";
         };
         // Characters on PixInsight's script command line (measured: run -x -p="key,value" ... "<file>"):
         // a comma and a backslash in a value arrive intact; a double quote is refused by the module.
         StringKeyValueList odd;
         nlohmann::json oddShown;
         const String oddErr = ValidateScriptArguments( pd, { { "s", "/mnt/My Data/a,b (1).xisf" } }, odd, oddShown );
         ::unlink( outGlobal.c_str() );
         String oErr;
         try { Console().ExecuteScriptGlobal( FromU8( root + "/ArgsProbeG.js" ), odd ); }
         catch ( const pcl::Exception& x ) { oErr = x.Message(); }
         nlohmann::json og;
         try { og = nlohmann::json::parse( ReadText( outGlobal ) ); } catch ( ... ) {}
         StringKeyValueList q;
         nlohmann::json qShown;
         const String quoteErr = ValidateScriptArguments( pd, { { "s", "x\" -p=\"i,99" } }, q, qShown );
         d["commaBackslash"] = { { "validate", U8( oddErr ) }, { "error", U8( oErr ) }, { "arrived", og } };
         d["quoteRefused"] = U8( quoteErr );
         const bool quotingOk = oddErr.IsEmpty() && oErr.IsEmpty() && og.is_object()
                             && og.value( "s", std::string() ) == "/mnt/My Data/a,b (1).xisf"
                             && quoteErr.Contains( "does not pass" ) && q.IsEmpty();
         d["quotingOk"] = quotingOk;
         ok = quotingOk && verr.IsEmpty() && pd.parameters.size() == 6 && gErr.IsEmpty() && vErr.IsEmpty()
           && valuesOk( g ) && g.value( "isGlobal", false ) && !g.value( "isView", true )
           && valuesOk( v ) && v.value( "isView", false ) && !v.value( "isGlobal", true )
           && v.value( "target", std::string() ) == "PCArgsView";
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      CloseNewWindows( windowsBefore );
      out["scriptArgsPlumbingDetail"] = d;
      out["scriptArgsPlumbingOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S5b: PixInsight's script command line, character by character -------------
   // Measured, not assumed: the core passes arguments as run -x -p="key,value" ... "<file>".
   // Every printable ASCII character goes through it in three positions (inside, leading,
   // trailing) between two sentinel arguments, plus multi-character injection candidates.
   // The module's allowlist (ScriptArgumentCharAllowed) must hold only characters that
   // arrive intact without disturbing the sentinels or adding arguments; everything else
   // is refused by ValidateScriptArguments. What the refused characters do is recorded.
   SelfTestSectionMark( "S5b script argument characters" );
   {
      nlohmann::json d;
      bool ok = false;
      try
      {
         const std::string dir = ScratchDir() + "/argchars";
         MakeDir( dir );
         const std::string outPath = dir + "/out.json";
         WriteText( dir + "/CharProbe.js",
            "#feature-id PCCharProbe : PICopilotTest > CharProbe\n"
            "var o = { s0: Parameters.get( \"s0\" ), p1: Parameters.get( \"p1\" ), p2: Parameters.get( \"p2\" ),\n"
            "  p3: Parameters.get( \"p3\" ), s9: Parameters.get( \"s9\" ), inj: Parameters.has( \"inj\" ),\n"
            "  injValue: Parameters.get( \"inj\" ), isGlobal: Parameters.isGlobalTarget };\n"
            "File.writeTextFile( " + ScriptLiteral( FromU8( outPath ) ) + ", JSON.stringify( o ) );\n" );
         const String probe = FromU8( RealPath8( dir ) + "/CharProbe.js" );
         // One run: returns what arrived, the core's command line and any console error line.
         auto runProbe = [&]( const std::vector<std::pair<std::string, std::string>>& kv, nlohmann::json& rec ) -> nlohmann::json
         {
            StringKeyValueList list;
            for ( const auto& e : kv )
               list << StringKeyValue( FromU8( e.first ), FromU8( e.second ) );
            ::unlink( outPath.c_str() );
            EvaluateAsciiJson( "console.beginLog(); return pcAscii( JSON.stringify( true ) );" );
            String err;
            try { Console().ExecuteScriptGlobal( probe, list ); }
            catch ( const pcl::Exception& x ) { err = x.Message(); }
            const nlohmann::json log = EvaluateAsciiJson(
               "var b = console.endLog(); return pcAscii( JSON.stringify( b ? String( b.toBase64() ) : \"\" ) );" );
            const ByteArray raw = IsoString( log.get<std::string>().c_str() ).FromBase64();
            const std::string text( reinterpret_cast<const char*>( raw.Begin() ), raw.Length() );
            std::string cmd, errs;
            std::istringstream ls( text );
            for ( std::string line; std::getline( ls, line ); )
            {
               if ( line.find( "run -x" ) != std::string::npos && cmd.empty() )
                  cmd = line.substr( line.find( "run -x" ) );
               if ( line.find( "*** Error" ) != std::string::npos || line.find( "Unknown" ) != std::string::npos
                    || line.find( "Invalid" ) != std::string::npos )
                  errs += line + "\n";
            }
            rec["cmd"] = cmd.size() > 400 ? cmd.substr( 0, 400 ) : cmd;
            if ( !errs.empty() )
               rec["consoleErrors"] = errs.substr( 0, 400 );
            if ( !err.IsEmpty() )
               rec["executeError"] = U8( err );
            nlohmann::json got;
            try { got = nlohmann::json::parse( ReadText( outPath ) ); } catch ( ... ) {}
            return got;
         };
         nlohmann::json chars = nlohmann::json::array();
         std::string safe, unsafe;
         bool allowedAllSafe = true, refusedAllRefused = true;
         for ( int c = 0x20; c <= 0x7E; ++c )
         {
            const std::string ch( 1, char( c ) );
            const std::string v1 = "a" + ch + "b", v2 = ch + "a", v3 = "a" + ch;
            nlohmann::json rec = { { "char", ch }, { "code", c } };
            // Never sent: '"' and '\' wedge the core's command line (an unterminated string swallows what the
            // instance runs next -- measured: the instance then never exits). Their refusal is asserted below.
            const bool wedges = c == '"' || c == '\\';
            const nlohmann::json got = wedges ? nlohmann::json()
                                     : runProbe( { { "s0", "first" }, { "p1", v1 }, { "p2", v2 }, { "p3", v3 }, { "s9", "last" } }, rec );
            if ( wedges )
               rec["notSent"] = "breaks PixInsight's script command line (measured 2026-10-13); refused by the module";
            const bool intact = !wedges && got.is_object() && got.value( "s0", std::string() ) == "first" && got.value( "p1", std::string() ) == v1
                             && got.value( "p2", std::string() ) == v2 && got.value( "p3", std::string() ) == v3
                             && got.value( "s9", std::string() ) == "last" && !got.value( "inj", true );
            rec["intact"] = intact;
            if ( !intact )
               rec["arrived"] = got;
            const bool allowed = ScriptArgumentCharAllowed( uint32( c ) );
            rec["allowed"] = allowed;
            (intact ? safe : unsafe) += ch;
            if ( allowed && !intact )
               allowedAllSafe = false;
            // The module refuses every character it does not allow.
            if ( !allowed )
            {
               ScriptDescription one;
               one.ok = true;
               ScriptParameter sp;
               sp.name = "p1";
               sp.type = "string";
               sp.getters = { "get" };
               one.parameters.push_back( sp );
               StringKeyValueList kv;
               nlohmann::json shown;
               const String e = ValidateScriptArguments( one, { { "p1", v1 } }, kv, shown );
               if ( e.IsEmpty() || !kv.IsEmpty() )
                  refusedAllRefused = false;
            }
            if ( !intact || !allowed )
               chars.push_back( rec );
         }
         d["notIntactOrNotAllowed"] = chars;
         d["intactChars"] = safe;
         d["notIntactChars"] = unsafe;
         // Multi-character candidates (recorded; the allowlist decides).
         const std::vector<std::string> cands = {   // pcl-move-ok: const, read only
            "a\\", "a\\\\", "a\\\"b", "x\" -p=\"inj,1", "a -p=inj,1", "a;b", "$HOME", "${HOME}", "%PATH%", "a|b",
            "`id`", "a'b", "a&b", "a>b", "/tmp/My Data/x y.fits", "/home/x/Scott's M81&M82 50%; v2 (final).xisf",
            "h\xC3\xA9llo w\xC3\xB6rld", "\xE6\x97\xA5\xE6\x9C\xAC", "\xF0\x9F\x93\xB7" };
         nlohmann::json multi = nlohmann::json::array();
         bool multiOk = true;
         for ( const std::string& v : cands )
         {
            nlohmann::json rec = { { "value", v } };
            const bool wedges = v.find( '"' ) != std::string::npos || v.find( '\\' ) != std::string::npos;
            const nlohmann::json got = wedges ? nlohmann::json() : runProbe( { { "s0", "first" }, { "p1", v }, { "s9", "last" } }, rec );
            if ( wedges )
               rec["notSent"] = "contains '\"' or '\\' (refused by the module; measured to break the command line)";
            const bool intact = !wedges && got.is_object() && got.value( "s0", std::string() ) == "first" && got.value( "p1", std::string() ) == v
                             && got.value( "s9", std::string() ) == "last" && !got.value( "inj", true );
            rec["intact"] = intact;
            if ( !intact )
               rec["arrived"] = got;
            bool allowed = true;
            const String v16 = FromU8( v );
            for ( size_type i = 0; i < v16.Length(); )
            {
               size_type u = 1;
               allowed = allowed && ScriptArgumentCharAllowed( CodePointAt( v16, i, u ) );
               i += u;
            }
            rec["allowed"] = allowed;
            if ( allowed && !intact )
               multiOk = false;
            multi.push_back( rec );
         }
         d["candidates"] = multi;
         // A path with spaces round-trips (allowed and intact).
         bool spacePathOk = false;
         for ( const nlohmann::json& m : multi )
            if ( m.value( "value", std::string() ) == "/tmp/My Data/x y.fits" )
               spacePathOk = m.value( "intact", false ) && m.value( "allowed", false );
         // Parameter NAMES: [A-Za-z0-9_] only (a ',' or '"' in a name would split or end the -p="key,value" pair).
         bool keysOk = true;
         for ( const char* bad : { "a,b", "a\"b", "a b", "a-b" } )
         {
            ScriptDescription one;
            one.ok = true;
            ScriptParameter sp;
            sp.name = bad;
            sp.type = "string";
            sp.getters = { "get" };
            one.parameters.push_back( sp );
            StringKeyValueList kv;
            nlohmann::json shown;
            const String e = ValidateScriptArguments( one, { { bad, "x" } }, kv, shown );
            keysOk = keysOk && !e.IsEmpty() && kv.IsEmpty();
         }
         d["checks"] = { { "allowedAllIntact", allowedAllSafe }, { "refusedAllRefused", refusedAllRefused },
                         { "multiAllowedIntact", multiOk }, { "spacePath", spacePathOk }, { "keyNames", keysOk } };
         ok = allowedAllSafe && refusedAllRefused && multiOk && spacePathOk && keysOk;
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      out["scriptArgCharsDetail"] = d;
      out["scriptArgCharsOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S6: describe_script + run_script on a real PixInsight script ------------
   SelfTestSectionMark( "S6 describe + run CorrectMagentaStars headless" );
   {
      nlohmann::json d;
      bool ok = false;
      const std::set<std::string> windowsBefore = OpenMainViewIds();
      try
      {
         int asks = 0;
         RgbWindow a( "PCCmsA", 0.6, 0.3, 0.5 ), b( "PCCmsB", 0.6, 0.3, 0.5 ), c( "PCCmsC", 0.6, 0.3, 0.5 );
         ToolContext ctx = BaseContext( a.MainView().FullId(), asks, true );
         const ToolOutcome desc = ExecuteTool( ToolCall{ "c0", "describe_script", { { "script_id", "CorrectMagentaStars" } } }, ctx );
         const nlohmann::json dj = Json0( desc );
         d["describe"] = dj;
         bool describeOk = !desc.isError && dj.contains( "description" );
         if ( describeOk )
         {
            const nlohmann::json& ps = dj["description"]["parameters"];
            describeOk = ps.size() == 7 && ps[0].value( "name", std::string() ) == "CMSAmount" && ps[0].value( "type", std::string() ) == "real"
                      && ps[0].value( "default", nlohmann::json() ) == 0.8
                      && dj["description"]["interaction"].value( "kind", std::string() ) == "honours-parameters-when-targeted";
         }
         const nlohmann::json common = { { "useAutoMask", false }, { "showAutoMask", false }, { "autodetectLinearity", false },
                                         { "linearImage", false }, { "CMSPresLight", false } };
         nlohmann::json argsA = common, argsB = common;
         argsA["CMSAmount"] = 0.25; argsA["protectionMethod"] = 0;   // G' = 0.3*0.75 + avg(0.6,0.5)*0.25 = 0.3625
         argsB["CMSAmount"] = 1.0;  argsB["protectionMethod"] = 1;   // G' = max(0.6,0.5) = 0.6
         const std::set<std::string> before = OpenMainViewIds();
         const ToolOutcome ra = ExecuteTool( ToolCall{ "c1", "run_script", { { "script_id", "CorrectMagentaStars" }, { "arguments", argsA },
                                                                            { "context", "view" }, { "reason", "fix magenta" } } }, ctx );
         ctx.turnViewId = b.MainView().FullId();
         const ToolOutcome rb = ExecuteTool( ToolCall{ "c2", "run_script", { { "script_id", "CorrectMagentaStars" }, { "arguments", argsB } } }, ctx );
         const std::set<std::string> after = OpenMainViewIds();
         int declineAsks = 0;
         ToolContext dctx = BaseContext( c.MainView().FullId(), declineAsks, false );
         const ToolOutcome rc = ExecuteTool( ToolCall{ "c3", "run_script", { { "script_id", "CorrectMagentaStars" }, { "arguments", argsB } } }, dctx );
         const ToolOutcome bad = ExecuteTool( ToolCall{ "c4", "run_script", { { "script_id", "CorrectMagentaStars" },
                                                                             { "arguments", { { "CMSAmount", "lots" } } } } }, ctx );
         const ToolOutcome unknownArg = ExecuteTool( ToolCall{ "c5", "run_script", { { "script_id", "CorrectMagentaStars" },
                                                                                    { "arguments", { { "amount", 0.5 } } } } }, ctx );
         const HistorySnapshot ha = ReadViewHistory( a.MainView().FullId(), 0 );
         nlohmann::json steps = nlohmann::json::array();
         for ( const HistoryStep& h : ha.steps )
            steps.push_back( h.processId );
         const double ga = a.Mean( 1 ), gb = b.Mean( 1 ), gc = c.Mean( 1 );
         d["runA"] = Json0( ra ); d["runB"] = Json0( rb ); d["declined"] = Text0( rc );
         d["badType"] = Text0( bad ); d["unknownArg"] = Text0( unknownArg );
         d["greenMeans"] = { { "A_amount0.25_avg", ga }, { "B_amount1_max", gb }, { "C_declined", gc } };
         d["redBlueA"] = { a.Mean( 0 ), a.Mean( 2 ) };
         d["historyA"] = steps;
         d["asks"] = asks; d["declineAsks"] = declineAsks;
         ok = describeOk && !ra.isError && !rb.isError && std::fabs( ga - 0.3625 ) < 1e-5 && std::fabs( gb - 0.6 ) < 1e-5
           && std::fabs( a.Mean( 0 ) - 0.6 ) < 1e-6 && std::fabs( a.Mean( 2 ) - 0.5 ) < 1e-6
           && before == after                                       // no mask window (showAutoMask false)
           && rc.isError && Has( Text0( rc ), "declined" ) && declineAsks == 1 && std::fabs( gc - 0.3 ) < 1e-6
           && bad.isError && Has( Text0( bad ), "must be a number" ) && unknownArg.isError && Has( Text0( unknownArg ), "Valid parameters" )
           && asks == 2 && ha.ok && ha.steps.size() == 1;
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      CloseNewWindows( windowsBefore );
      out["scriptAcceptanceDetail"] = d;
      out["scriptAcceptanceOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S6b: a script's steps in an image journey -----------------------------------
   SelfTestSectionMark( "S6b run_script in an image journey" );
   {
      nlohmann::json d;
      bool ok = false;
      const std::set<std::string> windowsBefore = OpenMainViewIds();
      try
      {
         const std::string lib = ScratchDir() + "/script-journeys";
         MakeDir( lib );
         String oe;
         std::unique_ptr<JourneyStore> store = JourneyStore::Open( FromU8( RealPath8( lib ) ), oe );
         if ( !store )
            throw Error( "journey store: " + oe );
         JourneyTracker trk( store.get() );
         trk.SetEnabled( true );
         RgbWindow jw( "PCScriptJourney", 0.6, 0.3, 0.5 );
         String se;
         const int64 jid = trk.StartJourneyFor( jw.MainView(), se, JourneyWallNow() );
         if ( jid == 0 )
            throw Error( "StartJourneyFor: " + se );
         auto settle = [&]( int n )
         {
            for ( int i = 0; i < n; ++i )
            {
               ThePICopilotModule->ProcessEvents( true );
               std::this_thread::sleep_for( std::chrono::milliseconds( 60 ) );
               trk.Tick( JourneyWallNow(), true );
            }
         };
         settle( 5 );
         JourneyToolHost host;
         host.store = store.get();
         host.tracker = &trk;
         int asks = 0;
         ToolContext ctx = BaseContext( jw.MainView().FullId(), asks, true );
         ctx.journeys = &host;
         const nlohmann::json args = { { "CMSAmount", 1.0 }, { "protectionMethod", 1 }, { "useAutoMask", false },
                                       { "showAutoMask", false }, { "autodetectLinearity", false }, { "linearImage", false },
                                       { "CMSPresLight", false } };
         const ToolOutcome r = ExecuteTool( ToolCall{ "j1", "run_script", { { "script_id", "CorrectMagentaStars" }, { "arguments", args },
                                                                           { "reason", "neutralize magenta" } } }, ctx );
         d["run"] = Json0( r );
         nlohmann::json rows = nlohmann::json::array();
         bool found = false;
         for ( int attempt = 0; attempt < 40 && !found; ++attempt )
         {
            settle( 1 );
            rows = nlohmann::json::array();
            for ( const ImageRow& img : store->Images( jid ) )
               for ( const StepRow& st : store->Steps( img.id, false ) )
               {
                  if ( st.params.value( "base", false ) )
                     continue;
                  const std::string manual = ManualWhy( st );
                  rows.push_back( { { "processId", st.processId }, { "actor", st.actor }, { "reason", st.reason },
                                    { "replayable", st.params.value( "replayable", true ) }, { "manualWhy", manual } } );
                  found = found || (st.processId == "Script" && st.actor == "copilot"
                                    && Has( st.reason, "run_script CorrectMagentaStars: neutralize magenta" )
                                    && !st.params.value( "replayable", true ) && !manual.empty());
               }
         }
         d["steps"] = rows;
         ok = !r.isError && found && Json0( r ).value( "journeyStepsAttributed", 0 ) == 1;
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { d["error"] = x.what(); }
      CloseNewWindows( windowsBefore );
      out["scriptJourneyDetail"] = d;
      out["scriptJourneyOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S7: nothing reaches Anthropic before the user agrees ----------------------
   SelfTestSectionMark( "S7 Anthropic consent gate" );
   {
      nlohmann::json d;
      bool ok = false;
      try
      {
         const bool grantedAtStart = DataConsent::Granted();   // the harness agreed for this isolated slot
         DataConsent::Revoke();
         const bool revoked = !DataConsent::Granted() && DataConsent::GrantedAt().IsEmpty();
         // A request to the loopback stall server: had it been sent, it would wait
         // for the 3 s deadline. Refused at construction, it ends at once, untouched.
         const char* stall = std::getenv( "PICOPILOT_SELFTEST_STALL_URL" );
         const String url = stall != nullptr ? String( stall ) : String( "http://127.0.0.1:9/v1/messages" );
         Array<AnthropicMessage> h;
         AnthropicMessage m;
         m.role = "user";
         m.content = "consent probe";
         h << m;
         const auto t0 = std::chrono::steady_clock::now();
         AnthropicResult r;
         {
            AnthropicRequest req( "sk-not-a-key", PICOPILOT_DEFAULT_MODEL, "probe", h, url, 3 );
            r = req.Perform();
         }
         const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
         // The production worker path (chat turns and journey write-ups both use ChatThread).
         AnthropicResult rt;
         const auto t1 = std::chrono::steady_clock::now();
         {
            ChatThread th( "sk-not-a-key", "probe", h, PICOPILOT_DEFAULT_MODEL, url, 3 );
            th.Start();
            th.Wait();
            th.TryTakeResult( rt );
         }
         const double msThread = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t1 ).count();
         const bool refusedOk = !r.ok && r.errorKind == RequestErrorKind::Consent && r.httpStatus == 0 && ms < 500
                             && Has( U8( r.error ), "Nothing was sent" )
                             && !rt.ok && rt.errorKind == RequestErrorKind::Consent && msThread < 1000;
         const std::string html = U8( DataConsent::ConsentHtml() );
         bool textOk = Has( html, "Anthropic" ) && Has( html, "api.anthropic.com" ) && Has( html, "https://www.anthropic.com/legal/privacy" )
                    && Has( html, "Claude Haiku" ) && Has( html, "journey" ) && Has( html, "file name" ) && Has( html, "Never sent" )
                    && Has( html, "preview" ) && Has( html, "statistics" ) && Has( html, "FITS keywords" ) && Has( html, "withdraw" )
                    && Has( html, "Do you agree" ) && !Has( U8( DataConsent::ConsentHtml( false ) ), "Do you agree" );
         for ( const char* k : PICopilotRedactedFitsKeywords )
            textOk = textOk && (Has( html, k ) || (std::string( k ).compare( 0, 7, "OBSGEO-" ) == 0 && Has( html, "OBSGEO-B/L/H" )));
         DataConsent::Grant();
         const bool regranted = DataConsent::Granted() && DataConsent::GrantedAt().Length() == 10;
         Settings::Write( "PICopilot/AnthropicConsentVersion", DataConsent::kVersion - 1 );
         const bool oldVersionNotEnough = !DataConsent::Granted();
         DataConsent::Grant();
         d["grantedAtStart"] = grantedAtStart; d["revoked"] = revoked;
         d["direct"] = { { "error", U8( r.error ) }, { "kind", int( r.errorKind ) }, { "ms", ms } };
         d["thread"] = { { "error", U8( rt.error ) }, { "kind", int( rt.errorKind ) }, { "ms", msThread } };
         d["textOk"] = textOk; d["regranted"] = regranted; d["oldVersionNotEnough"] = oldVersionNotEnough;
         ok = grantedAtStart && revoked && refusedOk && textOk && regranted && oldVersionNotEnough && DataConsent::Granted();
      }
      catch ( const pcl::Exception& x ) { d["error"] = U8( x.Message() ); DataConsent::Grant(); }
      catch ( const std::exception& x ) { d["error"] = x.what(); DataConsent::Grant(); }
      out["consentDetail"] = d;
      out["consentOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- S8: FITS privacy: location/identity keywords and folders never sent --------
   // Fail-closed (security review of 0.2.2.0): keyword names any case, HIERARCH and long
   // names, location under other names, COMMENT/HISTORY text, Windows/UNC/~/file:// paths,
   // paths inside text, relative paths, and values cut mid-path.
   SelfTestSectionMark( "S8 FITS privacy" );
   {
      nlohmann::json d = nlohmann::json::array();
      bool ok = true;
      try
      {
         const std::pair<const char*, const char*> cases[] = {
            { "/home/someone/data/M31/light_001.fits", "light_001.fits" },
            { "C:\\Users\\bob\\M31\\a.xisf, /mnt/qnap/x y/b.xisf", "a.xisf, b.xisf" },
            { "ImageIntegration.images: /home/u/f1.xisf /home/u/f2.xisf", "ImageIntegration.images: f1.xisf f2.xisf" },
            { "Integrated from /mnt/qnap/astro_data/M 31/x.fits and 3 others", "Integrated from x.fits and 3 others" },
            { "~/stack/master.xisf", "master.xisf" },
            { "~\\stack\\master.xisf", "master.xisf" },
            { "\\\\server\\share\\dir\\f.fits", "f.fits" },
            { "file='/tmp/a/b.fits'", "file='b.fits'" },
            { "file:///home/u/x.fits", "x.fits" },
            { "FILE:///C:/Users/bob/x.fits", "x.fits" },
            { "src{/home/u/x.fits}", "src{x.fits}" },          // after any non-alphanumeric
            { "a*/home/u/x.fits", "a*x.fits" },
            { "<C:\\Users\\bob\\x.fits>", "<x.fits>" },
            { "/mnt/qnap/astro_data", "[path]" },               // a folder: its name is not kept
            { "/home/u/data/M31/sessi", "[path]" },             // cut mid-path
            { "data/M31/raw", "[path]" },                       // relative
            { "M31/light_001.fits", "light_001.fits" },
            { "../x/y.fits", "y.fits" },
            { "https://example.com/a/b", "https:[path]" },
            { "$T/2", "$T/2" },
            { "ratio 1/2", "ratio 1/2" },
            { "2026/10/13", "2026/10/13" },
            { "N/A", "N/A" },
            { "Ha/OIII", "Ha/OIII" },
            { "Master Light", "Master Light" },
            { "L / R", "L / R" },
            { "/", "/" },
         };
         for ( const auto& c : cases )
         {
            const std::string got = FitsValueWithoutPaths( c.first );
            const bool caseOk = got == c.second;
            d.push_back( { { "in", c.first }, { "out", got }, { "ok", caseOk } } );
            ok = ok && caseOk;
         }
         const char* redacted[] = { "SITELAT", "sitelat", " SiteLong ", "SITEELEV", "SITENAME", "OBSSITE", "LAT-OBS",
            "LONG-OBS", "ALT-OBS", "OBSGEO-X", "OBSGEO-Y", "OBSGEO-Z", "OBSGEO-B", "OBSGEO-L", "OBSGEO-H", "GEOLAT",
            "GEOLON", "HIERARCH ESO TEL GEOLAT", "ESO TEL GEOLON", "ESO TEL GEOELEV", "GPS-LAT", "GPSLAT", "GPS LONG",
            "LATITUDE", "LONGITUD", "OBSERVER", "observer", "OBSERVAT", "Observation:Location:Latitude", "AUTHOR",
            "SITE", "LAT", "LON", "ELEVATIO", "HIERARCH OBS PI-COI NAME", "OPERATOR", "ORIGIN", "USER", "CONTACT",
            "SWOWNER", "HOSTNAME", "HOST", "COMPUTER", "LICENSE" };
         const char* kept[] = { "OBJECT", "FILTER", "EXPTIME", "DATE-OBS", "OBJCTALT", "OBJCTRA", "OBJCTDEC", "CENTALT",
            "INSTRUME", "TELESCOP", "CCD-TEMP", "SET-TEMP", "IMAGETYP", "NCOMBINE", "XBINNING", "FOCALLEN", "COMPOSIT",
            "POSITION", "AIRMASS", "PIERSIDE", "FLATFILE", "GAIN", "OFFSET" };
         nlohmann::json names = nlohmann::json::array();
         for ( const char* k : redacted )
            if ( !IsRedactedFitsKeyword( IsoString( k ) ) )
            {
               names.push_back( std::string( "not redacted: " ) + k );
               ok = false;
            }
         for ( const char* k : kept )
            if ( IsRedactedFitsKeyword( IsoString( k ) ) )
            {
               names.push_back( std::string( "over-redacted: " ) + k );
               ok = false;
            }
         // The value allowlist.
         const char* valued[] = { "OBJECT", "filter", "EXPTIME", "DATE-OBS", "GAIN", "OFFSET", "CCD-TEMP", "XBINNING", "XPIXSZ",
            "INSTRUME", "TELESCOP", "FOCALLEN", "OBJCTRA", "OBJCTDEC", "RA", "DEC", "BAYERPAT", "NAXIS1", "CTYPE2", "CRVAL1",
            "CRPIX2", "CD1_1", "CD2_2", "PC1_2", "CDELT1", "A_ORDER", "A_0_2", "BP_1_1", "EQUINOX", "NCOMBINE", "IMAGETYP" };
         const char* notValued[] = { "OBJCTALT", "OBJCTAZ", "CENTALT", "AIRMASS", "OBJCTHA", "LST", "HA", "SWCREATE", "SWOWNER",
            "FILENAME", "CREATOR", "PROGRAM", "MYKEY", "NAXISX", "CD1", "CDX_1", "A_", "CTYPE", "DATE-OBSX", "SITELAT" };
         for ( const char* k : valued )
            if ( !IsFitsValueAllowed( IsoString( k ) ) )
            {
               names.push_back( std::string( "value not allowed: " ) + k );
               ok = false;
            }
         for ( const char* k : notValued )
            if ( IsFitsValueAllowed( IsoString( k ) ) )
            {
               names.push_back( std::string( "value allowed: " ) + k );
               ok = false;
            }
         d.push_back( { { "names", names }, { "redactedChecked", sizeof( redacted )/sizeof( *redacted ) },
                        { "keptChecked", sizeof( kept )/sizeof( *kept ) } } );

         // End to end through the view context sent to the model: values only for allowlisted
         // keywords (still path-scrubbed), names only for the rest, nothing for location/identity
         // names and COMMENT/HISTORY.
         RgbWindow w( "PCFitsPaths", 0.1, 0.1, 0.1 );
         FITSKeywordArray k;
         k.Add( FITSHeaderKeyword( "OBJECT", "'/home/someone/data/M31/light_001.fits'", "" ) );
         k.Add( FITSHeaderKeyword( "TELESCOP", "'C:\\Users\\bob\\M31\\a.xisf, /mnt/x/b.xisf'", "" ) );
         k.Add( FITSHeaderKeyword( "INSTRUME", "'file:///home/someone/raw/x.fits'", "" ) );
         k.Add( FITSHeaderKeyword( "FRAME", "'/home/scott carter'", "" ) );                   // a folder with a space
         k.Add( FITSHeaderKeyword( "FILTER", "'Ha/OIII'", "" ) );
         k.Add( FITSHeaderKeyword( "EXPTIME", "300", "" ) );
         k.Add( FITSHeaderKeyword( "SWCREATE", "'N.I.N.A. 3.1 on DESKTOP-SCOTT'", "" ) );    // host name: name only
         k.Add( FITSHeaderKeyword( "FILENAME", "'/home/someone/data/M31/light_001.fits'", "" ) );
         k.Add( FITSHeaderKeyword( "MYKEY", "'secret value'", "" ) );                         // unknown: name only
         k.Add( FITSHeaderKeyword( "SWOWNER", "'Scott Carter'", "" ) );                       // SGP licence owner: left out
         k.Add( FITSHeaderKeyword( "OBSERVER", "'Someone'", "" ) );
         k.Add( FITSHeaderKeyword( "observat", "'Backyard of Someone'", "" ) );
         k.Add( FITSHeaderKeyword( "HIERARCH ESO TEL GEOLAT", "-24.6", "" ) );
         k.Add( FITSHeaderKeyword( "GPS-LAT", "'40 11 12'", "" ) );
         k.Add( FITSHeaderKeyword( "OBSGEO-X", "1234567.0", "" ) );
         k.Add( FITSHeaderKeyword( "OBJCTALT", "41.5", "" ) );                                 // locates the site with the time
         k.Add( FITSHeaderKeyword( "COMMENT", "'/home/someone/notes.txt'", "Someone's notes /home/someone" ) );
         k.Add( FITSHeaderKeyword( "HISTORY", "", "ImageIntegration.images: /home/someone/l1.xisf" ) );
         w.Window().SetKeywords( k );
         const nlohmann::json ctx = BuildViewContext( w.MainView() );
         std::map<std::string, std::string> kv;
         std::set<std::string> nameOnly;
         for ( const nlohmann::json& e : ctx["fitsKeywords"] )
         {
            if ( e.contains( "value" ) )
               kv[e.value( "name", std::string() )] = e.value( "value", std::string() );
            else
               nameOnly.insert( e.value( "name", std::string() ) );
         }
         const std::string dump = ctx.dump();
         const bool e2e = kv["OBJECT"] == "light_001.fits" && kv["TELESCOP"] == "a.xisf, b.xisf" && kv["INSTRUME"] == "x.fits"
                       && kv["FRAME"] == "[path]" && kv["FILTER"] == "Ha/OIII" && kv["EXPTIME"] == "300" && kv.size() == 6
                       && nameOnly == std::set<std::string>( { "SWCREATE", "FILENAME", "MYKEY", "OBJCTALT" } )
                       && ctx.value( "fitsKeywordsRedacted", 0 ) == 6 && ctx.value( "fitsCommentaryKeywordsOmitted", 0 ) == 2
                       && ctx.value( "fitsKeywordValuesWithheld", 0 ) == 4 && ctx.value( "fitsValuesPathsScrubbed", 0 ) == 4
                       && !Has( dump, "someone" ) && !Has( dump, "Someone" ) && !Has( dump, "Users" ) && !Has( dump, "/home/" )
                       && !Has( dump, "carter" ) && !Has( dump, "Carter" ) && !Has( dump, "DESKTOP" ) && !Has( dump, "SCOTT" )
                       && !Has( dump, "SWOWNER" ) && !Has( dump, "secret" ) && !Has( dump, "-24.6" ) && !Has( dump, "40 11 12" )
                       && !Has( dump, "41.5" );
         d.push_back( { { "endToEnd", ctx["fitsKeywords"] }, { "redacted", ctx.value( "fitsKeywordsRedacted", 0 ) },
                        { "commentary", ctx.value( "fitsCommentaryKeywordsOmitted", 0 ) },
                        { "withheld", ctx.value( "fitsKeywordValuesWithheld", 0 ) },
                        { "scrubbed", ctx.value( "fitsValuesPathsScrubbed", 0 ) }, { "ok", e2e } } );
         ok = ok && e2e;
      }
      catch ( const pcl::Exception& x ) { d.push_back( { { "error", U8( x.Message() ) } } ); ok = false; }
      catch ( const std::exception& x ) { d.push_back( { { "error", x.what() } } ); ok = false; }
      out["fitsPathScrubCases"] = d;
      out["fitsPathScrubOk"] = ok;
      allOk = allOk && ok;
   }

   out["scriptSelfTestOk"] = allOk;
   return allOk;
}

} // namespace pcl
