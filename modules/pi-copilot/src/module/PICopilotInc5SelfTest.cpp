// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AgentSession.h"
#include "AgentTools.h"
#include "AnthropicClient.h"
#include "ChatThread.h"
#include "CopilotSettings.h"
#include "GlobalRunFiles.h"
#include "HistoryBudget.h"
#include "KeyStore.h"
#include "Keyring.h"
#include "ModelCatalog.h"
#include "PanelPlacement.h"
#include "PICopilotInc5SelfTest.h"
#include "PICopilotModule.h"
#include "ProcessApply.h"
#include "ProcessSafety.h"
#include "PjsrRunner.h"
#include "ProcessCatalog.h"
#include "SseStream.h"
#include "SystemPrompt.h"
#include "TurnEndNotes.h"
#include "Utf8.h"
#include "ViewCapture.h"

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
#include <pcl/Settings.h>
#include <pcl/Variant.h>
#include <pcl/View.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

double Inc5Median( View v, int channel )
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

int CountKey( const nlohmann::json& v, const char* key )
{
   int n = 0;
   if ( v.is_object() )
      for ( auto it = v.begin(); it != v.end(); ++it )
         n += (it.key() == key ? 1 : 0) + CountKey( it.value(), key );
   else if ( v.is_array() )
      for ( const nlohmann::json& e : v )
         n += CountKey( e, key );
   return n;
}

AnthropicMessage TextMsg( const char* role, const std::string& text )
{
   AnthropicMessage m;
   m.role = role;
   m.content = String::UTF8ToUTF16( text.c_str() );
   return m;
}

AnthropicMessage BlocksMsg( const char* role, const nlohmann::json& blocks )
{
   AnthropicMessage m;
   m.role = role;
   m.blocks = blocks;
   return m;
}

// k exchanges of: fresh user text, assistant text+tool_use, user tool_result+merged text, assistant text.
Array<AnthropicMessage> LongHistory( int k, size_t chars )
{
   Array<AnthropicMessage> h;
   const std::string big( chars, 'a' );
   for ( int i = 0; i < k; ++i )
   {
      const std::string id = "toolu_h" + std::to_string( i );
      h.Add( TextMsg( "user", "question " + std::to_string( i ) + " " + big ) );
      nlohmann::json a = nlohmann::json::array();
      a.push_back( { { "type", "text" }, { "text", "checking" } } );
      a.push_back( { { "type", "tool_use" }, { "id", id }, { "name", "describe_process" }, { "input", { { "id", "PixelMath" } } } } );
      h.Add( BlocksMsg( "assistant", a ) );
      nlohmann::json u = nlohmann::json::array();
      u.push_back( { { "type", "tool_result" }, { "tool_use_id", id },
                     { "content", nlohmann::json::array( { { { "type", "text" }, { "text", big } } } ) }, { "is_error", false } } );
      u.push_back( { { "type", "text" }, { "text", "and also this" } } );
      h.Add( BlocksMsg( "user", u ) );
      h.Add( TextMsg( "assistant", "answer " + std::to_string( i ) + " " + big ) );
   }
   h.Add( TextMsg( "user", "the current question" ) );
   return h;
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

   // ---- Section B3: models, caching/binding shape, history budget (Task 4) ----
   {
      bool modelsOk = false, shapeOk = false, wireOk = false, trimOk = false, sessionTrimOk = true, noTrimOk = false;
      bool thinkOnlyOk = false, trimResetOk = false;
      bool wireSkipped = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         std::set<std::string> ids;
         for ( const ModelInfo& m : kPICopilotModels )
            ids.insert( m.id );
         modelsOk = ids.size() == PICopilotModelCount && std::string( kPICopilotModels[0].id ) == PICOPILOT_DEFAULT_MODEL
                 && FindModel( "claude-opus-5-5" ) && FindModel( "claude-opus-5-5" )->thinkingBinding
                 && FindModel( "claude-fable-5-1" ) && FindModel( "claude-fable-5-1" )->thinkingBinding
                 && FindModel( "claude-sonnet-5" ) && !FindModel( "claude-sonnet-5" )->thinkingBinding
                 && FindModel( "claude-haiku-4-5" ) && !FindModel( "claude-haiku-4-5" )->thinkingBinding
                 && FindModel( "claude-opus-4-8" ) && !FindModel( "claude-opus-4-8" )->thinkingBinding
                 && FindModel( "claude-nope" ) == nullptr && ModelIndex( "claude-fable-5-1" ) == 2 && ModelIndex( "x" ) == -1;

         Array<AnthropicMessage> hist;
         hist.Add( TextMsg( "user", "hi" ) );
         const nlohmann::json tools = ToolDefinitions( AgentMode::Copilot );
         const nlohmann::json b55 = nlohmann::json::parse(
            BuildMessagesRequestBody( "claude-opus-5-5", "sys", hist, tools, ProductionRequestShape( "claude-opus-5-5" ) ) );
         const nlohmann::json b48 = nlohmann::json::parse(
            BuildMessagesRequestBody( "claude-opus-4-8", "sys", hist, tools, ProductionRequestShape( "claude-opus-4-8" ) ) );
         const nlohmann::json plain = nlohmann::json::parse( BuildMessagesRequestBody( "m", "sys", hist, tools ) );
         const nlohmann::json eph = { { "type", "ephemeral" } };
         shapeOk = b55.at( "system" ).is_array() && b55["system"].size() == 1
                && b55["system"][0].at( "text" ) == "sys" && b55["system"][0].at( "cache_control" ) == eph
                && b55.at( "tools" ).back().at( "cache_control" ) == eph && b55.at( "cache_control" ) == eph
                && CountKey( b55, "cache_control" ) == 3
                && b55.at( "thinking" ) == nlohmann::json::parse(
                      "{\"type\":\"adaptive\",\"block_binding\":{\"prefix_mismatch_behavior\":\"drop_block\"}}" )
                && !b48.contains( "thinking" ) && CountKey( b48, "cache_control" ) == 3
                && plain.at( "system" ).is_string() && CountKey( plain, "cache_control" ) == 0 && !plain.contains( "thinking" );
         detail["b55"] = b55;

         if ( const char* echoUrl = std::getenv( "PICOPILOT_SELFTEST_ECHO_URL" ) )
         {
            wireSkipped = false;
            auto echo = [&]( const char* model ) {
               RequestShape s = ProductionRequestShape( model );
               s.stream = false;   // the echo path answers non-streamed
               AnthropicRequest req( "sk-ant-invalid-selftest", model, "sys", hist, String( echoUrl ), 30, tools, s );
               const AnthropicResult r = req.Perform();
               return r.ok ? nlohmann::json::parse( U8( r.text ) ) : nlohmann::json( { { "error", U8( r.error ) } } );
            };
            const nlohmann::json e55 = echo( "claude-opus-5-5" ), e48 = echo( "claude-opus-4-8" );
            detail["echo55"] = { { "anthropic_beta", e55.value( "anthropic_beta", nlohmann::json() ) }, { "thinking", e55.value( "thinking", nlohmann::json() ) } };
            wireOk = e55.value( "anthropic_beta", nlohmann::json() ) == PICOPILOT_THINKING_BINDING_BETA
                  && e55.at( "thinking" ).at( "block_binding" ).at( "prefix_mismatch_behavior" ) == "drop_block"
                  && e55.at( "system" ).at( 0 ).at( "cache_control" ) == eph
                  && e48.value( "anthropic_beta", nlohmann::json() ).is_null() && e48.value( "thinking", nlohmann::json() ).is_null();
         }

         {  // Direct trim: 30 long exchanges with tool rounds -> within the target, API-valid, current turn kept.
            Array<AnthropicMessage> h = LongHistory( 30, 6000 );
            const size_type before = EstimateHistoryTokens( h );
            const AnthropicMessage lastBefore = h[h.Length()-1];
            const size_type removed = TrimHistoryToBudget( h, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget );
            String why;
            const bool valid = HistoryIsApiValid( h, why );
            const size_type after = EstimateHistoryTokens( h );
            detail["trim"] = { { "before", before }, { "after", after }, { "removed", removed }, { "why", U8( why ) },
                               { "first", h.IsEmpty() ? nlohmann::json() : h[0].blocks } };
            trimOk = before > PICopilotHistoryTokenBudget && removed > 0 && valid
                  && after <= PICopilotHistoryTrimTarget + 100   // + the trim note itself
                  && IsFreshUserTurn( h[0] ) && h[0].blocks.is_array()
                  && h[0].blocks.at( 0 ).at( "text" ) == kPICopilotTrimNote
                  && h[h.Length()-1].content == lastBefore.content;
         }
         {  // Under budget: untouched. Only the current turn over budget: nothing to cut.
            Array<AnthropicMessage> small = LongHistory( 2, 100 );
            const size_type n0 = small.Length();
            Array<AnthropicMessage> huge;
            huge.Add( TextMsg( "user", std::string( 400000, 'b' ) ) );
            noTrimOk = TrimHistoryToBudget( small, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget ) == 0
                    && small.Length() == n0
                    && TrimHistoryToBudget( huge, PICopilotHistoryTokenBudget, PICopilotHistoryTrimTarget ) == 0 && huge.Length() == 1;
         }
         {  // Through AgentSession: 40 long plain turns; the history never exceeds the budget and stays valid.
            AgentSession s;
            size_type trimmedTotal = 0;
            const std::string big( 24000, 'c' );
            for ( int i = 0; i < 40 && sessionTrimOk; ++i )
            {
               s.BeginUserTurn( TextMsg( "user", "turn " + std::to_string( i ) + " " + big ) );
               trimmedTotal += s.TakeTrimmedMessages();
               String why;
               sessionTrimOk = HistoryIsApiValid( s.History(), why )
                            && EstimateHistoryTokens( s.History() ) <= PICopilotHistoryTokenBudget;
               AnthropicResult r;
               r.ok = true;
               r.httpStatus = 200;
               r.stopReason = "end_turn";
               r.text = String::UTF8ToUTF16( ("reply " + big).c_str() );
               r.contentBlocks = nlohmann::json::array( { { { "type", "text" }, { "text", "reply " + big } } } );
               s.OnResponse( r, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
               trimmedTotal += s.TakeTrimmedMessages();
            }
            detail["sessionTrimmed"] = trimmedTotal;
            sessionTrimOk = sessionTrimOk && trimmedTotal > 0;
         }
         {  // A reply whose only kept blocks are thinking (max_tokens while thinking; or
            // thinking + a cut-off tool_use that is dropped) still stores a text block
            // AFTER the verbatim thinking, so a drop_block can never empty the turn.
            const nlohmann::json th = { { "type", "thinking" }, { "thinking", "let me see" }, { "signature", "sig-abc" } };
            const nlohmann::json red = { { "type", "redacted_thinking" }, { "data", "opaque" } };
            auto store = [&]( const nlohmann::json& blocks ) -> nlohmann::json
            {
               AgentSession s;
               s.BeginUserTurn( TextMsg( "user", "q" ) );
               AnthropicResult r;
               r.ok = true;
               r.httpStatus = 200;
               r.stopReason = "max_tokens";
               r.truncated = true;
               r.contentBlocks = blocks;
               const AgentStep st = s.OnResponse( r, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
               String why;
               s.BeginUserTurn( TextMsg( "user", "next" ) );
               if ( st.kind != AgentStep::Done || s.History().Length() != 3 || !HistoryIsApiValid( s.History(), why ) )
                  return nlohmann::json();
               return s.History()[1].blocks;
            };
            const nlohmann::json a = store( nlohmann::json::array( { th } ) );
            const nlohmann::json b = store( nlohmann::json::array( { th, red,
               { { "type", "tool_use" }, { "id", "toolu_x" }, { "name", "describe_process" }, { "input", nlohmann::json::object() } } } ) );
            const nlohmann::json c = store( nlohmann::json::array( { th, { { "type", "text" }, { "text", "partial answer" } } } ) );
            detail["thinkOnly"] = { a, b, c };
            thinkOnlyOk = a.is_array() && a.size() == 2 && a[0] == th && a[1].at( "type" ) == "text"
                       && a[1].at( "text" ) == "[empty reply (stop_reason max_tokens)]"
                       && b.is_array() && b.size() == 3 && b[0] == th && b[1] == red && b[2].at( "type" ) == "text"
                       && b[2].at( "text" ) == "[reply cut off (max_tokens) before a tool call completed]"
                       && c.is_array() && c.size() == 2 && c[0] == th && c[1].at( "text" ) == "partial answer";
         }
         {  // A trim done by BeginUserTurn() is not reported when the request then fails
            // with nothing run: the history is back to the untrimmed snapshot.
            AgentSession s;
            const std::string big( 24000, 'd' );
            for ( int i = 0; i < 12; ++i )
            {
               s.BeginUserTurn( TextMsg( "user", "turn " + std::to_string( i ) + " " + big ) );
               AnthropicResult r;
               r.ok = true;
               r.httpStatus = 200;
               r.stopReason = "end_turn";
               r.contentBlocks = nlohmann::json::array( { { { "type", "text" }, { "text", "reply " + big } } } );
               s.OnResponse( r, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
            }
            s.TakeTrimmedMessages();
            const size_type before = s.History().Length();
            s.BeginUserTurn( TextMsg( "user", "one more " + std::string( 150000, 'e' ) ) );   // ~50k: always over the budget
            const size_type trimmedOnBegin = s.History().Length() < before + 1 ? 1 : 0;
            AnthropicResult fail;
            fail.ok = false;
            fail.errorKind = RequestErrorKind::Http;
            fail.httpStatus = 529;
            fail.error = "Overloaded";
            s.OnResponse( fail, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
            const size_type reported = s.TakeTrimmedMessages();
            detail["trimReset"] = { { "before", before }, { "trimmedOnBegin", trimmedOnBegin }, { "reported", reported },
                                    { "after", s.History().Length() } };
            trimResetOk = trimmedOnBegin == 1 && reported == 0 && s.History().Length() == before;
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }

      const bool ok = modelsOk && shapeOk && (wireSkipped || wireOk) && trimOk && noTrimOk && sessionTrimOk
                   && thinkOnlyOk && trimResetOk;
      detail["flags"] = { { "models", modelsOk }, { "shape", shapeOk }, { "wire", wireOk }, { "trim", trimOk },
                          { "noTrim", noTrimOk }, { "sessionTrim", sessionTrimOk }, { "thinkOnly", thinkOnlyOk },
                          { "trimReset", trimResetOk } };
      out["conversationDetail"] = detail;
      out["conversationError"] = U8( error );
      out["conversationWireSkipped"] = wireSkipped;
      out["conversationOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B3L: gated LIVE caching + thinking binding (Task 4) ------------
   {
      bool skipped = true, ok = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      if ( const char* key = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         skipped = false;
         ok = false;
         try
         {
            // (1) Prompt cache: the second identical-prefix request reads the cache.
            Array<AnthropicMessage> h;
            h.Add( TextMsg( "user", "Reply with the single word: ok." ) );
            nlohmann::json usage[2];
            bool bothOk = true;
            for ( int i = 0; i < 2; ++i )
            {
               AnthropicRequest req( String( key ), PICOPILOT_DEFAULT_MODEL, BuildSystemPrompt( AgentMode::Copilot ), h,
                                     PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                     ToolDefinitions( AgentMode::Copilot ), ProductionRequestShape( PICOPILOT_DEFAULT_MODEL ) );
               const auto t0 = std::chrono::steady_clock::now();
               const AnthropicResult r = req.Perform();
               detail["cacheSeconds"].push_back( std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
               bothOk = bothOk && r.ok;
               usage[i] = r.usage;
               if ( !r.ok )
                  detail["cacheError"] = U8( r.error );
            }
            const int cacheRead = usage[1].value( "cache_read_input_tokens", 0 );
            detail["cacheUsage"] = { usage[0], usage[1] };
            out["liveCacheRead"] = cacheRead;
            const bool cacheOk = bothOk && cacheRead > 0;

            // (2) Opus 5.5 and Fable 5.1, an EDITED history (the first turn's
            //     image is stripped when the tool round is appended) with the
            //     binding on: a 200 through a full tool round. ((3) is the case
            //     that actually makes the API drop a block.) Every thinking block a
            //     reply carried must be in the history verbatim (signature
            //     included), and each request's duration is recorded against
            //     the 600 s deadline (thinking on).
            Inc5TestWindow tw( "PCBindLive", 256, 192, 3, 0.2 );
            View v = tw.MainView();
            auto runBinding = [&]( const char* model, nlohmann::json& transformations, nlohmann::json& d ) -> bool
            {
               ToolContext ctx;
               ctx.mode = AgentMode::Advisor;
               ctx.turnViewId = v.FullId();
               AgentSession session;
               StringList notes;
               session.BeginUserTurn( CaptureViewTurn( "Call describe_process for PixelMath, then answer in one short sentence.", &v, notes ) );
               AgentStep s;
               int requests = 0;
               bool allArrays = true;
               nlohmann::json seconds = nlohmann::json::array();
               nlohmann::json thinking = nlohmann::json::array();   // every thinking / redacted_thinking block received
               transformations = nlohmann::json::array();
               do
               {
                  AnthropicRequest req( String( key ), model, BuildSystemPrompt( AgentMode::Advisor ), session.History(),
                                        PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                        ToolDefinitions( AgentMode::Advisor ), ProductionRequestShape( model ) );
                  const auto t0 = std::chrono::steady_clock::now();
                  const AnthropicResult r = req.Perform();
                  seconds.push_back( std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() );
                  ++requests;
                  if ( !r.ok )
                     d["bindingError"] = { { "status", r.httpStatus }, { "error", U8( r.error ) } };
                  allArrays = allArrays && r.inputTransformations.is_array();
                  transformations.push_back( r.inputTransformations );
                  if ( r.contentBlocks.is_array() )
                     for ( const nlohmann::json& blk : r.contentBlocks )
                        if ( blk.is_object() && (blk.value( "type", std::string() ) == "thinking"
                                              || blk.value( "type", std::string() ) == "redacted_thinking") )
                           thinking.push_back( blk );
                  s = session.OnResponse( r, [&ctx]( const ToolCall& c ) { return ExecuteTool( c, ctx ); }, []() { return false; } );
               }
               while ( s.kind == AgentStep::SendAgain && requests < 5 );

               int kept = 0, signed_ = 0;
               for ( const nlohmann::json& t : thinking )
               {
                  bool found = false;
                  for ( const AnthropicMessage& m : session.History() )
                     if ( m.role == "assistant" && m.blocks.is_array() )
                        for ( const nlohmann::json& blk : m.blocks )
                           found = found || blk == t;
                  kept += found ? 1 : 0;
                  signed_ += (t.contains( "signature" ) || t.contains( "data" )) ? 1 : 0;
               }
               d["requests"] = requests;
               d["seconds"] = seconds;
               d["thinkingBlocks"] = int( thinking.size() );
               d["thinkingKeptVerbatim"] = kept;
               d["thinkingSigned"] = signed_;
               d["final"] = int( s.kind );
               d["answer"] = U8( s.assistantText );
               bool inTime = true;
               for ( const nlohmann::json& sec : seconds )
                  inTime = inTime && sec.get<double>() < PICopilotRequestTimeoutSeconds;
               return s.kind == AgentStep::Done && requests >= 2 && allArrays && inTime
                   && kept == int( thinking.size() ) && signed_ == int( thinking.size() );
            };
            // (3) Opus 5.5, a TRIMMED history: TrimHistoryToBudget() removes the
            //     first exchange in front of an exchange whose reply thought, so
            //     that thinking block's bound prefix changed. The request must be
            //     a 200, and when the reply did think, the API must report the
            //     drop (input_transformations: thinking_dropped). The image-strip
            //     edit in (2) turned out not to be a prefix mismatch (observed
            //     2026-09-24: no transformation with or without the binding).
            bool trimLiveOk = false;
            {
               nlohmann::json log = nlohmann::json::array();
               Array<AnthropicMessage> h;
               auto send = [&]() -> AnthropicResult
               {
                  AnthropicRequest req( String( key ), "claude-opus-5-5", BuildSystemPrompt( AgentMode::Advisor ), h,
                                        PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                        nlohmann::json()/*no tools: plain text replies*/,
                                        ProductionRequestShape( "claude-opus-5-5" ) );
                  const auto t0 = std::chrono::steady_clock::now();
                  const AnthropicResult r = req.Perform();
                  log.push_back( { { "status", r.httpStatus }, { "error", U8( r.error ) }, { "transformations", r.inputTransformations },
                                   { "seconds", std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() } } );
                  return r;
               };
               auto reply = []( const AnthropicResult& r )
               {
                  AnthropicMessage m;
                  m.role = "assistant";
                  m.blocks = r.contentBlocks;
                  return m;
               };
               h.Add( TextMsg( "user", "What is 17*23? Answer with just the number." ) );
               const AnthropicResult r1 = send();
               bool thought = false;
               if ( r1.ok )
               {
                  h.Add( reply( r1 ) );
                  // The binding is only exercised when r2 carries a thinking block
                  // (adaptive thinking may skip an easy question): ask something
                  // that invites thinking, up to 3 times.
                  h.Add( TextMsg( "user", "Think carefully, step by step, before answering: how many prime numbers "
                                          "lie strictly between 100 and 200? Answer with just the number." ) );
                  AnthropicResult r2;
                  for ( int attempt = 0; attempt < 3 && !thought; ++attempt )
                  {
                     r2 = send();
                     if ( !r2.ok )
                        break;
                     for ( const nlohmann::json& blk : r2.contentBlocks )
                        thought = thought || (blk.is_object() && blk.value( "type", std::string() ) == "thinking");
                  }
                  if ( r2.ok && !thought )
                     detail["trimLiveReason"] = "binding not exercised: model did not think (3 attempts)";
                  if ( r2.ok && thought )
                  {
                     h.Add( reply( r2 ) );
                     h.Add( TextMsg( "user", "Thanks. Reply with the single word: ok." ) );
                     const size_type removed = TrimHistoryToBudget( h, EstimateHistoryTokens( h ) - 1, EstimateHistoryTokens( h, 2 ) );
                     String why;
                     const bool valid = HistoryIsApiValid( h, why );
                     const AnthropicResult r3 = send();
                     bool dropped = false;
                     if ( r3.inputTransformations.is_array() )
                        for ( const nlohmann::json& t : r3.inputTransformations )
                           dropped = dropped || (t.is_object() && t.value( "type", std::string() ) == "thinking_dropped");
                     detail["trimLiveRemoved"] = removed;
                     out["liveTrimTransformations"] = r3.inputTransformations;
                     trimLiveOk = removed == 2 && valid && r3.ok && r3.httpStatus == 200 && dropped;
                     if ( !dropped )
                        detail["trimLiveReason"] = "the model thought, but the trimmed request reported no thinking_dropped";
                  }
               }
               detail["trimLive"] = log;
               out["liveTrimThought"] = thought;
            }
            // (4) Model switch mid-conversation (fix round 1): an Opus 5.5 turn
            //     that thought, then the next message on another model. Raw =
            //     the Opus 5.5 thinking blocks re-sent verbatim (recorded only:
            //     what the API does with foreign blocks); stripped = the
            //     session's SetModel() path, which must be a 200 for each target.
            bool switchOk = false;
            {
               nlohmann::json sw = nlohmann::json::object();
               AgentSession base;
               base.SetModel( "claude-opus-5-5" );
               base.BeginUserTurn( TextMsg( "user", "Think carefully, step by step, before answering: how many prime numbers "
                                                    "lie strictly between 100 and 200? Answer with just the number." ) );
               auto sendOn = [&]( const char* model, const Array<AnthropicMessage>& hist ) -> AnthropicResult
               {
                  AnthropicRequest req( String( key ), model, BuildSystemPrompt( AgentMode::Advisor ), hist,
                                        PICOPILOT_MESSAGES_URL, PICopilotRequestTimeoutSeconds,
                                        nlohmann::json()/*no tools*/, ProductionRequestShape( model ) );
                  return req.Perform();
               };
               bool thought = false;
               AnthropicResult r0;
               const Array<AnthropicMessage> start = base.History();
               for ( int attempt = 0; attempt < 3 && !thought; ++attempt )
               {
                  r0 = sendOn( "claude-opus-5-5", start );
                  if ( !r0.ok )
                     break;
                  for ( const nlohmann::json& blk : r0.contentBlocks )
                     thought = thought || (blk.is_object() && blk.value( "type", std::string() ) == "thinking");
               }
               sw["firstStatus"] = r0.httpStatus;
               sw["thought"] = thought;
               if ( r0.ok && thought )
               {
                  const AgentStep st0 = base.OnResponse( r0, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
                  sw["firstStep"] = int( st0.kind );
                  const AnthropicMessage next = TextMsg( "user", "Thanks. Reply with the single word: ok." );
                  bool strippedAll200 = st0.kind == AgentStep::Done;
                  for ( const char* target : { "claude-opus-4-8", "claude-sonnet-5", "claude-fable-5-1" } )
                  {
                     AgentSession raw = base;           // foreign thinking re-sent verbatim
                     raw.BeginUserTurn( next );
                     const AnthropicResult rr = sendOn( target, raw.History() );
                     AgentSession fixed = base;         // the production path
                     fixed.SetModel( target );
                     fixed.BeginUserTurn( next );
                     String why;
                     const bool valid = HistoryIsApiValid( fixed.History(), why );
                     const AnthropicResult rs = sendOn( target, fixed.History() );
                     sw[target] = { { "rawStatus", rr.httpStatus }, { "rawError", U8( rr.error ) },
                                    { "strippedStatus", rs.httpStatus }, { "strippedError", U8( rs.error ) },
                                    { "strippedValid", valid } };
                     strippedAll200 = strippedAll200 && valid && rs.ok && rs.httpStatus == 200;
                  }
                  switchOk = strippedAll200;
               }
               else
                  sw["reason"] = r0.ok ? "switch not exercised: Opus 5.5 did not think (3 attempts)" : "first request failed";
               detail["modelSwitch"] = sw;
               out["liveModelSwitch"] = sw;
            }

            nlohmann::json t55, tFable, d55 = nlohmann::json::object(), dFable = nlohmann::json::object();
            const bool ok55 = runBinding( "claude-opus-5-5", t55, d55 );
            const bool okFable = runBinding( "claude-fable-5-1", tFable, dFable );
            out["liveBindingTransformations"] = t55;
            out["liveBindingTransformationsFable"] = tFable;
            detail["binding55"] = d55;
            detail["bindingFable"] = dFable;
            const bool bindingOk = ok55 && okFable && trimLiveOk && switchOk;
            ok = cacheOk && bindingOk;
         }
         catch ( const pcl::Exception& x ) { error = x.Message(); }
         catch ( const std::exception& x ) { error = String( x.what() ); }
      }
      out["liveConversationDetail"] = detail;
      out["liveConversationError"] = U8( error );
      out["liveConversationSkipped"] = skipped;
      out["liveConversationOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B4: keyring-first key storage (Task 5) ------------------------
   {
      bool missOk = false, saveOk = false, loadOk = false, migrateOk = false, fallbackOk = false,
           clearOk = false, noLeakOk = true;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      const IsoString sk = "PICopilot/SelfTestApiKey";
      KeyringId id;
      id.service = "picopilot-selftest";
      id.account = String().Format( "b4-%u", unsigned( std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF ) );
      auto noLeak = [&]( const String& s ) { noLeakOk = noLeakOk && !s.Contains( "sk-ant-selftest" ); };
      try
      {
         KeyStore::SetKeyringForSelfTest( id, sk );
         Settings::Remove( sk );

         const KeyringResult miss = KeyringLookup( id );
         noLeak( miss.error );
         detail["miss"] = { { "ok", miss.ok }, { "found", miss.found }, { "error", U8( miss.error ) } };
         missOk = miss.ok && !miss.found;

         const KeyStore::State saved = KeyStore::Save( "sk-ant-selftest-AAAA" );
         noLeak( saved.note );
         const KeyringResult back = KeyringLookup( id );
         String settingsCopy;
         Settings::Read( sk, settingsCopy );
         detail["save"] = { { "where", int( saved.where ) }, { "note", U8( saved.note ) } };
         saveOk = saved.where == KeyStore::Where::Keyring && back.found && back.secret == "sk-ant-selftest-AAAA"
               && settingsCopy.IsEmpty() && KeyStore::DescribeWhere( saved ) == "stored in the system keyring";

         KeyStore::SetKeyringForSelfTest( id, sk );   // drop the cache: a fresh Load() reads the keyring
         const KeyStore::State loaded = KeyStore::Load();
         loadOk = loaded.where == KeyStore::Where::Keyring && loaded.key == "sk-ant-selftest-AAAA" && loaded.note.IsEmpty();

         // Migration: a plaintext Settings key moves into the keyring, verified, and the plaintext is removed.
         clearOk = KeyStore::Clear().note.IsEmpty() && !KeyringLookup( id ).found;
         Settings::Write( sk, String( "sk-ant-selftest-BBBB" ) );
         KeyStore::SetKeyringForSelfTest( id, sk );
         const KeyStore::State migrated = KeyStore::Load();
         noLeak( migrated.note );
         String left;
         Settings::Read( sk, left );
         const KeyringResult mback = KeyringLookup( id );
         detail["migrate"] = { { "where", int( migrated.where ) }, { "note", U8( migrated.note ) } };
         migrateOk = migrated.where == KeyStore::Where::Keyring && migrated.key == "sk-ant-selftest-BBBB"
                  && migrated.note.Contains( "moved" ) && left.IsEmpty() && mback.found && mback.secret == "sk-ant-selftest-BBBB";

         // Keyring unusable -> Settings, with a visible note; the plaintext copy is kept.
         KeyringId bad = id;
         bad.program = "/nonexistent/secret-tool";
         KeyStore::SetKeyringForSelfTest( bad, sk );
         const KeyStore::State fb = KeyStore::Save( "sk-ant-selftest-CCCC" );
         noLeak( fb.note );
         String plain;
         Settings::Read( sk, plain );
         KeyStore::SetKeyringForSelfTest( bad, sk );
         const KeyStore::State fbLoad = KeyStore::Load();
         noLeak( fbLoad.note );
         const KeyStore::Cleared badClear = KeyStore::Clear();
         noLeak( badClear.note );
         String afterClear;
         Settings::Read( sk, afterClear );
         detail["fallback"] = { { "note", U8( fb.note ) }, { "loadNote", U8( fbLoad.note ) }, { "clear", U8( badClear.note ) }, { "clearWarning", badClear.warning } };
         fallbackOk = fb.where == KeyStore::Where::Settings && plain == "sk-ant-selftest-CCCC"
                   && fb.note.Contains( "keyring could not be used" ) && fb.note.Contains( "not installed" )
                   && KeyStore::DescribeWhere( fb ) == "stored in PixInsight's settings (plain text)"
                   && fbLoad.where == KeyStore::Where::Settings && fbLoad.key == "sk-ant-selftest-CCCC"
                   && badClear.note.Contains( "not installed" ) && !badClear.warning && afterClear.IsEmpty();

         KeyStore::SetKeyringForSelfTest( id, sk );
         clearOk = clearOk && KeyStore::Clear().note.IsEmpty() && !KeyringLookup( id ).found
                && KeyStore::Load().where == KeyStore::Where::None;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      KeyringClear( id );
      Settings::Remove( sk );

      const bool ok = missOk && saveOk && loadOk && migrateOk && fallbackOk && clearOk && noLeakOk;
      out["keyStoreDetail"] = detail;
      out["keyStoreError"] = U8( error );
      out["keyStoreKeyringOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B5: settings, placement side, failure wording (Task 6) ---------
   {
      bool settingsOk = false, placementOk = false, wordingOk = true, kindOk = false, refusalOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         for ( const char* k : { "PICopilot/Model", "PICopilot/RunPjsrEnabled", "PICopilot/PanelSide" } )
            Settings::Remove( IsoString( k ) );
         const bool defaults = CopilotSettings::LoadModel() == PICOPILOT_DEFAULT_MODEL
                            && !CopilotSettings::LoadRunPjsrEnabled()
                            && CopilotSettings::LoadPanelSide() == PanelSide::Right;
         CopilotSettings::SaveModel( "claude-opus-5-5" );
         CopilotSettings::SaveRunPjsrEnabled( true );
         CopilotSettings::SavePanelSide( PanelSide::Left );
         const bool stored = CopilotSettings::LoadModel() == "claude-opus-5-5" && CopilotSettings::LoadRunPjsrEnabled()
                          && CopilotSettings::LoadPanelSide() == PanelSide::Left;
         Settings::Write( "PICopilot/Model", String( "claude-bogus-9" ) );
         const bool unknownFallsBack = CopilotSettings::LoadModel() == PICOPILOT_DEFAULT_MODEL;
         for ( const char* k : { "PICopilot/Model", "PICopilot/RunPjsrEnabled", "PICopilot/PanelSide" } )
            Settings::Remove( IsoString( k ) );
         settingsOk = defaults && stored && unknownFallsBack;

         const PanelPlacement r = ComputeDefaultPanelPlacement( 1280, 720, 420, 40, 60, 8 );
         const PanelPlacement l = ComputeDefaultPanelPlacement( 1280, 720, 420, 40, 60, 8, PanelSide::Left );
         const PanelPlacement ml = ComputeDefaultPanelPlacement( 3200, 720, 420, 40, 60, 8, PanelSide::Left );
         placementOk = r.ok && r.x == 2560 - 420 - 8 && l.ok && l.x == 8 && l.y == 40 && l.width == 420
                    && l.height == 1440 - 100 && ml.ok && ml.x == 1920 + 8;

         auto note = []( RequestErrorKind k, const char* err, int status ) {
            AgentStep s;
            s.kind = AgentStep::Failed;
            s.error = err;
            s.errorKind = k;
            const TurnEndView v = DescribeTurnEnd( s, status );
            return v.notes.IsEmpty() ? String() : v.notes[0];
         };
         struct Case { RequestErrorKind kind; const char* err; int status; const char* expect; };
         const Case cases[] = {
            { RequestErrorKind::TimedOut, "request timed out after 600 s", 0,
              "The request took too long and was stopped (request timed out after 600 s). Send the message again, or ask for something smaller." },
            { RequestErrorKind::Stalled, "the reply stalled: no data from the API for 120 s", 0,
              "The reply stalled and was stopped (the reply stalled: no data from the API for 120 s). Send the message again." },
            { RequestErrorKind::Network, "network request failed: Could not resolve host", 0,
              "Could not reach the Anthropic API (network request failed: Could not resolve host). Check the internet connection, then send the message again." },
            { RequestErrorKind::Stream, "the reply stream failed: overloaded_error: Overloaded", 200,
              "The reply was cut off by the Anthropic API (the reply stream failed: overloaded_error: Overloaded). Send the message again." },
            { RequestErrorKind::Http, "invalid x-api-key", 401,
              "Anthropic API error 401: invalid x-api-key (check your API key in PI Copilot's settings)" },
            { RequestErrorKind::Http, "Overloaded", 529,
              "Anthropic API error 529: Overloaded (the service is busy; wait a moment, then send again)" },
            { RequestErrorKind::BadReply, "no text in reply (stop_reason=pause_turn)", 200,
              "Unexpected reply from the Anthropic API: no text in reply (stop_reason=pause_turn)" },
            // Carried from the Task 3/4 reviews: every kind worded, never "Error 0".
            { RequestErrorKind::Http, "network error", 0,
              "Anthropic API error: network error" },
            { RequestErrorKind::Build, "image too large", 0,
              "PI Copilot could not build the request (image too large). Nothing was sent; send the message again, "
              "or press New chat if this keeps happening." },
            { RequestErrorKind::Internal, "worker thread ended without a result", 0,
              "Internal error in PI Copilot (worker thread ended without a result). Send the message again." },
            { RequestErrorKind::Cancelled, "request cancelled", 0,
              "The request was cancelled (request cancelled). Send the message again." },
         };
         nlohmann::json w = nlohmann::json::array();
         for ( const Case& c : cases )
         {
            const String n = note( c.kind, c.err, c.status );
            const bool pass = n == c.expect && !n.Contains( "Error 0" );
            w.push_back( { { "note", U8( n ) }, { "pass", pass } } );
            wordingOk = wordingOk && pass;
         }
         // A streamed reply cut off: Stop says the partial text is not kept;
         // a failure says it was interrupted, before the cause.
         {
            AgentStep st;
            st.kind = AgentStep::Stopped;
            st.errorKind = RequestErrorKind::Cancelled;
            st.error = "request cancelled";
            const TurnEndView sv = DescribeTurnEnd( st, 0, true/*partialReplyCut*/ );
            const TurnEndView sn = DescribeTurnEnd( st, 0 );
            AgentStep fl;
            fl.kind = AgentStep::Failed;
            fl.errorKind = RequestErrorKind::Stalled;
            fl.error = "the reply stalled: no data from the API for 120 s";
            const TurnEndView fv = DescribeTurnEnd( fl, 200, true );
            const bool pass = sv.notes.Length() == 1
                           && sv.notes[0] == "(stopped -- the partial reply above is not kept in the conversation)"
                           && sn.notes.Length() == 1 && sn.notes[0] == "(stopped)"
                           && fv.notes.Length() == 2
                           && fv.notes[0] == "(the partial reply above was interrupted; it is not kept in the conversation)"
                           && fv.notes[1].StartsWith( "The reply stalled and was stopped" );
            w.push_back( { { "note", U8( sv.notes.IsEmpty() ? String() : sv.notes[0] ) }, { "pass", pass } } );
            wordingOk = wordingOk && pass;
         }
         detail["wording"] = w;

         AgentSession s;
         AnthropicMessage u;
         u.role = "user";
         u.content = "x";
         s.BeginUserTurn( u );
         AnthropicResult stalled;
         stalled.errorKind = RequestErrorKind::Stalled;
         stalled.error = "the reply stalled: no data from the API for 120 s";
         const AgentStep st = s.OnResponse( stalled, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
         kindOk = st.kind == AgentStep::Failed && st.errorKind == RequestErrorKind::Stalled;

         // AbortTurn carries its cause (fix round 1): "could not start" is
         // Internal, an invalid history is Build -- both worded with a next step.
         {
            AgentSession a;
            a.BeginUserTurn( u );
            const AgentStep ab = a.AbortTurn( "history invalid: x (nothing was sent)", RequestErrorKind::Build );
            const TurnEndView av = DescribeTurnEnd( ab, 0 );
            const bool pass = ab.errorKind == RequestErrorKind::Build && !av.notes.IsEmpty()
                           && av.notes[0].StartsWith( "PI Copilot could not build the request (history invalid" );
            detail["abortKind"] = { { "note", U8( av.notes.IsEmpty() ? String() : av.notes[0] ) }, { "pass", pass } };
            wordingOk = wordingOk && pass;
         }

         const AnthropicResult ref = ParseMessagesResponse( 200, IsoString( "{\"content\":[],\"stop_reason\":\"refusal\"}" ), String() );
         detail["refusal"] = U8( ref.error );
         refusalOk = !ref.ok && ref.errorKind == RequestErrorKind::BadReply
                  && ref.error == "the model declined this request (stop_reason refusal); rephrase it, or choose "
                                  "another model in PI Copilot's settings";
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }

      // Carried from the Task 4/5 reviews: a visible note for an unknown saved
      // model; legacy Settings key validated before migration; Clear without
      // secret-tool is informational; the no-key note mentions a locked
      // keyring; "Waiting for the system keyring..." fires only for a slow
      // call; a helper that exits without reading stdin still yields its
      // finished result.
      bool modelNoteOk = false, legacyOk = false, clearInfoOk = false, noKeyOk = false, waitOk = false,
           earlyExitOk = false, stripOk = false;
      const IsoString sk = "PICopilot/SelfTestApiKey";
      KeyringId id;
      id.service = "picopilot-selftest";
      id.account = String().Format( "b5-%u", unsigned( std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFF ) );
      std::string scriptDir;
      try
      {
         // Model switch (fix round 1): thinking / redacted_thinking blocks are
         // bound to the model that produced them. Switching strips them from
         // every earlier assistant turn of another model; no turn gets empty;
         // the history stays API-valid; the same model keeps them verbatim.
         {
            auto blocksOf = []( const char* j ) { return nlohmann::json::parse( j ); };
            AgentSession ss;
            ss.SetModel( "claude-opus-5-5" );
            AnthropicMessage u1; u1.role = "user"; u1.content = "q1";
            ss.BeginUserTurn( u1 );
            AnthropicResult r1;
            r1.ok = true; r1.stopReason = "tool_use"; r1.text = "";
            r1.contentBlocks = blocksOf( "[{\"type\":\"thinking\",\"thinking\":\"t\",\"signature\":\"s1\"},"
                                         "{\"type\":\"redacted_thinking\",\"data\":\"d1\"},"
                                         "{\"type\":\"tool_use\",\"id\":\"tu1\",\"name\":\"list_processes\",\"input\":{}}]" );
            ToolOutcome okOutcome;
            okOutcome.content.push_back( { { "type", "text" }, { "text", "done" } } );
            const AgentStep a1 = ss.OnResponse( r1, [&okOutcome]( const ToolCall& ) { return okOutcome; }, []() { return false; } );
            AnthropicResult r2;
            r2.ok = true; r2.stopReason = "end_turn"; r2.text = "answer";
            r2.contentBlocks = blocksOf( "[{\"type\":\"thinking\",\"thinking\":\"t2\",\"signature\":\"s2\"},"
                                         "{\"type\":\"text\",\"text\":\"answer\"}]" );
            const AgentStep a2 = ss.OnResponse( r2, []( const ToolCall& ) { return ToolOutcome(); }, []() { return false; } );
            auto countThinking = []( const Array<AnthropicMessage>& h )
            {
               int n = 0;
               for ( const AnthropicMessage& m : h )
                  if ( m.blocks.is_array() )
                     for ( const nlohmann::json& b : m.blocks )
                        if ( b.is_object() && (b.value( "type", std::string() ) == "thinking"
                                            || b.value( "type", std::string() ) == "redacted_thinking") )
                           ++n;
               return n;
            };
            const int before = countThinking( ss.History() );
            ss.SetModel( "claude-opus-5-5" );   // same model: kept verbatim
            const int same = countThinking( ss.History() );
            ss.SetModel( "claude-opus-4-8" );   // different model: stripped
            const int after = countThinking( ss.History() );
            bool noneEmpty = true, toolUseKept = false;
            for ( const AnthropicMessage& m : ss.History() )
               if ( m.role == "assistant" )
               {
                  noneEmpty = noneEmpty && m.blocks.is_array() && !m.blocks.empty();
                  if ( m.blocks.is_array() )
                     for ( const nlohmann::json& b : m.blocks )
                        toolUseKept = toolUseKept || b.value( "type", std::string() ) == "tool_use";
               }
            AnthropicMessage u2; u2.role = "user"; u2.content = "q2";
            ss.BeginUserTurn( u2 );
            String why;
            const bool valid = HistoryIsApiValid( ss.History(), why );

            // A turn of thinking only (defensive: the placeholder rule keeps
            // one from being stored, but a stripped turn must never be empty).
            Array<AnthropicMessage> h;
            h.Add( TextMsg( "user", "x" ) );
            AnthropicMessage onlyThinking;
            onlyThinking.role = "assistant";
            onlyThinking.model = "claude-fable-5-1";
            onlyThinking.blocks = blocksOf( "[{\"type\":\"thinking\",\"thinking\":\"t\",\"signature\":\"s\"}]" );
            h.Add( onlyThinking );
            h.Add( TextMsg( "user", "y" ) );
            const size_type removed = StripForeignThinking( h, "claude-sonnet-5" );
            String why2;
            const bool valid2 = HistoryIsApiValid( h, why2 );
            detail["strip"] = { { "before", before }, { "same", same }, { "after", after }, { "noneEmpty", noneEmpty },
                                { "toolUseKept", toolUseKept }, { "valid", valid }, { "why", U8( why ) },
                                { "removedOnly", int( removed ) }, { "valid2", valid2 },
                                { "placeholder", h[1].blocks } };
            stripOk = a1.kind == AgentStep::SendAgain && a2.kind == AgentStep::Done
                   && before == 3 && same == 3 && after == 0 && noneEmpty && toolUseKept && valid
                   && removed == 1 && valid2 && h[1].blocks.is_array() && h[1].blocks.size() == 1
                   && h[1].blocks[0].value( "type", std::string() ) == "text";
         }

         Settings::Write( "PICopilot/Model", String( "claude-bogus-9" ) );
         String mnote;
         const IsoString mid = CopilotSettings::LoadModel( &mnote );
         Settings::Remove( IsoString( "PICopilot/Model" ) );
         String unsetNote = "x";
         const IsoString mdef = CopilotSettings::LoadModel( &unsetNote );
         detail["modelNote"] = U8( mnote );
         modelNoteOk = mid == PICOPILOT_DEFAULT_MODEL && mnote.Contains( "claude-bogus-9" )
                    && mnote.Contains( "settings" ) && mdef == PICOPILOT_DEFAULT_MODEL && unsetNote.IsEmpty();

         KeyStore::SetKeyringForSelfTest( id, sk );
         Settings::Write( sk, String( "  sk-ant-selftest-DDDD \n" ) );
         KeyStore::SetKeyringForSelfTest( id, sk );
         const KeyStore::State trimmed = KeyStore::Load();
         const KeyringResult tback = KeyringLookup( id );
         KeyStore::Clear();
         Settings::Write( sk, String( "sk-ant-sel ftest-EEEE" ) );
         KeyStore::SetKeyringForSelfTest( id, sk );
         const KeyStore::State invalid = KeyStore::Load();
         const KeyringResult iback = KeyringLookup( id );
         detail["legacy"] = { { "trimmedWhere", int( trimmed.where ) }, { "invalidNote", U8( invalid.note ) } };
         legacyOk = trimmed.where == KeyStore::Where::Keyring && trimmed.key == "sk-ant-selftest-DDDD"
                 && tback.found && tback.secret == "sk-ant-selftest-DDDD"
                 && invalid.key.IsEmpty() && invalid.where == KeyStore::Where::None
                 && invalid.note.Contains( "not a valid" ) && !invalid.note.Contains( "sk-ant-selftest" )
                 && iback.ok && !iback.found;
         KeyStore::Clear();

         KeyringId absent = id;
         absent.program = "/nonexistent/secret-tool";
         KeyStore::SetKeyringForSelfTest( absent, sk );
         const KeyStore::Cleared ci = KeyStore::Clear();
         detail["clearAbsent"] = { { "note", U8( ci.note ) }, { "warning", ci.warning } };
         clearInfoOk = !ci.warning && ci.note.Contains( "not installed" ) && !ci.note.Contains( "Could not remove" );

         const String nk = KeyStore::NoKeyNote();
         detail["noKey"] = U8( nk );
         noKeyOk = nk.Contains( "locked" ) && nk.Contains( "settings" );

         // Helper scripts standing in for secret-tool (never the real keyring).
         scriptDir = std::string( File::SystemTempDirectory().ToUTF8().c_str() )
                   + "/picopilot-b5-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
         std::filesystem::create_directories( scriptDir );
         auto script = [&]( const char* name, const char* body ) {
            const std::string path = scriptDir + "/" + name;
            std::ofstream( path ) << "#!/bin/sh\n" << body << "\n";
            std::filesystem::permissions( path, std::filesystem::perms::owner_all );
            return String( path.c_str() );
         };
         KeyringId slow = id;
         slow.program = script( "slow", "sleep 1\nexit 1" );
         KeyringId fast = id;
         fast.program = script( "fast", "exit 1" );
         std::vector<int> events;
         {
            KeyringWaitScope scope( [&events]( bool waiting ) { events.push_back( waiting ? 1 : 0 ); } );
            const KeyringResult sr = KeyringLookup( slow );
            const size_type slowEvents = events.size();
            const KeyringResult fr = KeyringLookup( fast );
            waitOk = sr.ok && !sr.found && fr.ok && slowEvents == 2 && events.size() == 2
                  && events[0] == 1 && events[1] == 0;
         }
         detail["waitEvents"] = events;

         KeyringId early = id;
         early.program = script( "early", "exit 3" );
         earlyExitOk = true;
         nlohmann::json ee = nlohmann::json::array();
         for ( int i = 0; i < 5; ++i )
         {
            const KeyringResult er = KeyringStore( early, "PI Copilot self-test", IsoString( "sk-ant-selftest-FFFF" ) );
            ee.push_back( U8( er.error ) );
            earlyExitOk = earlyExitOk && !er.ok && er.error.Contains( "exit 3" ) && !er.error.Contains( "sk-ant-selftest" );
         }
         detail["earlyExit"] = ee;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      KeyringClear( id );
      Settings::Remove( sk );
      Settings::Remove( IsoString( "PICopilot/Model" ) );
      if ( !scriptDir.empty() )
      {
         std::error_code ec;
         std::filesystem::remove_all( scriptDir, ec );
      }
      detail["carried"] = { { "modelNote", modelNoteOk }, { "legacy", legacyOk }, { "clearInfo", clearInfoOk },
                            { "noKey", noKeyOk }, { "wait", waitOk }, { "earlyExit", earlyExitOk },
                            { "strip", stripOk } };

      const bool ok = settingsOk && placementOk && wordingOk && kindOk && refusalOk
                   && modelNoteOk && legacyOk && clearInfoOk && noKeyOk && waitOk && earlyExitOk && stripOk;
      out["configDetail"] = detail;
      out["configError"] = U8( error );
      out["configPolishOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B6: process safety policy (Task 7) -----------------------------
   {
      bool coverageOk = false, idsOk = false, verdictOk = false, denyToolOk = false, confirmToolOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         const nlohmann::json unclassified = UnclassifiedSideEffectCandidates();
         const nlohmann::json unknown = UnknownPolicyProcessIds();
         detail["unclassified"] = unclassified;   // the review worklist (Step 5)
         detail["unknownPolicyIds"] = unknown;
         coverageOk = unclassified.is_array() && unclassified.empty();
         idsOk = unknown.is_array() && unknown.empty();

         const SafetyVerdict pc = CheckProcessSafety( "ProcessContainer", nlohmann::json::object(), nlohmann::json() );
         const SafetyVerdict iiPlain = CheckProcessSafety( "ImageIntegration",
            { { "generateDrizzleData", false }, { "closePreviousImages", false } }, nlohmann::json() );
         const SafetyVerdict iiDrz = CheckProcessSafety( "ImageIntegration", { { "generateDrizzleData", true } }, nlohmann::json() );
         const SafetyVerdict pm = CheckProcessSafety( "PixelMath", { { "expression", "$T" } }, nlohmann::json() );
         verdictOk = pc.kind == SafetyVerdict::Deny && pc.reason.Contains( "cannot check" )
                  && CheckProcessSafety( "PICopilot", nlohmann::json::object(), nlohmann::json() ).kind == SafetyVerdict::Deny
                  && iiPlain.kind == SafetyVerdict::Allow
                  && iiDrz.kind == SafetyVerdict::Confirm && iiDrz.reason.Contains( ".xdrz" )
                  && pm.kind == SafetyVerdict::Allow
                  // a notEquals rule: the default (empty command) runs; any command asks
                  && CheckProcessSafety( "MultiscaleGradientCorrection", nlohmann::json::object(), nlohmann::json() ).kind == SafetyVerdict::Allow
                  && CheckProcessSafety( "MultiscaleGradientCorrection", { { "command", "configure" } }, nlohmann::json() ).kind == SafetyVerdict::Confirm;

         Inc5TestWindow tw( "PCSafetyT", 64, 48, 1, 0.4 );
         View v = tw.MainView();
         int confirmCalls = 0;
         bool answer = false;
         String lastChanges;
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         ctx.turnViewId = v.FullId();
         ctx.confirm = [&]( const String&, const String&, const String& changes ) { ++confirmCalls; lastChanges = changes; return answer; };

         const ToolOutcome denied = ExecuteTool( ToolCall{ "s1", "apply_process", { { "process_id", "ProcessContainer" } } }, ctx );
         detail["deny"] = denied.content;
         denyToolOk = denied.isError && confirmCalls == 0
                   && denied.content.at( 0 ).at( "text" ).get<std::string>().find( "is not allowed from PI Copilot" ) != std::string::npos;

         // A confirm rule, in Copilot: asked once; No -> unchanged; Yes -> applied.
         const nlohmann::json testPolicy = nlohmann::json::parse(
            "{\"deny\":{},\"confirmAlways\":{},\"reviewedSafe\":{},\"fileTables\":{},\"confirmWhen\":{\"PixelMath\":"
            "[{\"parameter\":\"expression\",\"equals\":\"$T*0.5\",\"reason\":\"self-test confirm rule\"}]}}" );
         SetProcessSafetyPolicyForSelfTest( &testPolicy );
         const double before = Inc5Median( v, 0 );
         const ToolCall halve{ "s2", "apply_process", { { "process_id", "PixelMath" }, { "parameters", { { "expression", "$T*0.5" } } } } };
         answer = false;
         const ToolOutcome no = ExecuteTool( halve, ctx );
         const double afterNo = Inc5Median( v, 0 );
         const int callsNo = confirmCalls;
         const String changesNo = lastChanges;
         answer = true;
         const ToolOutcome yes = ExecuteTool( halve, ctx );
         const double afterYes = Inc5Median( v, 0 );
         ctx.mode = AgentMode::Guided;   // Guided + a confirm rule: still ONE dialog
         const ToolOutcome guided = ExecuteTool( halve, ctx );
         SetProcessSafetyPolicyForSelfTest( nullptr );
         detail["confirm"] = { { "callsNo", callsNo }, { "calls", confirmCalls }, { "changes", U8( changesNo ) },
                               { "before", before }, { "afterNo", afterNo }, { "afterYes", afterYes } };
         confirmToolOk = no.isError && callsNo == 1 && changesNo.Contains( "self-test confirm rule" )
                      && std::fabs( afterNo - before ) < 1e-6
                      && !yes.isError && std::fabs( afterYes - 0.5*before ) < 1e-5
                      && !guided.isError && confirmCalls == 3;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      SetProcessSafetyPolicyForSelfTest( nullptr );

      // ---- fix round 1: unreviewed processes, trigger text, alias/enum/unknown-rule hardening ----
      bool unreviewedOk = false, triggerOk = false, enumOk = false, aliasOk = false, enumAliasOk = false,
           unknownRuleOk = false;
      try
      {
         const SafetyVerdict iiDrz = CheckProcessSafety( "ImageIntegration", { { "generateDrizzleData", true } }, nlohmann::json() );

         // A process in NO section with file/output-like ids asks at runtime.
         nlohmann::json noReview = CompiledProcessSafety();
         noReview["reviewedSafe"] = nlohmann::json::object();
         SetProcessSafetyPolicyForSelfTest( &noReview );
         const SafetyVerdict pmU = CheckProcessSafety( "PixelMath", { { "expression", "$T" } }, nlohmann::json() );
         const SafetyVerdict ctU = CheckProcessSafety( "CurvesTransformation", nlohmann::json::object(), nlohmann::json() );
         SetProcessSafetyPolicyForSelfTest( nullptr );
         detail["unreviewed"] = { { "pixelMath", U8( pmU.reason ) }, { "curvesKind", int( ctU.kind ) } };
         unreviewedOk = pmU.kind == SafetyVerdict::Confirm
                     && pmU.reason.StartsWith( "it has not been reviewed and has file/output-like parameters: " )
                     && pmU.reason.Contains( "outputData" )
                     && ctU.kind == SafetyVerdict::Allow;

         nlohmann::json rules = nlohmann::json::parse(
            "{\"deny\":{},\"confirmAlways\":{},\"reviewedSafe\":{},\"fileTables\":{},\"confirmWhen\":{"
            "\"PixelMath\":[{\"parameter\":\"generateOutput\",\"equals\":true,\"reason\":\"default rule\"}],"
            "\"SCNR\":[{\"parameter\":\"colorToRemove\",\"equals\":\"Blue\",\"reason\":\"enum rule\"}],"
            "\"CurvesTransformation\":[{\"parameter\":\"noSuchParameter\",\"equals\":true,\"reason\":\"x\"}]}}" );

         // Find a scalar parameter with an alias id, and an enumeration element with an alias (native lists).
         std::string aliasProc, aliasParam, aliasId, enumProc, enumParam, enumId, enumAlias;
         nlohmann::json aliasValue;
         for ( const Process& P : Process::AllProcesses() )
         {
            if ( !aliasProc.empty() && !enumProc.empty() )
               break;
            const std::string pid( P.Id().c_str() );
            if ( pid == "PixelMath" || pid == "SCNR" || pid == "CurvesTransformation" )
               continue;
            for ( const ProcessParameter& p : P.Parameters() )
            {
               if ( p.IsTable() || p.IsReadOnly() )
                  continue;
               if ( aliasProc.empty() && (p.IsBoolean() || p.IsNumeric()) )
               {
                  const IsoStringList aliases = p.Aliases();
                  if ( !aliases.IsEmpty() && !aliases[0].Trimmed().IsEmpty() )
                  {
                     ProcessInstance d( P );
                     const Variant def = d.ParameterValue( p, 0 );
                     aliasProc = pid;
                     aliasParam = p.Id().c_str();
                     aliasId = aliases[0].Trimmed().c_str();
                     aliasValue = p.IsBoolean() ? nlohmann::json( !def.ToBoolean() ) : nlohmann::json( def.ToDouble() + 1 );
                  }
               }
               if ( enumProc.empty() && p.IsEnumeration() )
                  try
                  {
                     for ( const ProcessParameter::EnumerationElement& e : p.EnumerationElements() )
                        if ( !e.aliases.IsEmpty() && !e.aliases[0].Trimmed().IsEmpty() )
                        {
                           enumProc = pid;
                           enumParam = p.Id().c_str();
                           enumId = e.id.c_str();
                           enumAlias = e.aliases[0].Trimmed().c_str();
                           break;
                        }
                  }
                  catch ( ... )
                  {
                  }
            }
         }
         detail["aliasFound"] = { { "process", aliasProc }, { "parameter", aliasParam }, { "alias", aliasId },
                                  { "value", aliasValue } };
         detail["enumAliasFound"] = { { "process", enumProc }, { "parameter", enumParam }, { "id", enumId },
                                      { "alias", enumAlias } };
         if ( !aliasProc.empty() )
            rules["confirmWhen"][aliasProc] = { { { "parameter", aliasParam }, { "equals", aliasValue }, { "reason", "alias rule" } } };
         if ( !enumProc.empty() && enumProc != aliasProc )
            rules["confirmWhen"][enumProc] = { { { "parameter", enumParam }, { "equals", enumId }, { "reason", "enum alias rule" } } };

         SetProcessSafetyPolicyForSelfTest( &rules );
         const SafetyVerdict pmDef = CheckProcessSafety( "PixelMath", nlohmann::json::object(), nlohmann::json() );
         const SafetyVerdict scInt = CheckProcessSafety( "SCNR", { { "colorToRemove", 2 } }, nlohmann::json() );
         const SafetyVerdict scRed = CheckProcessSafety( "SCNR", { { "colorToRemove", "Red" } }, nlohmann::json() );
         const SafetyVerdict ctBad = CheckProcessSafety( "CurvesTransformation", nlohmann::json::object(), nlohmann::json() );
         SafetyVerdict alViaAlias, alDefault, enViaAlias;
         if ( !aliasProc.empty() )
         {
            alViaAlias = CheckProcessSafety( IsoString( aliasProc.c_str() ), { { aliasId, aliasValue } }, nlohmann::json() );
            alDefault = CheckProcessSafety( IsoString( aliasProc.c_str() ), nlohmann::json::object(), nlohmann::json() );
         }
         if ( !enumProc.empty() && enumProc != aliasProc )
            enViaAlias = CheckProcessSafety( IsoString( enumProc.c_str() ), { { enumParam, enumAlias } }, nlohmann::json() );
         SetProcessSafetyPolicyForSelfTest( nullptr );

         detail["hardening"] = { { "iiDrz", U8( iiDrz.reason ) }, { "pmDefault", U8( pmDef.reason ) },
                                 { "scnrInt", U8( scInt.reason ) }, { "scnrRedKind", int( scRed.kind ) },
                                 { "unknownRule", U8( ctBad.reason ) }, { "aliasVia", U8( alViaAlias.reason ) },
                                 { "aliasDefaultKind", int( alDefault.kind ) }, { "enumAliasVia", U8( enViaAlias.reason ) } };
         triggerOk = iiDrz.reason.Contains( "(generateDrizzleData = true)" )
                  && pmDef.kind == SafetyVerdict::Confirm && pmDef.reason.Contains( "(generateOutput = true, the process default)" );
         enumOk = scInt.kind == SafetyVerdict::Confirm && scInt.reason.Contains( "(colorToRemove = \"Blue\")" )
               && scRed.kind == SafetyVerdict::Allow;
         unknownRuleOk = ctBad.kind == SafetyVerdict::Confirm && ctBad.reason.Contains( "unknown parameter CurvesTransformation.noSuchParameter" );
         // Every installed module on this PI must offer an alias to test; none found is a failure, not a pass.
         aliasOk = !aliasProc.empty() && alViaAlias.kind == SafetyVerdict::Confirm && alViaAlias.reason.Contains( "alias rule" )
                && alDefault.kind == SafetyVerdict::Allow;
         // No installed enumeration on PI 1.9.5 declares element aliases (the whole
         // catalog is scanned above); then there is nothing to slip past a rule, and
         // detail.enumAliasFound shows the empty search. Any that appear are checked.
         enumAliasOk = enumProc.empty() || (enumProc == aliasProc
                       || (enViaAlias.kind == SafetyVerdict::Confirm && enViaAlias.reason.Contains( String::UTF8ToUTF16( ("\"" + enumId + "\"").c_str() ) )));
      }
      catch ( const pcl::Exception& x ) { error += x.Message(); }
      catch ( const std::exception& x ) { error += String( x.what() ); }
      SetProcessSafetyPolicyForSelfTest( nullptr );
      detail["fix1"] = { { "unreviewed", unreviewedOk }, { "trigger", triggerOk }, { "enum", enumOk }, { "alias", aliasOk },
                         { "enumAlias", enumAliasOk }, { "unknownRule", unknownRuleOk } };

      const bool ok = coverageOk && idsOk && verdictOk && denyToolOk && confirmToolOk
                   && unreviewedOk && triggerOk && enumOk && aliasOk && enumAliasOk && unknownRuleOk;
      out["processSafetyDetail"] = detail;
      out["processSafetyError"] = U8( error );
      out["processSafetyOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B7: global processes / run_global_process (Task 8) --------------
   {
      bool precheckOk = true, runOk = false, guidedOk = false, advisorOk = false, safetyOk = false, denyOk = false,
           schemaOk = false, redirectOk = false, cleanupOk = false;
      nlohmann::json detail = nlohmann::json::object();
      std::vector<std::string> created;
      String error, tempDir;
      try
      {
         SyntheticFrames frames( 3 );
         tempDir = frames.Dir();
         auto rows = [&]( int enabled ) {
            nlohmann::json r = nlohmann::json::array();
            for ( size_type i = 0; i < frames.Paths().Length(); ++i )
               r.push_back( nlohmann::json::array( { int( i ) < enabled, U8( frames.Paths()[i] ), "", "" } ) );
            return r;
         };
         const String notImage = frames.AddFile( "notes.txt", "hello" );
         const std::string frame0 = U8( frames.Paths()[0] );
         const std::string dir8 = U8( frames.Dir() );
         // Links, an unreadable directory and upper-case copies. Declared after
         // `frames`, so it is torn down first (the links are not followed; the
         // locked directory gets its permissions back before removal).
         struct FsExtras
         {
            std::vector<std::string> paths;
            std::string              locked;
            ~FsExtras()
            {
               std::error_code ec;
               if ( !locked.empty() )
               {
                  std::filesystem::permissions( locked, std::filesystem::perms::owner_all, ec );
                  std::filesystem::remove_all( locked, ec );
               }
               for ( const std::string& p : paths )
                  std::filesystem::remove( p, ec );
            }
         } fsx;
         const std::string linked = dir8 + "/linked_frame.fits";
         std::filesystem::create_symlink( frame0, linked );
         fsx.paths.push_back( linked );
         const std::string dangling = dir8 + "/dangling.fits";
         std::filesystem::create_symlink( "/nonexistent/picopilot/gone.fits", dangling );
         fsx.paths.push_back( dangling );
         fsx.locked = dir8 + "/locked";
         std::filesystem::create_directory( fsx.locked );
         const std::string lockedFrame = fsx.locked + "/inside.fits";
         std::filesystem::copy_file( frame0, lockedFrame );
         std::filesystem::permissions( fsx.locked, std::filesystem::perms::none );
         const std::string upperFits = dir8 + "/UPPER_01.FITS", upperFit = dir8 + "/UPPER_02.FIT";
         std::filesystem::copy_file( frame0, upperFits );
         std::filesystem::copy_file( frame0, upperFit );
         fsx.paths.push_back( upperFits );
         fsx.paths.push_back( upperFit );
         auto three = [&]( const std::string& first ) {
            nlohmann::json r = rows( 3 );
            r[0][1] = first;
            return r;
         };
         struct Case { const char* name; IsoString process; nlohmann::json params; nlohmann::json tables; std::string expect; };
         const std::vector<Case> cases = {
            { "noTable", "ImageIntegration", nlohmann::json::object(), nlohmann::json::object(),
              "ImageIntegration.images is required: pass table_parameters.images as rows [enabled, path, drizzlePath, "
              "localNormalizationDataPath]" },
            { "relative", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, "light_01.fits", "", "" }, { true, "b.fits", "", "" }, { true, "c.fits", "", "" } } } },
              "ImageIntegration.images[0].path: 'light_01.fits' is not an absolute path" },
            { "tilde", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, "~/a.fits", "", "" } } } },
              "ImageIntegration.images[0].path: '~/a.fits': use an absolute path (PixInsight does not expand ~)" },
            { "missing", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, "/nonexistent/picopilot/a.fits", "", "" } } } },
              "ImageIntegration.images[0].path: '/nonexistent/picopilot/a.fits' does not exist" },
            { "directory", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, dir8, "", "" } } } },
              "ImageIntegration.images[0].path: '" + dir8 + "' is not a file" },
            { "notImage", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, U8( notImage ), "", "" } } } },
              "ImageIntegration.images[0].path: '" + U8( notImage ) + "' is not an image file PixInsight can read (.txt)" },
            { "dangling", "ImageIntegration", nlohmann::json::object(), { { "images", three( dangling ) } },
              "ImageIntegration.images[0].path: '" + dangling + "' is a broken symbolic link" },
            { "danglingOptional", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, frame0, dangling, "" } } } },
              "ImageIntegration.images[0].drizzlePath: '" + dangling + "' is a broken symbolic link" },
            { "danglingLoose", "ImageIntegration", { { "csvWeights", dangling } }, { { "images", rows( 3 ) } },
              "ImageIntegration.csvWeights: '" + dangling + "' is a broken symbolic link" },
            { "noAccess", "ImageIntegration", nlohmann::json::object(), { { "images", three( lockedFrame ) } },
              "ImageIntegration.images[0].path: '" + lockedFrame + "' cannot be accessed (Permission denied)" },
            { "noAccessLoose", "ImageIntegration", { { "csvWeights", lockedFrame } }, { { "images", rows( 3 ) } },
              "ImageIntegration.csvWeights: '" + lockedFrame + "' cannot be accessed (Permission denied)" },
            // Dry-run SetParameters: shape / enum errors come back before any confirm dialog.
            { "dryRunEnum", "ImageIntegration", { { "weightMode", "NoSuchMode" } }, { { "images", rows( 3 ) } },
              "ImageIntegration.weightMode: 'NoSuchMode' is not a valid value; use one of: " },
            { "dryRunShape", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, frame0, "" }, { true, frame0, "", "" }, { true, frame0, "", "" } } } },
              "ImageIntegration.images: row 0 has 3 values; expected 4 (columns: enabled, path, drizzlePath, "
              "localNormalizationDataPath)" },
            { "dryRunUnknownParam", "ImageIntegration", { { "noSuchParam", 1 } }, { { "images", rows( 3 ) } },
              "unknown parameter ImageIntegration.noSuchParam; call describe_process for the valid parameter ids" },
            { "optionalMissing", "ImageIntegration", nlohmann::json::object(),
              { { "images", { { true, frame0, "/nonexistent/picopilot/a.xdrz", "" } } } },
              "ImageIntegration.images[0].drizzlePath: '/nonexistent/picopilot/a.xdrz' does not exist" },
            { "tooFew", "ImageIntegration", nlohmann::json::object(), { { "images", rows( 2 ) } },
              "ImageIntegration.images: 2 enabled rows; at least 3 are required" },
            { "looseMissing", "ImageIntegration", { { "csvWeights", "/nonexistent/picopilot/w.csv" } },
              { { "images", rows( 3 ) } },
              "ImageIntegration.csvWeights: '/nonexistent/picopilot/w.csv' does not exist" },
            { "looseTilde", "ImageIntegration", { { "csvWeights", "~/w.csv" } }, { { "images", rows( 3 ) } },
              "ImageIntegration.csvWeights: '~/w.csv': use an absolute path (PixInsight does not expand ~)" },
            { "unknown", "NoSuchProcessXYZ", nlohmann::json::object(), nlohmann::json::object(),
              "unknown process id 'NoSuchProcessXYZ'; call list_processes for valid ids" },
         };
         nlohmann::json pc = nlohmann::json::array();
         for ( const Case& c : cases )
         {
            const String e = PrecheckGlobalRun( c.process, c.params, c.tables );
            // Full-string match, except the enum case (its element list is the core's).
            const String want = String::UTF8ToUTF16( c.expect.c_str() );
            const bool pass = std::string( c.name ) == "dryRunEnum" ? e.StartsWith( want ) : e == want;
            pc.push_back( { { "case", c.name }, { "pass", pass }, { "error", U8( e ) } } );
            precheckOk = precheckOk && pass;
         }
         // Disabled rows are not checked (a missing file in a disabled row is fine)...
         nlohmann::json mixed = rows( 3 );
         mixed.push_back( nlohmann::json::array( { false, "/nonexistent/picopilot/off.fits", "", "" } ) );
         const String okMixed = PrecheckGlobalRun( "ImageIntegration", nlohmann::json::object(), { { "images", mixed } } );
         const String okThree = PrecheckGlobalRun( "ImageIntegration", nlohmann::json::object(), { { "images", rows( 3 ) } } );
         pc.push_back( { { "case", "valid3" }, { "error", U8( okThree ) } } );
         pc.push_back( { { "case", "disabledRowIgnored" }, { "error", U8( okMixed ) } } );
         precheckOk = precheckOk && okThree.IsEmpty() && okMixed.IsEmpty();
         // A symlink to a real frame is a frame (NAS-linked folders).
         const String okLink = PrecheckGlobalRun( "ImageIntegration", nlohmann::json::object(), { { "images", three( linked ) } } );
         pc.push_back( { { "case", "symlinkFollowed" }, { "error", U8( okLink ) } } );
         // Upper-case extensions (.FITS / .FIT) are image files.
         nlohmann::json upper = rows( 3 );
         upper[0][1] = upperFits;
         upper[1][1] = upperFit;
         const String okUpper = PrecheckGlobalRun( "ImageIntegration", nlohmann::json::object(), { { "images", upper } } );
         pc.push_back( { { "case", "upperCaseExtensions" }, { "error", U8( okUpper ) } } );
         nlohmann::json lookup = nlohmann::json::object();
         for ( const char* ext : { ".FITS", ".FIT", ".fits", ".XISF" } )
            try
            {
               const FileFormat f( ext, true/*toRead*/, false/*toWrite*/ );
               lookup[ext] = f.CanRead() ? "found, can read" : "found, cannot read";
            }
            catch ( ... )
            {
               lookup[ext] = "not found";
            }
         detail["formatLookupByExtension"] = lookup;
         precheckOk = precheckOk && okLink.IsEmpty() && okUpper.IsEmpty();
         // Policy integrity: never a silent pass on a bad policy entry.
         const nlohmann::json badEnabled = nlohmann::json::parse(
            R"({"ImageIntegration":{"images":{"enabledColumn":"nope","minEnabledRows":3,"columns":{"path":"image"}}}})" );
         const nlohmann::json badTable = nlohmann::json::parse(
            R"({"ImageIntegration":{"nope":{"enabledColumn":"enabled","columns":{"path":"image"}}}})" );
         const String eEnabled = ValidateGlobalRunFilePaths( "ImageIntegration", badEnabled, nlohmann::json::object(),
                                                             { { "images", rows( 3 ) } } );
         const String eTable = ValidateGlobalRunFilePaths( "ImageIntegration", badTable, nlohmann::json::object(),
                                                           { { "images", rows( 3 ) } } );
         pc.push_back( { { "case", "policyBadEnabledColumn" }, { "error", U8( eEnabled ) } } );
         pc.push_back( { { "case", "policyBadTable" }, { "error", U8( eTable ) } } );
         precheckOk = precheckOk
            && eEnabled == "internal: the file-table policy names nope as the enabled column of ImageIntegration.images, "
                           "which has no such column"
            && eTable == "internal: the file-table policy names ImageIntegration.nope, which is not a table parameter "
                         "of ImageIntegration";
         // Result-window attribution: named outputs are results; other new windows are listed apart.
         {
            std::vector<std::string> res, other;
            SplitNewWindows( { "integration", "rejection_low", "stray" },
                             { { "integrationImageId", "integration" }, { "lowRejectionMapImageId", "rejection_low" } },
                             res, other );
            std::vector<std::string> res2, other2;
            SplitNewWindows( { "a", "b" }, nlohmann::json::object(), res2, other2 );
            const bool splitOk = res == std::vector<std::string>{ "integration", "rejection_low" }
                              && other == std::vector<std::string>{ "stray" }
                              && res2 == std::vector<std::string>{ "a", "b" } && other2.empty();
            pc.push_back( { { "case", "splitNewWindows" }, { "pass", splitOk } } );
            precheckOk = precheckOk && splitOk;
         }
         // A view-only process, if this PI has one (most processes default to global-capable).
         for ( const Process& P : Process::AllProcesses() )
            if ( !P.CanProcessGlobal() )
            {
               const String e = PrecheckGlobalRun( P.Id(), nlohmann::json::object(), nlohmann::json::object() );
               detail["viewOnly"] = { { "process", std::string( P.Id().c_str() ) }, { "error", U8( e ) } };
               precheckOk = precheckOk && e.Contains( "cannot run in the global context" );
               break;
            }
         detail["precheck"] = pc;

         int confirmCalls = 0;
         bool answer = false;
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         String lastChanges;
         ctx.confirm = [&]( const String&, const String&, const String& changes ) { ++confirmCalls; lastChanges = changes; return answer; };
         const nlohmann::json input = { { "process_id", "ImageIntegration" },
                                        { "parameters", { { "weightMode", "DontCare" } } },
                                        { "table_parameters", { { "images", rows( 3 ) } } } };

         // A precheck failure through the tool: precise message, nothing asked, nothing opened.
         const std::set<std::string> b0 = OpenMainViewIds();
         nlohmann::json bad = input;
         bad["table_parameters"]["images"] = rows( 2 );
         const ToolOutcome badOut = ExecuteTool( ToolCall{ "g0", "run_global_process", bad }, ctx );
         const bool badOk = badOut.isError && confirmCalls == 0 && OpenMainViewIds() == b0
                         && badOut.content.at( 0 ).at( "text" ).get<std::string>()
                            == "ImageIntegration.images: 2 enabled rows; at least 3 are required";
         detail["toolPrecheck"] = { { "ok", badOk }, { "error", badOut.content.at( 0 ).at( "text" ) } };

         // Copilot: runs without asking; the result windows come back.
         const std::set<std::string> before = OpenMainViewIds();
         const ToolOutcome o = ExecuteTool( ToolCall{ "g1", "run_global_process", input }, ctx );
         for ( const std::string& id : OpenMainViewIds() )
            if ( before.count( id ) == 0 )
               created.push_back( id );
         nlohmann::json summary;
         if ( !o.isError )
            summary = nlohmann::json::parse( o.content.at( 0 ).at( "text" ).get<std::string>() );
         int images = 0;
         for ( const nlohmann::json& b : o.content )
            images += b.value( "type", std::string() ) == "image" ? 1 : 0;
         detail["run"] = { { "isError", o.isError }, { "log", U8( o.logLine ) }, { "summary", summary },
                           { "created", created }, { "images", images },
                           { "error", o.isError ? o.content.at( 0 ).at( "text" ) : nlohmann::json() } };
         const std::string integ = summary.value( "outputIds", nlohmann::json::object() ).value( "integrationImageId", std::string() );
         bool statsOk = false;
         if ( !o.isError )
         {
            // The primary result is described first; its statistics are those of the real integration.
            const nlohmann::json& w0 = summary.at( "createdWindows" ).at( 0 );
            const nlohmann::json& ch0 = w0.at( "context" ).at( "channelStats" ).at( 0 );
            const double mean = ch0.at( "mean" ).get<double>();
            detail["run"]["resultMean"] = mean;
            detail["run"]["inputMeanOfMeans"] = frames.MeanOfMeans();
            statsOk = w0.at( "id" ) == integ
                   && w0.at( "context" ).at( "geometry" ).at( "width" ) == kIiW
                   && w0.at( "context" ).at( "geometry" ).at( "height" ) == kIiH
                   && std::fabs( mean - frames.MeanOfMeans() ) < 0.01*frames.MeanOfMeans()
                   && summary.at( "createdWindowCount" ).get<size_t>()
                      + summary.value( "otherNewWindows", nlohmann::json::array() ).size() == created.size();
         }
         runOk = badOk && !o.isError && confirmCalls == 0 && !o.mutated && images == 1 && !integ.empty()
              && std::find( created.begin(), created.end(), integ ) != created.end() && statsOk
              && U8( o.logLine ).rfind( "\xE2\x96\xB6 run_global_process ImageIntegration", 0 ) == 0;

         // Guided: asks; No -> nothing opens.
         ctx.mode = AgentMode::Guided;
         const std::set<std::string> b2 = OpenMainViewIds();
         const ToolOutcome declined = ExecuteTool( ToolCall{ "g2", "run_global_process", input }, ctx );
         guidedOk = declined.isError && confirmCalls == 1 && OpenMainViewIds() == b2
                 && declined.content.at( 0 ).at( "text" ).get<std::string>().find( "declined" ) != std::string::npos;

         // Guided: a parameter error comes back BEFORE the confirm dialog (dry run).
         nlohmann::json badEnum = input;
         badEnum["parameters"]["weightMode"] = "NoSuchMode";
         const ToolOutcome enumOut = ExecuteTool( ToolCall{ "g3", "run_global_process", badEnum }, ctx );
         guidedOk = guidedOk && enumOut.isError && confirmCalls == 1
                 && enumOut.content.at( 0 ).at( "text" ).get<std::string>().rfind( "ImageIntegration.weightMode: 'NoSuchMode'", 0 ) == 0;

         // Copilot + a policy rule (generateDrizzleData would update .xdrz files
         // next to the frames): the shared gate asks even in Copilot; No -> nothing runs.
         ctx.mode = AgentMode::Copilot;
         nlohmann::json drz = input;
         drz["parameters"]["generateDrizzleData"] = true;
         const ToolOutcome drzOut = ExecuteTool( ToolCall{ "g6", "run_global_process", drz }, ctx );
         detail["safetyConfirm"] = { { "changes", U8( lastChanges ) }, { "result", drzOut.content.at( 0 ).at( "text" ) } };
         safetyOk = drzOut.isError && confirmCalls == 2 && OpenMainViewIds() == b2
                 && lastChanges.StartsWith( "Why you are asked: " )
                 && lastChanges.Contains( "(generateDrizzleData = true)" )
                 && drzOut.content.at( 0 ).at( "text" ).get<std::string>()
                    == "The user declined this run_global_process call (ImageIntegration). Nothing was run. "
                       "Do not repeat it; ask what they would like instead.";

         // Deny: a precise error, no dialog, nothing runs (PICopilot is global-capable and denied).
         const ToolOutcome denied = ExecuteTool( ToolCall{ "g7", "run_global_process", { { "process_id", "PICopilot" } } }, ctx );
         detail["deny"] = denied.content.at( 0 ).at( "text" );
         denyOk = denied.isError && confirmCalls == 2 && OpenMainViewIds() == b2
               && denied.content.at( 0 ).at( "text" ).get<std::string>().rfind( "PICopilot is not allowed from PI Copilot: ", 0 ) == 0
               && denied.content.at( 0 ).at( "text" ).get<std::string>().find( "Give the user the settings so they can run it themselves." )
                  != std::string::npos;

         // Advisor: not offered, refused if called anyway.
         ctx.mode = AgentMode::Advisor;
         const ToolOutcome adv = ExecuteTool( ToolCall{ "g4", "run_global_process", input }, ctx );
         advisorOk = adv.isError && adv.content.at( 0 ).at( "text" ).get<std::string>().find( "not available in Advisor" ) != std::string::npos
                  && confirmCalls == 2 && OpenMainViewIds() == b2;

         bool inCopilot = false, inGuided = false, inAdvisor = false;
         for ( const nlohmann::json& t : ToolDefinitions( AgentMode::Copilot ) )
            inCopilot = inCopilot || t.at( "name" ) == "run_global_process";
         for ( const nlohmann::json& t : ToolDefinitions( AgentMode::Guided ) )
            inGuided = inGuided || t.at( "name" ) == "run_global_process";
         for ( const nlohmann::json& t : ToolDefinitions( AgentMode::Advisor ) )
            inAdvisor = inAdvisor || t.at( "name" ) == "run_global_process";
         schemaOk = inCopilot && inGuided && !inAdvisor
                 && BuildSystemPrompt( AgentMode::Copilot ).Contains( "run_global_process" )
                 && BuildSystemPrompt( AgentMode::Guided ).Contains( "run_global_process" )
                 && !BuildSystemPrompt( AgentMode::Advisor ).Contains( "run_global_process" );

         // apply_process on a file-list process points to run_global_process.
         Inc5TestWindow tw( "PCRedirectT", 32, 32, 1, 0.3 );
         ctx.mode = AgentMode::Copilot;
         ctx.turnViewId = tw.MainView().FullId();
         const ToolOutcome red = ExecuteTool( ToolCall{ "g5", "apply_process", { { "process_id", "ImageIntegration" } } }, ctx );
         detail["redirect"] = red.content.at( 0 ).at( "text" );
         redirectOk = red.isError && !red.mutated
                   && red.content.at( 0 ).at( "text" ).get<std::string>()
                      == "ImageIntegration integrates files from disk, not an open image: use run_global_process with "
                         "the file list in table_parameters";
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }
      ForceCloseWindows( created );

      // Cleanup: temp files gone, created windows closed.
      const std::set<std::string> after = OpenMainViewIds();
      bool windowsGone = true;
      for ( const std::string& id : created )
         windowsGone = windowsGone && after.count( id ) == 0;
      cleanupOk = !tempDir.IsEmpty() && !File::DirectoryExists( tempDir ) && windowsGone;
      detail["cleanup"] = { { "tempDirGone", !tempDir.IsEmpty() && !File::DirectoryExists( tempDir ) },
                            { "windowsGone", windowsGone } };
      detail["checks"] = { { "precheck", precheckOk }, { "run", runOk }, { "guided", guidedOk },
                           { "safety", safetyOk }, { "deny", denyOk },
                           { "advisor", advisorOk }, { "schema", schemaOk }, { "redirect", redirectOk },
                           { "cleanup", cleanupOk } };

      const bool ok = precheckOk && runOk && guidedOk && safetyOk && denyOk && advisorOk && schemaOk && redirectOk
                   && cleanupOk;
      out["globalDetail"] = detail;
      out["globalError"] = U8( error );
      out["globalProcessOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B8: run_pjsr (Task 9) ------------------------------------------
   {
      bool checkOk = false, runOk = false, errorOk = false, boundOk = false, valueBoundOk = false, pixelOk = false,
           offOk = false, declineOk = false, approveOk = false, guidedOk = false, syntaxNoDialogOk = false,
           advisorOk = false, tooLongOk = false, schemaOk = false, promptOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         // Task 1 finding: a new Function() SyntaxError carries NO line under
         // EvaluateScript, so the check reports line 0 (unknown), never a guess.
         const PjsrCheck good = CheckPjsrSyntax( "var a = 1;\nreturn a + 2;" );
         const PjsrCheck bad = CheckPjsrSyntax( "var a = 1;\nvar b = ;\n" );
         detail["bad"] = { { "error", U8( bad.error ) }, { "line", bad.line }, { "offset", PjsrLineOffset() } };
         checkOk = good.ok && !bad.ok && bad.line == 0 && bad.error.StartsWith( "SyntaxError" )
                && PjsrLineOffset() == 2;   // Task 1: the synthesized header is 2 lines

         const PjsrRun r1 = RunPjsr( "console.writeln( \"hi pc\" );\nreturn { a: 1, t: targetViewId };", "PCPjsrT" );
         // Only what the script wrote: the core's log banner and per-line
         // timestamps are stripped; write() without a newline joins the line.
         const PjsrRun quiet = RunPjsr( "return 0;", IsoString() );
         const PjsrRun lines = RunPjsr( "console.writeln( 'L1' ); console.write( 'L2a' ); console.writeln( 'L2b' );"
                                        " console.warningln( 'W' ); return 0;", IsoString() );
         detail["r1"] = { { "value", U8( r1.value ) }, { "console", U8( r1.console ) }, { "error", U8( r1.error ) },
                          { "quietConsole", U8( quiet.console ) }, { "linesConsole", U8( lines.console ) } };
         runOk = r1.ok && r1.value == "{\"a\":1,\"t\":\"PCPjsrT\"}" && r1.console == "hi pc\n"
              && quiet.ok && quiet.console.IsEmpty() && lines.console == "L1\nL2aL2b\nW\n";

         // Runtime errors DO have a position: reported in the model's own lines,
         // including a throw from inside a nested function.
         const PjsrRun r2 = RunPjsr( "var a = 1;\nthrow new Error( \"pc-script-fail\" );", IsoString() );
         const PjsrRun r2b = RunPjsr( "function f() {\n   return null.x;\n}\nvar q = 0;\nreturn f();", IsoString() );
         detail["r2"] = { { "error", U8( r2.error ) }, { "line", r2.line },
                          { "nestedError", U8( r2b.error ) }, { "nestedLine", r2b.line } };
         errorOk = !r2.ok && r2.line == 2 && r2.error.Contains( "pc-script-fail" ) && !r2b.ok && r2b.line == 2;

         const PjsrRun r3 = RunPjsr( "for ( var i = 0; i < 400; ++i ) console.writeln( \"0123456789012345678901234567890123456789012345678\" );\n"
                                     "console.writeln( \"pc-last-line-\\u00e9\" );\nreturn \"x\";", IsoString() );
         detail["r3"] = { { "consoleLength", r3.console.Length() }, { "head", U8( r3.console.Left( 60 ) ) },
                          { "tail", U8( r3.console.Right( 40 ) ) }, { "value", U8( r3.value ) }, { "error", U8( r3.error ) } };
         boundOk = r3.ok && r3.consoleTruncated && r3.console.StartsWith( "[... " )
                && r3.console.Length() <= PICopilotMaxScriptConsoleChars + 64 && r3.value == "\"x\""
                && r3.console.Contains( String::UTF8ToUTF16( "pc-last-line-\xC3\xA9" ) );   // the TAIL is kept, UTF-8 intact

         const PjsrRun r5 = RunPjsr( "var s = \"\";\nfor ( var i = 0; i < 5000; ++i ) s += \"a\";\nreturn s;", IsoString() );
         detail["r5"] = { { "valueLength", r5.value.Length() }, { "valueTruncated", r5.valueTruncated }, { "error", U8( r5.error ) } };
         valueBoundOk = r5.ok && r5.valueTruncated && r5.value.Length() == PICopilotMaxScriptValueChars;

         Inc5TestWindow tw( "PCPjsrPix", 32, 32, 1, 0.4 );
         View v = tw.MainView();
         // The EvaluateScript engine (PI 1.9.5) namespaces the constants:
         // UndoFlag.PixelData / ImageOp.Mul (UndoFlag_PixelData is undefined
         // there). The prompt teaches exactly this spelling, and the change
         // it makes is really undoable: the window's undo() restores the
         // original pixels.
         const PjsrRun r4 = RunPjsr( "var v = View.viewById( targetViewId );\nv.beginProcess( UndoFlag.PixelData );\n"
                                     "v.image.apply( 0.5, ImageOp.Mul );\nv.endProcess();\nreturn v.image.median();", v.FullId() );
         const double afterRun = Inc5Median( v, 0 );
         EvalJs( "(function(){ ImageWindow.windowById( \"PCPjsrPix\" ).undo(); return 0; })()" );
         const double afterUndo = Inc5Median( v, 0 );
         detail["r4"] = { { "value", U8( r4.value ) }, { "error", U8( r4.error ) },
                          { "afterRun", afterRun }, { "afterUndo", afterUndo } };
         pixelOk = r4.ok && std::fabs( afterRun - 0.2 ) < 1e-6 && std::fabs( afterUndo - 0.4 ) < 1e-6;

         int asks = 0;
         bool answer = false;
         String askedCode;
         ToolContext ctx;
         ctx.mode = AgentMode::Copilot;
         ctx.turnViewId = v.FullId();
         ctx.confirmScript = [&]( const String&, const String& code, const IsoString& ) { ++asks; askedCode = code; return answer; };
         const std::string markCode = "new ImageWindow( 8, 8, 1, 32, true, false, \"PCPjsrMark\" );\nreturn 1;";
         const ToolCall mark{ "p1", "run_pjsr", { { "purpose", "Create a marker window" }, { "code", markCode } } };
         auto markExists = []() { return !ImageWindow::WindowById( IsoString( "PCPjsrMark" ) ).IsNull(); };
         auto text0 = []( const ToolOutcome& o ) { return o.content.at( 0 ).at( "text" ).get<std::string>(); };

         ctx.runPjsr = false;
         const ToolOutcome off = ExecuteTool( mark, ctx );
         offOk = off.isError && asks == 0 && !markExists() && text0( off ).find( "turned off" ) != std::string::npos;

         ctx.runPjsr = true;
         answer = false;
         const ToolOutcome declined = ExecuteTool( mark, ctx );
         declineOk = declined.isError && asks == 1 && U8( askedCode ) == markCode && !markExists() && !declined.mutated;

         answer = true;
         const ToolOutcome approved = ExecuteTool( mark, ctx );
         detail["approved"] = text0( approved );
         approveOk = !approved.isError && asks == 2 && markExists() && approved.mutated;
         ForceCloseWindows( { "PCPjsrMark" } );

         ctx.mode = AgentMode::Guided;   // asked in Guided too (and in Copilot above): every time
         answer = false;
         ExecuteTool( mark, ctx );
         guidedOk = asks == 3 && !markExists();

         ctx.mode = AgentMode::Copilot;
         const ToolOutcome syn = ExecuteTool( ToolCall{ "p2", "run_pjsr", { { "purpose", "broken" }, { "code", "var = ;" } } }, ctx );
         detail["syntax"] = text0( syn );
         syntaxNoDialogOk = syn.isError && asks == 3 && text0( syn ).find( "syntax error (line unknown)" ) != std::string::npos;

         const ToolOutcome longer = ExecuteTool( ToolCall{ "p3", "run_pjsr", { { "purpose", "too long" },
                                                 { "code", "return 1;" + std::string( PICopilotMaxScriptChars, ' ' ) } } }, ctx );
         detail["tooLong"] = text0( longer );
         tooLongOk = longer.isError && asks == 3 && text0( longer ).find( "the limit is 20000" ) != std::string::npos;

         ctx.mode = AgentMode::Advisor;
         const ToolOutcome adv = ExecuteTool( mark, ctx );
         advisorOk = adv.isError && asks == 3 && text0( adv ).find( "not available in Advisor" ) != std::string::npos;

         ToolOptions on;
         on.runPjsr = true;
         const nlohmann::json tOn = ToolDefinitions( AgentMode::Copilot, on );
         const nlohmann::json tOff = ToolDefinitions( AgentMode::Copilot );
         const nlohmann::json tAdv = ToolDefinitions( AgentMode::Advisor, on );
         bool offHas = false, advHas = false;
         for ( const nlohmann::json& t : tOff ) offHas = offHas || t.at( "name" ) == "run_pjsr";
         for ( const nlohmann::json& t : tAdv ) advHas = advHas || t.at( "name" ) == "run_pjsr";
         schemaOk = tOn.back().at( "name" ) == "run_pjsr"
                 && tOn.back().at( "input_schema" ).at( "required" ) == nlohmann::json::array( { "code", "purpose" } )
                 && !offHas && !advHas && ToolDefinitions( AgentMode::Guided, on ).back().at( "name" ) == "run_pjsr";
         promptOk = BuildSystemPrompt( AgentMode::Copilot, on ).Contains( "run_pjsr" )
                 && BuildSystemPrompt( AgentMode::Copilot, on ).Contains( "view.beginProcess(UndoFlag.PixelData)" )
                 && BuildSystemPrompt( AgentMode::Guided, on ).Contains( "run_pjsr" )
                 && !BuildSystemPrompt( AgentMode::Copilot ).Contains( "run_pjsr" )
                 && !BuildSystemPrompt( AgentMode::Advisor, on ).Contains( "run_pjsr" )
                 // folded inc-4 minor: assert the literal mode-switch phrase, not the ever-present "PI Copilot"
                 && BuildSystemPrompt( AgentMode::Advisor ).Contains( "switching the mode selector to Copilot" );
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      ForceCloseWindows( { "PCPjsrMark" } );

      detail["flags"] = { { "check", checkOk }, { "run", runOk }, { "error", errorOk }, { "bound", boundOk },
                          { "valueBound", valueBoundOk }, { "pixel", pixelOk }, { "off", offOk }, { "decline", declineOk },
                          { "approve", approveOk }, { "guided", guidedOk }, { "syntaxNoDialog", syntaxNoDialogOk },
                          { "tooLong", tooLongOk }, { "advisor", advisorOk }, { "schema", schemaOk }, { "prompt", promptOk } };
      const bool ok = checkOk && runOk && errorOk && boundOk && valueBoundOk && pixelOk && offOk && declineOk
                   && approveOk && guidedOk && syntaxNoDialogOk && tooLongOk && advisorOk && schemaOk && promptOk;
      out["runPjsrDetail"] = detail;
      out["runPjsrError"] = U8( error );
      out["runPjsrOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section B8a: run_pjsr breakout suite (Task 9) ---------------------------
   // The model's script reaches the engine ONLY as data (ScriptLiteral). Each
   // hostile text must either parse as exactly its own source, or fail as a
   // SyntaxError -- and the sentinel (a window named PCBreakout) must NEVER
   // appear during the parse check.
   {
      bool allCasesOk = true;
      nlohmann::json cases = nlohmann::json::array();
      String error;
      try
      {
         auto u16 = []( std::initializer_list<unsigned> units )
         {
            String s;
            for ( unsigned u : units )
               s += char16_type( u );
            return s;
         };
         // UTF-16 code units as "u,u,u" (a lone surrogate is not valid text: it travels as U+FFFD).
         auto codes = []( const String& s )
         {
            String r;
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
               r += String().Format( i ? ",%u" : "%u", u );
            }
            return r;
         };
         const char* const kCodesJs = "; var r = \"\"; for ( var i = 0; i < s.length; ++i ) r += (i ? \",\" : \"\") + s.charCodeAt( i ); return r; })()";
         auto sentinelExists = []() { return !ImageWindow::WindowById( IsoString( "PCBreakout" ) ).IsNull(); };

         const String sentinel = "new ImageWindow( 8, 8, 1, 32, true, false, \"PCBreakout\" );";
         struct BreakoutCase { const char* name; String code; };
         const std::vector<BreakoutCase> list = {
            { "quote",         "var s = \"a\\\"b\"; return s; \" " + sentinel },
            { "backslash",     "return \"\\\\\"; \\\" " + sentinel },
            { "commentClose",  "*/ " + sentinel + " /*" },
            { "scriptTag",     "return \"</script><script>" + sentinel + "</script>\";" },
            { "newlines",      "var a = 1;\r\n" + sentinel + "\n\"\n" },
            { "nulControl",    "return \"a" + u16( { 0 } ) + "b" + u16( { 1, 0x1f, 0x7f } ) + "\"; " + sentinel },
            { "lineSep2028",   "// c" + u16( { 0x2028 } ) + sentinel },
            { "paraSep2029",   "return \"p" + u16( { 0x2029 } ) + "\" + (" + sentinel + ")" },
            { "surrogates",    "return \"" + u16( { 0xD83D, 0xDE00 } ) + "\"; // " + sentinel },
            { "loneSurrogate", "return \"" + u16( { 0xD83D } ) + "\"; " + sentinel },
            { "literalEscape", "\"); " + sentinel + " (\"" },
            { "fnClose",       "}); " + sentinel + " (function(){" },
            { "unicodeEscape", "\\u0022); " + sentinel + " //" },
         };
         for ( const BreakoutCase& c : list )
         {
            nlohmann::json j;
            j["name"] = c.name;
            const std::string lit = ScriptLiteral( c.code );
            bool ascii = true;
            for ( unsigned char ch : lit )
               ascii = ascii && ch >= 0x20 && ch < 0x7f;
            j["asciiOnly"] = ascii;

            // Inside the engine the literal is exactly the code, code unit for code unit.
            const String want = codes( c.code );
            const String back = EvalJs( "(function(){ var s = " + String( lit.c_str() ) + kCodesJs );
            j["literalFaithful"] = back == want;

            const PjsrCheck chk = CheckPjsrSyntax( c.code );
            const bool ranOnCheck = sentinelExists();
            ForceCloseWindows( { "PCBreakout" } );
            j["parsed"] = chk.ok;
            j["error"] = U8( chk.error );
            j["sentinelRanOnCheck"] = ranOnCheck;

            bool outcomeOk;
            if ( chk.ok )
            {
               // Parsed: the compiled function's own source holds the code verbatim.
               const String src = EvalJs( "(function(){ var s = String( new Function( \"targetViewId\", "
                                          + String( lit.c_str() ) + " ) )" + kCodesJs );
               outcomeOk = ("," + src + ",").Contains( "," + want + "," );
               j["functionSourceHasCode"] = outcomeOk;
            }
            else
               outcomeOk = chk.error.StartsWith( "SyntaxError" );
            const bool caseOk = ascii && back == want && !ranOnCheck && outcomeOk;
            j["ok"] = caseOk;
            allCasesOk = allCasesOk && caseOk;
            cases.push_back( j );
         }

         // End to end through RunPjsr: non-BMP and NUL survive both ways, and a
         // literal-escape attempt is just a string expression statement.
         const PjsrRun e1 = RunPjsr( "return \"" + u16( { 0xD83D, 0xDE00 } ) + "\" + \"a" + u16( { 0 } ) + "b\".length;", IsoString() );
         const PjsrRun e2 = RunPjsr( "\"); new ImageWindow( 8, 8, 1, 32, true, false, 'PCBreakout' ); (\"\nreturn 7;", IsoString() );
         const bool e2Sentinel = sentinelExists();
         ForceCloseWindows( { "PCBreakout" } );
         const bool endToEndOk = e1.ok && e1.value == "\"" + u16( { 0xD83D, 0xDE00 } ) + "3\""
                              && e2.ok && e2.value == "7" && !e2Sentinel;
         cases.push_back( { { "name", "endToEnd" }, { "e1Value", U8( e1.value ) }, { "e1Error", U8( e1.error ) },
                            { "e2Value", U8( e2.value ) }, { "e2Error", U8( e2.error ) }, { "e2Sentinel", e2Sentinel },
                            { "ok", endToEndOk } } );
         allCasesOk = allCasesOk && endToEndOk;
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); allCasesOk = false; }
      catch ( const std::exception& x ) { error = String( x.what() ); allCasesOk = false; }
      ForceCloseWindows( { "PCBreakout" } );
      out["runPjsrBreakoutCases"] = cases;
      out["runPjsrBreakoutError"] = U8( error );
      out["runPjsrBreakoutOk"] = allCasesOk;
      allOk = allOk && allCasesOk;
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
