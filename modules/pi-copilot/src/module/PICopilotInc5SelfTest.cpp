// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInc5SelfTest.h"
#include "PICopilotModule.h"
#include "ProcessCatalog.h"
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
