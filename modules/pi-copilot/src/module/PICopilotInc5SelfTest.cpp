// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentSession.h"
#include "AgentTools.h"
#include "AnthropicClient.h"
#include "ChatThread.h"
#include "PICopilotInc5SelfTest.h"
#include "PICopilotModule.h"
#include "ProcessCatalog.h"
#include "SseStream.h"
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
#include <pcl/ByteArray.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/FileFormat.h>
#include <pcl/FileFormatInstance.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>
#include <pcl/View.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pcl
{

namespace
{

// ---- shared inc5 test helpers --------------------------------------------

constexpr int kIiW = 64, kIiH = 64;

// Deterministic synthetic light frame: gradient + xorshift noise + 3 Gaussian
// "stars", so noise estimation has something real to measure.
void FillFrame( Image& img, unsigned seed )
{
   img.AllocateData( kIiW, kIiH, 1, ColorSpace::Gray );
   uint32_t s = 2463534242u ^ (seed*2654435761u);
   auto rnd = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFFu)/double( 0x1000000 ); };
   const double stars[3][2] = { { 16, 20 }, { 40, 44 }, { 50, 12 } };
   for ( int y = 0; y < kIiH; ++y )
      for ( int x = 0; x < kIiW; ++x )
      {
         double v = 0.1 + 0.05*x/(kIiW - 1) + 0.01*(rnd() - 0.5);
         for ( const auto& st : stars )
         {
            const double dx = x - st[0], dy = y - st[1];
            v += 0.5*std::exp( -(dx*dx + dy*dy)/(2*1.5*1.5) );
         }
         img.Pixel( x, y ) = float( v );
      }
}

void WriteFits( const String& path, const Image& img )
{
   FileFormat fits( ".fits", false/*toRead*/, true/*toWrite*/ );
   FileFormatInstance f( fits );
   if ( !f.Create( path ) )
      throw Error( "SyntheticFrames: cannot create " + path );
   if ( !f.WriteImage( img ) )
      throw Error( "SyntheticFrames: cannot write " + path );
   f.Close();
}

// n synthetic FITS frames in a fresh temp directory; files + directory are
// removed in the destructor (best effort: a leftover is reported, not thrown).
class SyntheticFrames
{
public:

   explicit SyntheticFrames( int n = 3 )
   {
      m_dir = File::UniqueFileName( File::SystemTempDirectory(), 12, "picopilot-inc5-" );
      if ( !m_dir.StartsWith( '/' ) )   // a bare name: anchor it in the temp directory
         m_dir = File::SystemTempDirectory() + '/' + m_dir;
      File::CreateDirectory( m_dir );
      double sum = 0;
      for ( int i = 0; i < n; ++i )
      {
         Image img;
         FillFrame( img, unsigned( i + 1 ) );
         sum += img.Mean();
         const String path = m_dir + String().Format( "/light_%02d.fits", i + 1 );
         WriteFits( path, img );
         m_paths << path;
      }
      m_meanOfMeans = n > 0 ? sum/n : 0;
   }

   ~SyntheticFrames()
   {
      try
      {
         for ( const String& p : m_extra )
            if ( File::Exists( p ) )
               File::Remove( p );
         for ( const String& p : m_paths )
            if ( File::Exists( p ) )
               File::Remove( p );
         if ( File::DirectoryExists( m_dir ) )
            File::RemoveDirectory( m_dir );
      }
      catch ( ... )
      {
      }
   }

   SyntheticFrames( const SyntheticFrames& ) = delete;
   SyntheticFrames& operator =( const SyntheticFrames& ) = delete;

   const StringList& Paths() const { return m_paths; }
   const String& Dir() const { return m_dir; }
   double MeanOfMeans() const { return m_meanOfMeans; }

   // Another file in the directory (e.g. a non-image), removed with it.
   String AddFile( const String& name, const IsoString& contents )
   {
      const String p = m_dir + '/' + name;
      File::WriteFile( p, contents.Begin(), contents.Length() );
      m_extra << p;
      return p;
   }

private:

   String     m_dir;
   StringList m_paths;
   StringList m_extra;
   double     m_meanOfMeans = 0;
};

std::set<std::string> OpenMainViewIds()
{
   std::set<std::string> ids;
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
      ids.insert( std::string( w.MainView().Id().c_str() ) );
   return ids;
}

void ForceCloseWindows( const std::vector<std::string>& ids )
{
   for ( const std::string& id : ids )
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

String EvalJs( const String& src )
{
   return ThePICopilotModule->EvaluateScript( src, "JavaScript" ).ToString();
}

// Hidden float window filled with one constant; force-closed on destruction.
class Inc5TestWindow
{
public:

   Inc5TestWindow( const char* id, int w, int h, int channels, double value )
      : m_window( w, h, channels, 32, true/*float*/, channels >= 3/*color*/, false, IsoString( id ) )
   {
      if ( m_window.IsNull() )
         throw Error( "Inc5TestWindow: null window" );
      View v = m_window.MainView();
      AutoViewLock lock( v );
      ImageVariant iv = v.Image();
      static_cast<Image&>( *iv ).Fill( float( value ) );
   }

   ~Inc5TestWindow()
   {
      try { if ( !m_window.IsNull() ) m_window.ForceClose(); } catch ( ... ) {}
   }

   Inc5TestWindow( const Inc5TestWindow& ) = delete;
   Inc5TestWindow& operator =( const Inc5TestWindow& ) = delete;

   View MainView() const { return m_window.MainView(); }

private:

   ImageWindow m_window;
};

// [[maybe_unused]]: first used by Section B6 (Task 7).
[[maybe_unused]] double Inc5Median( View v, int channel )
{
   if ( !v.CanRead() || !v.CanWrite() )
      throw Error( "Inc5Median: view is busy" );
   AutoViewWriteLock lock( v );
   ImageVariant iv = v.Image();
   return iv.Median( iv.Bounds(), channel, channel );
}

// Sets an enumeration parameter by element id (via the same EnumerationInfoOf
// the tools use). Records a miss in info and returns false.
bool SetEnumById( ProcessInstance& instance, const Process& P, const char* param, const char* id,
                  nlohmann::json& info )
{
   const ProcessParameter p( P, IsoString( param ) );
   for ( const ProcessParameter::EnumerationElement& e : EnumerationInfoOf( p ).elements )
      if ( e.id == id )
         return instance.SetParameterValue( Variant( e.value ), p, 0 ) && instance.ParameterValue( p, 0 ).ToInt() == e.value;
   info[std::string( "missingEnum_" ) + param] = id;
   return false;
}

std::string SseEv( const char* name, const std::string& data )
{
   return std::string( "event: " ) + name + "\ndata: " + data + "\n\n";
}

struct Assembled
{
   nlohmann::json message;
   std::string    text;
   bool           finished = false;
   bool           failed = false;
   std::string    error;
   std::string    errorType;
};

Assembled AssembleInChunks( const std::string& s, size_t chunk )
{
   SseMessageAssembler a;
   Assembled r;
   for ( size_t i = 0; i < s.size(); i += chunk )
      r.text += a.Feed( s.data() + i, std::min( chunk, s.size() - i ) );
   r.finished = a.Finished();
   r.failed = a.Failed();
   r.error = a.Error();
   r.errorType = a.ErrorType();
   if ( r.finished )
      r.message = a.FinalMessage();
   return r;
}

std::string MessageStart( const char* id )
{
   return SseEv( "message_start", std::string( "{\"type\":\"message_start\",\"message\":{\"id\":\"" ) + id
      + "\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"m\",\"content\":[],\"stop_reason\":null,"
        "\"stop_sequence\":null,\"usage\":{\"input_tokens\":12,\"cache_read_input_tokens\":0,\"output_tokens\":1}}}" );
}

std::string BlockStart( int index, const std::string& block )
{
   return SseEv( "content_block_start", "{\"type\":\"content_block_start\",\"index\":" + std::to_string( index )
                                        + ",\"content_block\":" + block + "}" );
}

std::string Delta( int index, const std::string& delta )
{
   return SseEv( "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":" + std::to_string( index )
                                        + ",\"delta\":" + delta + "}" );
}

std::string BlockStop( int index )
{
   return SseEv( "content_block_stop", "{\"type\":\"content_block_stop\",\"index\":" + std::to_string( index ) + "}" );
}

std::string MessageEnd( const char* stopReason, int outputTokens )
{
   return SseEv( "message_delta", std::string( "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" ) + stopReason
                 + "\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":" + std::to_string( outputTokens ) + "}}" )
        + SseEv( "message_stop", "{\"type\":\"message_stop\"}" );
}

// The synthetic tool_use stream (S1) without its final message_stop tail.
std::string SyntheticToolStreamBody()
{
   return std::string( ": keep-alive comment\n\n" )
        + MessageStart( "msg_t1" )
        + "event: ping\ndata: {\"type\":\n" + "data: \"ping\"}\n\n"            // one event, two data lines
        + BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" )
        + Delta( 0, "{\"type\":\"text_delta\",\"text\":\"Caf\"}" )
        + Delta( 0, "{\"type\":\"text_delta\",\"text\":\"\xC3\xA9 \xF0\x9F\x93\xB7\"}" )   // raw UTF-8, split by small chunks
        + BlockStop( 0 )
        + SseEv( "some_future_event", "{\"type\":\"some_future_event\",\"x\":1}" )
        + BlockStart( 1, "{\"type\":\"tool_use\",\"id\":\"toolu_t1\",\"name\":\"describe_process\",\"input\":{}}" )
        + Delta( 1, "{\"type\":\"input_json_delta\",\"partial_json\":\"\"}" )
        + Delta( 1, "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"id\\\": \\\"Pix\"}" )
        + Delta( 1, "{\"type\":\"input_json_delta\",\"partial_json\":\"elMath\\\"}\"}" )
        + BlockStop( 1 );
}

std::string ReadFixture( const char* name )
{
   const char* dir = std::getenv( "PICOPILOT_SELFTEST_FIXTURES" );
   if ( dir == nullptr )
      throw Error( "PICOPILOT_SELFTEST_FIXTURES is not set" );
   const ByteArray b = File::ReadFile( String( dir ) + "/sse/" + name );
   return std::string( reinterpret_cast<const char*>( b.Begin() ), b.Length() );
}

} // namespace

bool RunInc5SelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section B0: platform smoke (Task 1) --------------------------------
   {
      bool iiOk = false, parseNoExecOk = false, breakoutOk = false, syntaxLineOk = false,
           runtimeLineOk = false, consoleOk = false, throwOk = false;
      nlohmann::json info = nlohmann::json::object();
      std::vector<std::string> created;
      String error;
      try
      {
         // (a) ImageIntegration in the global context over 3 synthetic FITS frames.
         SyntheticFrames frames( 3 );
         Process II( IsoString( "ImageIntegration" ) );
         const ProcessParameter images( II, IsoString( "images" ) );
         const ProcessParameter::parameter_list cols = images.TableColumns();
         nlohmann::json colIds = nlohmann::json::array();
         for ( const ProcessParameter& c : cols )
            colIds.push_back( std::string( c.Id().c_str() ) );
         info["iiImagesColumns"] = colIds;
         if ( cols.Length() != 4 )
            throw Error( String().Format( "ImageIntegration.images has %u columns, expected 4", unsigned( cols.Length() ) ) );
         for ( const char* e : { "weightMode", "rejection", "combination", "normalization" } )
         {
            // Named lvalue (not a temporary bound at the call site): GCC's
            // -Wdangling-reference cannot see that EnumerationInfoOf()
            // returns a reference into its own static cache, not into the
            // ProcessParameter argument, and warns on the temporary form.
            const ProcessParameter param( II, IsoString( e ) );
            const EnumerationInfo& ei = EnumerationInfoOf( param );
            nlohmann::json ids = nlohmann::json::array();
            for ( const ProcessParameter::EnumerationElement& el : ei.elements )
               ids.push_back( { { "id", std::string( el.id.c_str() ) }, { "value", el.value } } );
            info[std::string( "iiEnum_" ) + e] = { { "elements", ids }, { "default", std::string( ei.defaultId.c_str() ) } };
         }
         for ( const char* b : { "generateDrizzleData", "closePreviousImages", "generateRejectionMaps", "generateIntegratedImage" } )
            try
            {
               ProcessInstance d( II );
               info[std::string( "iiDefault_" ) + b] = d.ParameterValue( ProcessParameter( II, IsoString( b ) ), 0 ).ToBoolean();
            }
            catch ( ... )
            {
               info[std::string( "iiDefault_" ) + b] = "absent";
            }

         ProcessInstance ii( II );
         bool setOk = ii.AllocateTableRows( images, frames.Paths().Length() );
         for ( size_type r = 0; r < frames.Paths().Length(); ++r )
            setOk = setOk && ii.SetParameterValue( Variant( true ), cols[0], r )
                          && ii.SetParameterValue( Variant( frames.Paths()[r] ), cols[1], r )
                          && ii.SetParameterValue( Variant( String() ), cols[2], r )
                          && ii.SetParameterValue( Variant( String() ), cols[3], r );
         // Equal weights: PSF-signal weighting (the PI 1.9 default) is not what this smoke tests.
         setOk = setOk && SetEnumById( ii, II, "weightMode", "DontCare", info );
         info["iiSetOk"] = setOk;

         String whyNot;
         const bool valid = ii.Validate( whyNot );
         info["iiValidate"] = valid; info["iiValidateWhyNot"] = U8( whyNot );
         whyNot.Clear();
         const bool can = ii.CanExecuteGlobal( whyNot );
         info["iiCanExecuteGlobal"] = can; info["iiCanWhyNot"] = U8( whyNot );

         const std::set<std::string> before = OpenMainViewIds();
         const auto t0 = std::chrono::steady_clock::now();
         const bool ran = setOk && valid && can && ii.ExecuteGlobal();
         info["iiRan"] = ran;
         info["iiSeconds"] = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
         for ( const std::string& id : OpenMainViewIds() )
            if ( before.count( id ) == 0 )
               created.push_back( id );
         info["iiCreatedWindows"] = created;

         nlohmann::json outIds = nlohmann::json::object();
         for ( const char* o : { "integrationImageId", "lowRejectionMapImageId", "highRejectionMapImageId", "slopeMapImageId" } )
            try
            {
               outIds[o] = U8( ii.ParameterValue( ProcessParameter( II, IsoString( o ) ), 0 ).ToString() );
            }
            catch ( ... )
            {
               outIds[o] = "absent";
            }
         info["iiOutputIds"] = outIds;

         const std::string integ = outIds.value( "integrationImageId", std::string() );
         ImageWindow w = integ.empty() ? ImageWindow::Null() : ImageWindow::WindowById( IsoString( integ.c_str() ) );
         if ( !w.IsNull() )
         {
            View v = w.MainView();
            AutoViewWriteLock lock( v );
            ImageVariant iv = v.Image();
            info["iiResultGeometry"] = { iv.Width(), iv.Height(), iv.NumberOfChannels() };
            const double mean = iv.Mean();
            info["iiResultMean"] = mean;
            info["iiInputMeanOfMeans"] = frames.MeanOfMeans();
            iiOk = ran && iv.Width() == kIiW && iv.Height() == kIiH && iv.NumberOfChannels() == 1
                && std::fabs( mean - frames.MeanOfMeans() ) < 0.01*frames.MeanOfMeans()
                && std::find( created.begin(), created.end(), integ ) != created.end();
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      ForceCloseWindows( created );

      // (b) PJSR: parse without executing; breakout stays a SyntaxError; line numbers.
      try
      {
         const String parsed = EvalJs(
            "(function(){ var m = { v: 0 }; var f;"
            " try { f = new Function( \"m\", \"m.v = 1;\" ); } catch ( e ) { return \"error: \" + e; }"
            " var before = m.v; f( m ); return \"parsed:\" + before + \":\" + m.v; })()" );
         info["pjsrParse"] = U8( parsed );
         parseNoExecOk = parsed == "parsed:0:1";   // not run by parsing; runs when called

         const String breakout = EvalJs(
            "(function(){ var m = { v: 0 };"
            " try { new Function( \"m\", \"}); m.v = 2; (function(){\" ); return \"parsed:\" + m.v; }"
            " catch ( e ) { return \"syntax:\" + m.v + \":\" + (e instanceof SyntaxError); } })()" );
         info["pjsrBreakout"] = U8( breakout );
         breakoutOk = breakout == "syntax:0:true";

         // FINDING: under MetaModule::EvaluateScript(), a caught Error's own
         // properties are ONLY {message, stack} -- no lineNumber, columnNumber
         // or fileName (confirmed via Object.getOwnPropertyNames(e); "in e"
         // is false too, so it isn't an inherited-but-shadowed accessor
         // either). This is a different engine/mode than the core's own
         // script runner (a plain -r= script DOES expose e.lineNumber), and
         // its error TEXT differs too ("Unexpected token ';'" / "Cannot set
         // properties of null (setting 'x')" here vs "syntax error" / "null
         // has no properties" from a plain -r= script on the same platform).
         // For a SyntaxError thrown while COMPILING a new Function() body,
         // e.stack carries no body-relative location either: its frames are
         // "at new Function (<anonymous>)" (no line:col -- parsing never
         // reached a frame) and two "at unnamed:1:N" frames that are just the
         // new Function(...) CALL SITE and the END of the one-line wrapper
         // script -- both constant regardless of where inside the body the
         // syntax error actually is. Ruling #2 (global-constraints.md) cannot
         // get a body-relative line number for a rejected script this way.
         const String syn = EvalJs(
            "(function(){ try { new Function( \"targetViewId\", \"var a = 1;\\nvar b = 2;\\nvar c = ;\\n\" ); return \"parsed\"; }"
            " catch ( e ) { return JSON.stringify( { name: e.name, message: String( e.message ),"
            " hasLineNumber: (\"lineNumber\" in e) && typeof e.lineNumber === \"number\","
            " ownProps: Object.getOwnPropertyNames( e ), stack: String( e.stack ) } ); } })()" );
         info["pjsrSyntaxForBodyLine3"] = U8( syn );
         const nlohmann::json sj = nlohmann::json::parse( U8( syn ) );
         const bool syntaxNoLineNumberConfirmed = sj.value( "name", std::string() ) == "SyntaxError"
                                                && sj.value( "hasLineNumber", true ) == false;
         syntaxLineOk = syntaxNoLineNumberConfirmed;   // the FACT itself, not a numeric line (see finding above)

         // FINDING (positive): unlike a compile-time SyntaxError, a RUNTIME
         // error thrown by CALLING the compiled function DOES get a usable
         // inner frame: "at eval (eval at <anonymous> (unnamed:L:C),
         // <anonymous>:LINE:COL)". <anonymous>:LINE:COL is relative to the
         // engine's synthesized source "function anonymous(args\n) {\n<body>\n}"
         // -- a 2-line header before the body's own line 1 -- so
         // LINE - 2 recovers the 1-based body line. Verified on two separate
         // bodies (a 1-line body throwing at its own line 1 reports inner
         // LINE 3; this 2-line body throwing at its own line 2 reports inner
         // LINE 4): the header offset is a stable +2 in both cases.
         const String run = EvalJs(
            "(function(){ try { var f = new Function( \"targetViewId\", \"var a = 1;\\nnull.x = 2;\\n\" ); f( \"\" ); return \"ran\"; }"
            " catch ( e ) { var innerLine = null, innerCol = null;"
            " var lines = String( e.stack ).split( \"\\n\" );"
            " for ( var i = 0; i < lines.length; ++i ) {"
            " var mm = /<anonymous>:(\\d+):(\\d+)/.exec( lines[i] );"
            " if ( mm ) { innerLine = parseInt( mm[1], 10 ); innerCol = parseInt( mm[2], 10 ); break; } }"
            " return JSON.stringify( { name: e.name, message: String( e.message ),"
            " hasLineNumber: (\"lineNumber\" in e) && typeof e.lineNumber === \"number\","
            " innerLine: innerLine, innerCol: innerCol, bodyLine: innerLine === null ? null : innerLine - 2,"
            " stack: String( e.stack ) } ); } })()" );
         info["pjsrRuntimeForBodyLine2"] = U8( run );
         const nlohmann::json rj = nlohmann::json::parse( U8( run ) );
         // Body line 2 ("null.x = 2;") is the second statement of this body -> bodyLine must read back as 2.
         runtimeLineOk = rj.value( "name", std::string() ) == "TypeError" && rj.value( "bodyLine", -1 ) == 2;

         info["pjsrLineNumberFinding"] =
            "MetaModule::EvaluateScript() Error objects have no lineNumber/columnNumber/fileName "
            "(only message+stack), unlike the core's own -r= script engine. A new Function() "
            "SyntaxError (compile-time) carries NO body-relative location anywhere (own props or "
            "stack). A runtime error thrown by CALLING the compiled function DOES: e.stack's "
            "\"<anonymous>:LINE:COL\" frame, minus a constant 2-line synthesized-header offset, "
            "gives the 1-based body line (confirmed on two different body lengths).";
      }
      catch ( const pcl::Exception& x ) { info["pjsrError"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { info["pjsrError"] = x.what(); }

      // (c) console capture inside ONE EvaluateScript; (d) what an uncaught throw looks like.
      try
      {
         // console.endLog() returns a (JS) ByteArray of the raw log bytes,
         // already correct UTF-8 (confirmed separately: a script that writes
         // b.toString() straight to a file via the core's own File API gets
         // "pc-smoke-é" back correctly, i.e. "pc-smoke-é"). But a bare
         // JS-string return value of ByteArray.toString() gets DOUBLE
         // UTF-8-encoded somewhere in the EvaluateScript()/Variant marshaling
         // boundary for any character in the Latin-1 range (<=U+00FF):
         // "pc-smoke-é" comes back here as "pc-smoke-Ã©" once decoded by U8().
         // A plain JS string LITERAL with the same character round-trips
         // through that same boundary correctly, so the corruption is
         // specific to strings originating from ByteArray.toString() (very
         // likely SpiderMonkey's Latin1-narrow string storage colliding with
         // the core's Value->pcl::String extraction). Fact for Tasks 8/9
         // (run_pjsr console capture): never return ByteArray.toString()
         // directly across this boundary -- round-trip it through
         // ByteArray.toBase64() (pure ASCII, immune to the narrow-string
         // issue) and decode with IsoString::FromBase64() in C++ instead.
         const String log = EvalJs(
            "(function(){ var out = {}; console.beginLog();"
            " try { console.writeln( \"pc-smoke-line-1\" ); console.writeln( \"pc-smoke-\\u00e9\" ); }"
            " finally { var b = console.endLog(); out.type = typeof b; out.b64 = b.toBase64(); }"
            " return JSON.stringify( out ); })()" );
         info["consoleCapture"] = U8( log );
         const IsoString b64( nlohmann::json::parse( U8( log ) ).value( "b64", std::string() ).c_str() );
         const ByteArray raw = b64.FromBase64();
         const std::string text( reinterpret_cast<const char*>( raw.Begin() ), raw.Length() );
         info["consoleCaptureText"] = text;
         consoleOk = text.find( "pc-smoke-line-1" ) != std::string::npos
                  && text.find( "pc-smoke-\xC3\xA9" ) != std::string::npos;
      }
      catch ( const pcl::Exception& x ) { info["consoleError"] = U8( x.Message() ); }
      catch ( const std::exception& x ) { info["consoleError"] = x.what(); }
      try
      {
         EvalJs( "throw new Error( \"pc-boom\" );" );
         info["evalThrow"] = "no exception";
      }
      catch ( const pcl::Exception& x )
      {
         info["evalThrow"] = U8( x.Message() );
         throwOk = x.Message().Contains( "pc-boom" );
      }

      const bool ok = iiOk && parseNoExecOk && breakoutOk && syntaxLineOk && runtimeLineOk && consoleOk;
      out["inc5SmokeInfo"] = info;
      out["inc5SmokeIiOk"] = iiOk;
      out["inc5SmokePjsrOk"] = parseNoExecOk && breakoutOk && syntaxLineOk && runtimeLineOk;
      out["inc5SmokeConsoleOk"] = consoleOk;
      out["inc5SmokeEvalThrowNamesMessage"] = throwOk;   // informational (RunPjsr catches inside JS anyway)
      out["inc5SmokeError"] = U8( error );
      out["inc5SmokeOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B1: SSE parser + assembler (Task 2) --------------------------
   {
      bool s1Ok = true, crlfOk = false, errorOk = false, unknownDeltaOk = false, truncOk = false,
           thinkingOk = false, badJsonOk = false, orderOk = false, fixturesOk = true,
           bareCrOk = false, redactedOk = false, truncatedBadJsonOk = false,
           dupStartOk = false, openBlockOk = false, lineBoundOk = false, dataBoundOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         const std::string s1 = SyntheticToolStreamBody() + MessageEnd( "tool_use", 40 );
         const nlohmann::json expected = nlohmann::json::parse(
            "{\"id\":\"msg_t1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"m\","
            "\"content\":[{\"type\":\"text\",\"text\":\"Caf\xC3\xA9 \xF0\x9F\x93\xB7\"},"
            "{\"type\":\"tool_use\",\"id\":\"toolu_t1\",\"name\":\"describe_process\",\"input\":{\"id\":\"PixelMath\"}}],"
            "\"stop_reason\":\"tool_use\",\"stop_sequence\":null,"
            "\"usage\":{\"input_tokens\":12,\"cache_read_input_tokens\":0,\"output_tokens\":40}}" );
         for ( size_t chunk : { size_t( 1 ), size_t( 2 ), size_t( 7 ), size_t( 64 ), s1.size() } )
         {
            const Assembled a = AssembleInChunks( s1, chunk );
            const bool pass = a.finished && !a.failed && a.message == expected && a.text == "Caf\xC3\xA9 \xF0\x9F\x93\xB7";
            if ( !pass )
               detail["s1Fail_" + std::to_string( chunk )] = { { "message", a.message }, { "text", a.text }, { "error", a.error } };
            s1Ok = s1Ok && pass;
         }
         {  // The rebuilt body parses exactly like a non-streamed reply.
            const Assembled a = AssembleInChunks( s1, 5 );
            const AnthropicResult r = ParseMessagesResponse( 200, IsoString( a.message.dump().c_str() ), String() );
            s1Ok = s1Ok && r.ok && r.stopReason == "tool_use" && r.contentBlocks.size() == 2
                && r.contentBlocks[1].at( "input" ) == nlohmann::json( { { "id", "PixelMath" } } );
         }
         {  // CRLF line endings, split anywhere -- including one byte at a time,
            // so a "\r\n" pair split exactly between Feed() calls is covered.
            std::string crlf;
            for ( char c : s1 )
               crlf += (c == '\n') ? std::string( "\r\n" ) : std::string( 1, c );
            crlfOk = true;
            for ( size_t chunk : { size_t( 1 ), size_t( 3 ), crlf.size() } )
            {
               const Assembled a = AssembleInChunks( crlf, chunk );
               const bool pass = a.finished && !a.failed && a.message == expected;
               if ( !pass )
                  detail["crlfFail_" + std::to_string( chunk )] = { { "message", a.message }, { "error", a.error } };
               crlfOk = crlfOk && pass;
            }
         }
         {  // Bare CR (no LF) line endings -- classic Mac style -- split anywhere.
            // Two consecutive bare CRs (from an original "\n\n" blank line) must
            // still be read as two line terminators (an empty line dispatches).
            std::string cr;
            for ( char c : s1 )
               cr += (c == '\n') ? '\r' : c;
            bareCrOk = true;
            for ( size_t chunk : { size_t( 1 ), size_t( 5 ), cr.size() } )
            {
               const Assembled a = AssembleInChunks( cr, chunk );
               const bool pass = a.finished && !a.failed && a.message == expected;
               if ( !pass )
                  detail["bareCrFail_" + std::to_string( chunk )] = { { "message", a.message }, { "error", a.error } };
               bareCrOk = bareCrOk && pass;
            }
         }
         {  // A redacted_thinking block (opaque, no deltas -- delivered whole in
            // content_block_start) is carried through verbatim, at any chunk size.
            const nlohmann::json redacted = nlohmann::json::parse(
               "{\"type\":\"redacted_thinking\",\"data\":\"EmVhbXBsZS1vcGFxdWUtZW5jcnlwdGVkLWRhdGE=\"}" );
            const std::string s = MessageStart( "msg_r" ) + BlockStart( 0, redacted.dump() ) + BlockStop( 0 )
               + BlockStart( 1, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 1, "{\"type\":\"text_delta\",\"text\":\"ok\"}" ) + BlockStop( 1 )
               + MessageEnd( "end_turn", 4 );
            redactedOk = true;
            for ( size_t chunk : { size_t( 1 ), size_t( 6 ), s.size() } )
            {
               const Assembled a = AssembleInChunks( s, chunk );
               const bool pass = a.finished && !a.failed && a.message["content"].at( 0 ) == redacted
                              && a.text == "ok";
               if ( !pass )
                  detail["redactedFail_" + std::to_string( chunk )] = { { "message", a.message }, { "error", a.error } };
               redactedOk = redactedOk && pass;
            }
         }
         {  // API "error" event mid-stream.
            const std::string s = MessageStart( "msg_e" ) + BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 0, "{\"type\":\"text_delta\",\"text\":\"Partial\"}" )
               + SseEv( "error", "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}" );
            const Assembled a = AssembleInChunks( s, 4 );
            detail["errorCase"] = { { "error", a.error }, { "text", a.text } };
            errorOk = a.failed && !a.finished && a.error == "overloaded_error: Overloaded"
                   && a.errorType == "overloaded_error" && a.text == "Partial";
         }
         {  // An unknown DELTA type fails loudly (the block could not be echoed back correctly).
            const std::string s = MessageStart( "msg_u" ) + BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 0, "{\"type\":\"mystery_delta\",\"x\":1}" ) + BlockStop( 0 ) + MessageEnd( "end_turn", 3 );
            const Assembled a = AssembleInChunks( s, 9 );
            detail["unknownDelta"] = a.error;
            unknownDeltaOk = a.failed && a.error == "unsupported stream delta type 'mystery_delta' (content[0])";
         }
         {  // Cut before message_stop: neither finished nor failed (the transport reports it).
            const std::string s = SyntheticToolStreamBody();
            const Assembled a = AssembleInChunks( s, 11 );
            truncOk = !a.finished && !a.failed;
         }
         {  // Thinking block: thinking_delta + signature_delta rebuilt verbatim.
            const std::string s = MessageStart( "msg_th" )
               + BlockStart( 0, "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}" )
               + Delta( 0, "{\"type\":\"thinking_delta\",\"thinking\":\"Let me\"}" )
               + Delta( 0, "{\"type\":\"thinking_delta\",\"thinking\":\" think\"}" )
               + Delta( 0, "{\"type\":\"signature_delta\",\"signature\":\"c2ln\"}" )
               + BlockStop( 0 )
               + BlockStart( 1, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 1, "{\"type\":\"text_delta\",\"text\":\"Done.\"}" ) + BlockStop( 1 )
               + MessageEnd( "end_turn", 9 );
            const Assembled a = AssembleInChunks( s, 6 );
            const AnthropicResult r = a.finished
               ? ParseMessagesResponse( 200, IsoString( a.message.dump().c_str() ), String() ) : AnthropicResult();
            thinkingOk = a.finished && r.ok && r.text == "Done." && a.text == "Done."
                      && r.contentBlocks.at( 0 ) == nlohmann::json::parse(
                            "{\"type\":\"thinking\",\"thinking\":\"Let me think\",\"signature\":\"c2ln\"}" );
         }
         {  // Invalid tool input JSON under stop_reason tool_use.
            const std::string s = MessageStart( "msg_b" )
               + BlockStart( 0, "{\"type\":\"tool_use\",\"id\":\"toolu_b\",\"name\":\"describe_process\",\"input\":{}}" )
               + Delta( 0, "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"id\\\": \"}" ) + BlockStop( 0 )
               + MessageEnd( "tool_use", 5 );
            const Assembled a = AssembleInChunks( s, 8 );
            detail["badJson"] = a.error;
            badJsonOk = a.failed && a.error == "tool_use input (content[0]) was not valid JSON";
         }
         {  // Protocol order.
            const Assembled a = AssembleInChunks( BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" ), 100 );
            orderOk = a.failed && a.error == "content_block_start before message_start";
         }
         {  // Bad tool-input JSON under a NON-tool_use stop_reason (e.g. a
            // max_tokens cut mid tool call): the stream still finishes -- the
            // caller's own truncation handling is what must drop the block --
            // but "input" is left as the raw partial text, never a fabricated
            // {} that would read back as a legitimate empty call.
            const std::string s = MessageStart( "msg_mt" )
               + BlockStart( 0, "{\"type\":\"tool_use\",\"id\":\"toolu_mt\",\"name\":\"describe_process\",\"input\":{}}" )
               + Delta( 0, "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"id\\\": \\\"Pix\"}" ) + BlockStop( 0 )
               + MessageEnd( "max_tokens", 200 );
            const Assembled a = AssembleInChunks( s, 7 );
            detail["truncatedBadJson"] = { { "finished", a.finished }, { "failed", a.failed }, { "message", a.message } };
            truncatedBadJsonOk = a.finished && !a.failed
               && a.message["content"].at( 0 ).at( "input" ).is_string()
               && a.message["content"].at( 0 ).at( "input" ).get<std::string>() == "{\"id\": \"Pix";
         }
         {  // A second message_start before message_stop is a protocol violation.
            const std::string s = MessageStart( "msg_d1" ) + MessageStart( "msg_d2" );
            const Assembled a = AssembleInChunks( s, 5 );
            detail["dupMessageStart"] = a.error;
            dupStartOk = a.failed && !a.finished
               && a.error == "duplicate message_start (message_stop was not seen for the previous message)";
         }
         {  // message_stop while a content block is still open (no
            // content_block_stop) is a protocol violation, not a silent finish.
            const std::string s = MessageStart( "msg_o" ) + BlockStart( 0, "{\"type\":\"text\",\"text\":\"\"}" )
               + Delta( 0, "{\"type\":\"text_delta\",\"text\":\"hi\"}" ) + MessageEnd( "end_turn", 3 );
            const Assembled a = AssembleInChunks( s, 5 );
            detail["openBlockAtStop"] = a.error;
            openBlockOk = a.failed && !a.finished
               && a.error == "message_stop while a content block is still open (no content_block_stop)";
         }
         {  // An SSE line with no terminator past the buffer bound (4 MiB) is a
            // bounded, precise failure -- not unbounded growth from a hostile
            // or broken stream.
            const std::string huge( 4u*1024u*1024u + 16u, 'x' );   // no '\n' anywhere: one giant "line"
            SseMessageAssembler a;
            a.Feed( huge.data(), huge.size() );
            detail["lineBound"] = { { "failed", a.Failed() }, { "error", a.Error() } };
            lineBoundOk = a.Failed() && !a.Finished()
               && a.Error() == "SSE line exceeded 4194304 bytes without a line terminator";
         }
         {  // Two separate "data:" lines in one event, each individually under
            // the per-line bound, whose CUMULATIVE payload exceeds it.
            const std::string big1( 4u*1024u*1024u - 32u, 'y' );
            const std::string s = "event: message_start\ndata: " + big1 + "\ndata: " + std::string( 64, 'z' ) + "\n\n";
            SseMessageAssembler a;
            a.Feed( s.data(), s.size() );
            detail["dataBound"] = { { "failed", a.Failed() }, { "error", a.Error() } };
            dataBoundOk = a.Failed() && !a.Finished() && a.Error() == "SSE event data exceeded 4194304 bytes";
         }

         // Recorded real streams: chunking-invariant, complete, and parse like non-streamed replies.
         // text-opus-4-8.sse's prompt asked for an exact echo ("café ok — done"),
         // so its assembled text is checked byte-for-byte (incl. the two
         // multibyte characters), not just "non-empty".
         struct Fixture { const char* file; const char* stop; const char* text; };
         const Fixture fixtures[] = { { "text-opus-4-8.sse", "end_turn", "caf\xC3\xA9 ok \xE2\x80\x94 done" },
                                      { "tool-opus-4-8.sse", "tool_use", nullptr },
                                      { "thinking-tool-opus-5-5.sse", "tool_use", nullptr } };
         for ( const Fixture& f : fixtures )
         {
            const std::string bytes = ReadFixture( f.file );
            const Assembled one = AssembleInChunks( bytes, 1 ), big = AssembleInChunks( bytes, 4096 );
            const AnthropicResult r = big.finished
               ? ParseMessagesResponse( 200, IsoString( big.message.dump().c_str() ), String() ) : AnthropicResult();
            int toolUses = 0, thinking = 0;
            bool signaturesOk = true;
            for ( const nlohmann::json& b : r.contentBlocks )
            {
               const std::string t = b.value( "type", std::string() );
               if ( t == "tool_use" && b.value( "name", std::string() ) == "describe_process"
                    && b.at( "input" ).is_object() && b.at( "input" ).value( "id", nlohmann::json() ).is_string() )
                  ++toolUses;
               if ( t == "thinking" )
               {
                  ++thinking;
                  signaturesOk = signaturesOk && !b.value( "signature", std::string() ).empty();
               }
            }
            const bool textExactOk = f.text == nullptr || (one.text == f.text && big.text == f.text);
            const bool pass = one.finished && big.finished && !one.failed && one.message == big.message
                           && one.text == big.text && r.ok && r.stopReason == f.stop
                           && (std::string( f.stop ) != "tool_use" || toolUses == 1)
                           && (std::string( f.stop ) != "end_turn" || !r.text.IsEmpty())
                           && signaturesOk && textExactOk;
            detail[f.file] = { { "pass", pass }, { "stop", r.stopReason }, { "toolUses", toolUses },
                               { "thinkingBlocks", thinking }, { "error", big.error }, { "text", big.text } };
            fixturesOk = fixturesOk && pass;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); fixturesOk = false; }
      catch ( const std::exception& x ) { error = String( x.what() ); fixturesOk = false; }

      const bool ok = s1Ok && crlfOk && errorOk && unknownDeltaOk && truncOk && thinkingOk && badJsonOk && orderOk
                   && fixturesOk && bareCrOk && redactedOk && truncatedBadJsonOk && dupStartOk && openBlockOk
                   && lineBoundOk && dataBoundOk;
      out["sseDetail"] = detail;
      out["sseError"] = U8( error );
      out["sseParserOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B2: streaming transport (Task 3) ------------------------------
   {
      using clock = std::chrono::steady_clock;
      auto secondsSince = []( clock::time_point t0 ) { return std::chrono::duration<double>( clock::now() - t0 ).count(); };
      bool bodyOk = false, liveDeltasOk = false, loopOk = false, errorOk = false, stallOk = false, cancelOk = false;
      bool truncatedOk = false, httpErrorOk = false, http401Ok = false, failFastOk = false;
      bool skipped = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         Array<AnthropicMessage> hist;
         AnthropicMessage hi;
         hi.role = "user";
         hi.content = "hi";
         hist.Add( hi );
         const nlohmann::json streamed = nlohmann::json::parse(
            BuildMessagesRequestBody( "m", "sys", hist, nlohmann::json(), ProductionRequestShape( PICOPILOT_DEFAULT_MODEL ) ) );
         const nlohmann::json plain = nlohmann::json::parse( BuildMessagesRequestBody( "m", "sys", hist ) );
         bodyOk = streamed.value( "stream", false ) && streamed.at( "max_tokens" ) == PICopilotStreamMaxTokens
               && !plain.contains( "stream" ) && plain.at( "max_tokens" ) == 4096;

         if ( const char* base = std::getenv( "PICOPILOT_SELFTEST_STREAM_BASE" ) )
         {
            skipped = false;
            RequestShape shape;
            shape.stream = true;
            shape.maxTokens = 1000;
            shape.streamIdleSeconds = 5;

            // (1) Deltas reach this (UI) thread while the request is still running.
            AgentSession session;
            AnthropicMessage u;
            u.role = "user";
            u.content = "stream please";
            session.BeginUserTurn( u );
            AnthropicResult r;
            std::string shown;
            int takesWhileActive = 0;
            double firstDelta = -1, total = 0;
            {
               ChatThread t( "sk-ant-invalid-selftest", "sys", session.History(), PICOPILOT_DEFAULT_MODEL,
                             String( base ) + "/stream", 30, ToolDefinitions( AgentMode::Advisor ), shape );
               const clock::time_point t0 = clock::now();
               t.Start();
               while ( t.IsActive() )
               {
                  const String d = t.TakeStreamedText();
                  if ( !d.IsEmpty() )
                  {
                     ++takesWhileActive;
                     shown += U8( d );
                     if ( firstDelta < 0 )
                        firstDelta = secondsSince( t0 );
                  }
                  ThePICopilotModule->ProcessEvents( true );
                  std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
               }
               t.Wait();
               total = secondsSince( t0 );
               shown += U8( t.TakeStreamedText() );
               t.TryTakeResult( r );
            }
            detail["stream1"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "stop", r.stopReason }, { "shown", shown },
                                  { "takesWhileActive", takesWhileActive }, { "firstDelta", firstDelta }, { "total", total },
                                  { "usage", r.usage } };
            liveDeltasOk = r.ok && r.stopReason == "tool_use" && takesWhileActive >= 2 && firstDelta >= 0
                        && firstDelta < total - 0.5 && shown == "Hello, streamed world \xC3\xA9" && U8( r.text ) == shown
                        && r.usage.value( "output_tokens", 0 ) == 30;

            // (2) The unchanged AgentSession loop over two streamed requests.
            ToolContext ctx;
            ctx.mode = AgentMode::Advisor;
            const AgentStep s = session.OnResponse( r, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); },
                                                    []() { return false; } );
            if ( s.kind == AgentStep::SendAgain )
            {
               AnthropicResult r2;
               {
                  ChatThread t2( "sk-ant-invalid-selftest", "sys", session.History(), PICOPILOT_DEFAULT_MODEL,
                                 String( base ) + "/stream", 30, ToolDefinitions( AgentMode::Advisor ), shape );
                  t2.Start();
                  t2.Wait();
                  t2.TryTakeResult( r2 );
               }
               const AgentStep s2 = session.OnResponse( r2, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); },
                                                        []() { return false; } );
               String why;
               detail["loop"] = { { "text", U8( r2.text ) }, { "error", U8( r2.error ) } };
               loopOk = s2.kind == AgentStep::Done && U8( r2.text ) == "Got 1 tool_result(s) \xE2\x80\x94 done."
                     && HistoryPrefixIsApiValid( session.History(), why ) && session.History().Length() == 4;
            }

            // (3) API error event mid-stream.
            {
               AnthropicResult e;
               std::string partial;
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-error", 30, nlohmann::json(), shape );
                  t.Start();
                  t.Wait();
                  partial = U8( t.TakeStreamedText() );
                  t.TryTakeResult( e );
               }
               detail["streamError"] = { { "error", U8( e.error ) }, { "partial", partial } };
               errorOk = !e.ok && e.errorKind == RequestErrorKind::Stream && !e.cancelled
                      && e.error == "the reply stream failed: overloaded_error: Overloaded" && partial == "Partial";
            }

            // (4) Stall: message_start, then silence -> the idle deadline ends it.
            {
               RequestShape idle = shape;
               idle.streamIdleSeconds = 3;
               AnthropicResult e;
               const clock::time_point t0 = clock::now();
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-stall", 30, nlohmann::json(), idle );
                  t.Start();
                  t.Wait();
                  t.TryTakeResult( e );
               }
               const double took = secondsSince( t0 );
               detail["stall"] = { { "error", U8( e.error ) }, { "seconds", took } };
               stallOk = !e.ok && e.errorKind == RequestErrorKind::Stalled
                      && e.error == "the reply stalled: no data from the API for 3 s" && took < 10;
            }

            // (5) Cancel mid-stream.
            {
               AnthropicResult e;
               const clock::time_point t0 = clock::now();
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-stall", 30, nlohmann::json(), shape );
                  t.Start();
                  std::this_thread::sleep_for( std::chrono::milliseconds( 1500 ) );
                  t.RequestCancel();
                  t.Wait();
                  t.TryTakeResult( e );
               }
               const double took = secondsSince( t0 );
               detail["cancel"] = { { "error", U8( e.error ) }, { "seconds", took } };
               cancelOk = !e.ok && e.cancelled && e.errorKind == RequestErrorKind::Cancelled
                       && e.error == "request cancelled" && took < 5;
            }

            // (6) The connection closes mid-reply (no message_stop): a precise
            // Stream error, never a partial success.
            {
               AnthropicResult e;
               std::string partial;
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-truncated", 30, nlohmann::json(), shape );
                  t.Start();
                  t.Wait();
                  partial = U8( t.TakeStreamedText() );
                  t.TryTakeResult( e );
               }
               detail["truncated"] = { { "error", U8( e.error ) }, { "partial", partial }, { "http", e.httpStatus } };
               truncatedOk = !e.ok && e.errorKind == RequestErrorKind::Stream && !e.cancelled && e.contentBlocks.is_null()
                          && e.error == "the reply stream ended before it was complete (no message_stop)"
                          && partial == "Cut ";
            }

            // (7)/(8) A non-2xx reply to a streamed request is a plain JSON
            // error body: Http kind, the API's message verbatim, no live text.
            auto httpError = [&]( const char* path, int code, const char* message, const char* key ) -> bool
            {
               AnthropicResult e;
               std::string partial;
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + path, 30, nlohmann::json(), shape );
                  t.Start();
                  t.Wait();
                  partial = U8( t.TakeStreamedText() );
                  t.TryTakeResult( e );
               }
               detail[key] = { { "error", U8( e.error ) }, { "partial", partial }, { "http", e.httpStatus },
                               { "kind", int( e.errorKind ) } };
               return !e.ok && e.errorKind == RequestErrorKind::Http && !e.cancelled && e.httpStatus == code
                   && U8( e.error ) == message && partial.empty() && e.contentBlocks.is_null();
            };
            httpErrorOk = httpError( "/stream-http-error", 529, "Overloaded", "httpError" );
            http401Ok = httpError( "/stream-401", 401, "invalid x-api-key", "http401" );

            // (9) An assembler failure aborts the transfer at once: the server
            // keeps sending ~9 s of pings the client must not wait for.
            {
               AnthropicResult e;
               std::string partial;
               const clock::time_point t0 = clock::now();
               {
                  ChatThread t( "sk-ant-invalid-selftest", "sys", hist, PICOPILOT_DEFAULT_MODEL,
                                String( base ) + "/stream-fail-then-more", 30, nlohmann::json(), shape );
                  t.Start();
                  t.Wait();
                  partial = U8( t.TakeStreamedText() );
                  t.TryTakeResult( e );
               }
               const double took = secondsSince( t0 );
               detail["failFast"] = { { "error", U8( e.error ) }, { "partial", partial }, { "http", e.httpStatus },
                                      { "seconds", took } };
               failFastOk = !e.ok && e.errorKind == RequestErrorKind::Stream && !e.cancelled && e.httpStatus == 0
                         && e.error == "the reply stream failed: unsupported stream delta type 'mystery_delta' (content[0])"
                         && partial == "Early " && took < 3;
            }
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }

      const bool ok = bodyOk && (skipped || (liveDeltasOk && loopOk && errorOk && stallOk && cancelOk && truncatedOk
                                     && httpErrorOk && http401Ok && failFastOk));
      out["streamDetail"] = detail;
      out["streamError"] = U8( error );
      out["streamLoopbackSkipped"] = skipped;
      out["streamTransportOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- inc5 sections end ----

   // Let the core finish deferred window teardown before --force-exit (same
   // reasoning as the drains in the vision and agent self-tests).
   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }
   return allOk;
}

} // namespace pcl
