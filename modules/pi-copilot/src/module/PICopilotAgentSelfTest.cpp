// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotAgentSelfTest.h"
#include "PICopilotModule.h"
#include "ProcessApply.h"
#include "ProcessCatalog.h"
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Exception.h>
#include <pcl/Image.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/Variant.h>
#include <pcl/View.h>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace pcl
{

namespace
{

// Synthetic agent test image: 400x300 32-bit float RGB, background grey
// gradient (R=G=B), pure-red 100x100 square. Small on purpose -- these
// sections test process execution, not preview downscaling (increment 3 did).
constexpr int kAgW = 400, kAgH = 300;
constexpr int kAgSqX0 = 150, kAgSqY0 = 100, kAgSq = 100;

double AgBackground( int x )
{
   return 0.02 + 0.03*x/(kAgW - 1);
}

// Owns a hidden test window and force-closes it on destruction (ImageWindow
// is only an alias handle; its destructor does not close the core window).
// Not copyable: exactly one owner per window.
class AgentTestWindow
{
public:

   explicit AgentTestWindow( const char* id )
      : m_window( kAgW, kAgH, 3, 32, true/*floatSample*/, true/*color*/,
                  false/*initialProcessing*/, IsoString( id ) )
   {
      if ( m_window.IsNull() )
         throw Error( "AgentTestWindow: ImageWindow construction returned a null window" );
      try
      {
         Fill();
      }
      catch ( ... )
      {
         Close();
         throw;
      }
   }

   ~AgentTestWindow()
   {
      Close();
   }

   AgentTestWindow( const AgentTestWindow& ) = delete;
   AgentTestWindow& operator =( const AgentTestWindow& ) = delete;

   View MainView() const
   {
      return m_window.MainView();
   }

private:

   ImageWindow m_window;

   void Fill()
   {
      View view = m_window.MainView();
      AutoViewLock lock( view );
      ImageVariant v = view.Image();
      if ( !v || !v.IsFloatSample() || v.BitsPerSample() != 32 || v.NumberOfChannels() != 3 )
         throw Error( "AgentTestWindow: not a 32-bit float RGB image" );
      Image& img = static_cast<Image&>( *v );
      for ( int y = 0; y < kAgH; ++y )
         for ( int x = 0; x < kAgW; ++x )
         {
            const bool sq = x >= kAgSqX0 && x < kAgSqX0 + kAgSq && y >= kAgSqY0 && y < kAgSqY0 + kAgSq;
            for ( int c = 0; c < 3; ++c )
               img.Pixel( x, y, c ) = float( sq ? (c == 0 ? 1.0 : 0.0) : AgBackground( x ) );
         }
   }

   void Close()
   {
      try
      {
         if ( !m_window.IsNull() )
            m_window.ForceClose();
      }
      catch ( ... )
      {
      }
   }
};

// Non-blocking busy probe (global constraint: never block on a busy view).
// Mirrors ViewCapture.cpp's CanRead()/CanWrite() check before
// AutoViewWriteLock/ExecuteOn: LockForWrite() on a view already locked by a
// running process HANGS PixInsight rather than failing fast, so the check
// must happen first, every time, not just be "usually fine because the test
// window is fresh." ChannelMedian/SampleAt are shared helpers Tasks 2-7 use
// against instances of this same test window across several process runs,
// and Task 2's production apply_process will copy this exact idiom onto
// real (possibly busy) user views -- fail loudly with a clear, catchable
// error instead of blocking.
bool ViewBusy( View view )
{
   return !view.CanRead() || !view.CanWrite();
}

double ChannelMedian( View view, int c )
{
   if ( ViewBusy( view ) )
      throw Error( "ChannelMedian: view is busy (locked by a running process)" );
   AutoViewWriteLock lock( view );
   ImageVariant v = view.Image();
   return v.Median( v.Bounds(), c, c );
}

// [[maybe_unused]]: not called by Section A0 (Task 1) -- Tasks 2-7 use it to
// verify per-pixel results of PixelMath/SCNR/HistogramTransformation runs.
[[maybe_unused]] float SampleAt( View view, int x, int y, int c )
{
   if ( ViewBusy( view ) )
      throw Error( "SampleAt: view is busy (locked by a running process)" );
   AutoViewWriteLock lock( view );
   ImageVariant v = view.Image();
   if ( !v.IsFloatSample() || v.BitsPerSample() != 32 )
      throw Error( "SampleAt: not a 32-bit float image" );
   return static_cast<const Image&>( *v ).Pixel( x, y, c );
}

} // namespace

// A core-level rejection inside a compiled process's own parameter
// validation (observed live: an invalid row-index argument to
// SetParameterValue() on PixelMath, since fixed below) can pop a real,
// blocking, modal "<Process>: <message>" dialog on whatever X11 DISPLAY this
// headless --automation-mode run inherited, instead of surfacing as a
// catchable pcl::Exception -- SetParameterValue() just returns false. This
// is NOT gated by pcl::Exception::EnableGUIOutput()/DisableGUIOutput(): that
// static flag was tried here first and did NOT suppress the dialog, because
// this module is statically linked against its OWN copy of libPCL-pxi.a,
// so DisableGUIOutput() called from this module only changes OUR copy of
// that flag, not the separate one the CORE application's own compiled
// process implementation consults for its own error reporting. There is no
// module-side switch to reach into the core's dialog. The only reliable
// defence is to never hand the core an invalid call in the first place
// (see the row-index fix below) -- which is also the finding for Task 2's
// production apply_process: validate arguments against the process's own
// declared bounds/types BEFORE calling SetParameterValue()/ExecuteOn(),
// rather than relying on catching whatever comes back afterward.
bool RunAgentSelfTest( nlohmann::json& out )
{
   bool allOk = true;

   // ---- Section A0: native process execution smoke (Task 1) ---------------
   // Proves the primitives ApplyProcess() is built on, from INSIDE this
   // self-test's own ExecuteGlobal() (nested process execution), and records
   // the parameter facts later sections assert.
   {
      bool pmExecOk = false, enumRoundTripOk = false, htTableOk = false, rangeOk = false, globalOnlyOk = false;
      nlohmann::json info = nlohmann::json::object();
      String error;
      try
      {
         AgentTestWindow tw( "PICopilotAgentSmoke" );
         View view = tw.MainView();

         // PixelMath "$T*0.5" on the view: median must halve exactly.
         //
         // Row index: ProcessInstance::ParameterValue()/SetParameterValue()
         // pass rowIndex straight through to the core API
         // (API->Process->SetParameterValue(handle, param, rowIndex, ...) --
         // see ProcessInstance.cpp) with NO special-casing of the ~size_type(0)
         // default for non-table parameters. Empirically, that default
         // sentinel is rejected by the core as an out-of-range row against an
         // implicit single-row scalar "table" -- confirmed live: it raised
         // "PixelMath: Invalid table row index: expression" from CORE-SIDE
         // code (not a pcl::Exception our own catch below ever sees) and
         // popped a native modal dialog that is NOT gated by
         // pcl::Exception::EnableGUIOutput/DisableGUIOutput (that flag lives
         // in OUR module's own statically-linked copy of libPCL-pxi.a, a
         // separate instance from whatever copy the CORE's own compiled
         // PixelMath implementation consults). A plain PJSR probe
         // (`P.expression = "$T*0.5"`, no module involved) sets the very same
         // parameter without incident, confirming the parameter and value are
         // fine and the defect was purely in the row index we passed. Every
         // scalar (non-table) parameter access below passes row 0 explicitly;
         // only genuine table columns (HistogramTransformation's `H`, further
         // down) use a real row index.
         constexpr size_type kScalarRow = 0;

         const double before = ChannelMedian( view, 0 );
         Process P( IsoString( "PixelMath" ) );
         ProcessInstance pm( P );
         const bool setOk = pm.SetParameterValue( Variant( String( "$T*0.5" ) ), IsoString( "expression" ), kScalarRow );
         String whyNot;
         // Skip Validate()/CanExecuteOn() entirely when the set already
         // failed -- nothing downstream can be meaningfully validated against
         // an instance whose expression was never written.
         const bool valid = setOk && pm.Validate( whyNot );
         info["pmValidateWhyNot"] = U8( whyNot );
         // Non-blocking busy probe (global constraint; see ViewBusy() above)
         // immediately before CanExecuteOn()/ExecuteOn() -- placed here, at
         // the call site, not just inside ChannelMedian()/SampleAt(), because
         // Task 2's apply_process must probe at EVERY point it is about to
         // read or execute on a real user view, not rely on a shared helper
         // happening to do it upstream.
         bool can = false;
         if ( valid )
         {
            if ( ViewBusy( view ) )
               whyNot = "view is busy (locked by a running process)";
            else
               can = pm.CanExecuteOn( view, whyNot );
         }
         info["pmCanExecuteWhyNot"] = U8( whyNot );
         const bool ran = can && pm.ExecuteOn( view );
         const double after = ChannelMedian( view, 0 );
         info["pmSet"] = setOk; info["pmValid"] = valid; info["pmCan"] = can; info["pmRan"] = ran;
         info["pmMedianBefore"] = before; info["pmMedianAfter"] = after;
         pmExecOk = ran && std::fabs( after - 0.5*before ) < 1e-5;

         // SCNR enum: element list, default, set-by-value + read-back.
         //
         // color.EnumerationElements() -- pure metadata introspection, no
         // parameter value access at all -- is a THIRD confirmed instance of
         // the cross-module core bug already documented in ProcessCatalog.cpp
         // for PixelMath's newImageColorSpace/newImageSampleFormat: the same
         // "GetParameterElementIdentifier(): API function error" (the
         // query-length-then-fill two-call pattern for the element ID STRING,
         // ProcessParameter.cpp:440/444, fails inside PCL's own static
         // library for this parameter when queried from a foreign module).
         // Isolated in its own try/catch so this pre-existing, external,
         // documented core limitation cannot mask the independent facts this
         // section still has to prove (amount range, HT table, global-only
         // detection).
         //
         // Ground truth for the values this bug hides (confirmed externally,
         // NOT assumed): a standalone PJSR probe with NO module involved --
         // `SCNR.prototype.Red/.Green/.Blue` -- reads 0/1/2 (default
         // colorToRemove == 1, Green), matching PixInsight's long-published,
         // stable SCNR scripting reference. The round trip below uses value 0
         // directly for that reason when introspection fails, so it still
         // proves the SetParameterValue()/ParameterValue() round trip for an
         // Enumeration-type parameter -- the capability Task 2's
         // apply_process actually depends on -- without depending on the
         // broken call to "discover" the value first.
         Process S( IsoString( "SCNR" ) );
         ProcessParameter color( S, IsoString( "colorToRemove" ) );
         nlohmann::json elements = nlohmann::json::array();
         bool enumIntrospectionOk = false;
         String enumIntrospectionError;
         try
         {
            for ( const ProcessParameter::EnumerationElement& e : color.EnumerationElements() )
               elements.push_back( { { "id", std::string( e.id.c_str() ) }, { "value", e.value } } );
            enumIntrospectionOk = !elements.empty();
         }
         catch ( const pcl::Exception& x ) { enumIntrospectionError = x.Message(); }
         info["scnrColorElements"] = elements;
         info["scnrColorElementsIntrospectionOk"] = enumIntrospectionOk;
         info["scnrColorElementsError"] = U8( enumIntrospectionError );

         constexpr int kScnrRedConfirmedExternally = 0;   // see comment above
         int redValue = kScnrRedConfirmedExternally;
         for ( const nlohmann::json& e : elements )
            if ( e.at( "id" ) == "Red" )
               redValue = e.at( "value" ).get<int>();

         ProcessInstance si( S );
         info["scnrColorDefault"] = si.ParameterValue( color, kScalarRow ).ToInt();
         const bool setRed = si.SetParameterValue( Variant( redValue ), color, kScalarRow );
         info["scnrColorReadBack"] = si.ParameterValue( color, kScalarRow ).ToInt();
         enumRoundTripOk = setRed && si.ParameterValue( color, kScalarRow ).ToInt() == redValue;

         // SCNR amount range (Task 2's out-of-range case depends on it).
         ProcessParameter amount( S, IsoString( "amount" ) );
         double lo = 0, hi = 0;
         amount.GetNumericRange( lo, hi );
         info["scnrAmountRange"] = { lo, hi };
         rangeOk = lo == 0 && hi == 1;

         // HistogramTransformation H: column ids, reallocation, cell set + read-back.
         Process H( IsoString( "HistogramTransformation" ) );
         ProcessParameter table( H, IsoString( "H" ) );
         ProcessInstance hi_( H );
         const ProcessParameter::parameter_list columns = table.TableColumns();
         nlohmann::json colIds = nlohmann::json::array();
         for ( const ProcessParameter& c : columns )
            colIds.push_back( std::string( c.Id().c_str() ) );
         info["htColumns"] = colIds;
         info["htDefaultRows"] = hi_.TableRowCount( table );
         bool cellsOk = hi_.AllocateTableRows( table, 5 );
         for ( size_type r = 0; r < 5; ++r )
            for ( size_type k = 0; k < columns.Length(); ++k )
            {
               const double v = (k == 1) ? (r == 3 ? 0.25 : 0.5) : ((k == 2 || k == 4) ? 1.0 : 0.0);
               cellsOk = cellsOk && hi_.SetParameterValue( Variant( v ), columns[k], r );
            }
         htTableOk = cellsOk && columns.Length() == 5 && hi_.TableRowCount( table ) == 5
                  && std::fabs( hi_.ParameterValue( columns[1], 3 ).ToDouble() - 0.25 ) < 1e-12;

         // Global-only detection.
         //
         // The brief's original choice, ImageIntegration, is NOT global-only
         // in this PixInsight version (1.9.5 Lockhart): its
         // CanProcessViews() returns true here (confirmed live; the base
         // MetaProcess default is true for both flags -- "the vast majority
         // of processes" -- and it appears nobody overrides it for
         // ImageIntegration in this build). A scan of the full 131-process
         // compiled-in catalog (Process::AllProcesses(), !CanProcessViews()
         // && CanProcessGlobal()) found exactly three ids: "MARSGen" (a
         // third-party module, not ours to depend on), "NukeX", and
         // "PICopilot" -- our OWN process, PICopilotProcess.cpp:64-70,
         // CanProcessViews() explicitly returns false, CanProcessGlobal()
         // true ("Global-only utility process"). Using "PICopilot" here is
         // strictly more reliable than any core-shipped process: it is a
         // fact we control and guarantee, not an assumption about the
         // platform's version-dependent defaults for a built-in process we
         // don't own. Pure metadata query, no execution -- no recursion risk.
         Process II( IsoString( "PICopilot" ) );
         info["globalOnlyProcessId"] = "PICopilot";
         info["globalOnlyCanProcessViews"] = II.CanProcessViews();
         info["globalOnlyCanProcessGlobal"] = II.CanProcessGlobal();
         info["pmCanProcessViews"] = P.CanProcessViews();
         globalOnlyOk = !II.CanProcessViews() && II.CanProcessGlobal() && P.CanProcessViews();
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = pmExecOk && enumRoundTripOk && htTableOk && rangeOk && globalOnlyOk;
      out["agentSmokeInfo"] = info;
      out["agentSmokePixelMathOk"] = pmExecOk;
      out["agentSmokeEnumOk"] = enumRoundTripOk;
      out["agentSmokeTableOk"] = htTableOk;
      out["agentSmokeRangeOk"] = rangeOk;
      out["agentSmokeGlobalOnlyOk"] = globalOnlyOk;
      out["agentSmokeError"] = U8( error );
      out["agentSmokeOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- Section A1: ApplyProcess (Task 2) ----------------------------------
   {
      bool pmOk = false, htOk = false, scnrOk = false, errorsOk = true, busyOk = false,
           changesOk = false, enumDefaultOk = false;
      nlohmann::json detail = nlohmann::json::object();
      String error;
      try
      {
         {  // PixelMath: String parameter -> pixel values halved.
            AgentTestWindow tw( "PICopilotApplyPM" );
            View v = tw.MainView();
            const double before = ChannelMedian( v, 0 );
            const ApplyProcessResult r = ApplyProcess( "PixelMath", { { "expression", "$T*0.5" } }, nlohmann::json(), v );
            const double after = ChannelMedian( v, 0 );
            detail["pm"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "before", before }, { "after", after },
                             { "elapsedMs", r.elapsedMs } };
            pmOk = r.ok && r.processId == "PixelMath" && std::fabs( after - 0.5*before ) < 1e-5
                && r.parametersSet.value( "expression", std::string() ) == "$T*0.5" && r.error.IsEmpty();
         }
         {  // HistogramTransformation: table parameter H (row 3 = combined RGB/K, m = 0.25 brightens).
            AgentTestWindow tw( "PICopilotApplyHT" );
            View v = tw.MainView();
            const double before = ChannelMedian( v, 1 );
            nlohmann::json rows = nlohmann::json::array();
            for ( int i = 0; i < 5; ++i )
               rows.push_back( nlohmann::json::array( { 0, i == 3 ? 0.25 : 0.5, 1, 0, 1 } ) );
            nlohmann::json tables = nlohmann::json::object();
            tables["H"] = rows;
            const ApplyProcessResult r = ApplyProcess( "HistogramTransformation", nlohmann::json::object(), tables, v );
            const double after = ChannelMedian( v, 1 );
            detail["ht"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "before", before }, { "after", after } };
            htOk = r.ok && after > 2*before;
         }
         {  // SCNR: enumeration parameters by element id -> red square loses red, grey background untouched.
            AgentTestWindow tw( "PICopilotApplySCNR" );
            View v = tw.MainView();
            const float bgBefore = SampleAt( v, 10, 10, 0 );
            const nlohmann::json p = { { "colorToRemove", "Red" }, { "protectionMethod", "AverageNeutral" },
                                       { "amount", 1.0 }, { "preserveLightness", false } };
            const ApplyProcessResult r = ApplyProcess( "SCNR", p, nlohmann::json(), v );
            const float sq = SampleAt( v, kAgSqX0 + kAgSq/2, kAgSqY0 + kAgSq/2, 0 );
            const float bgAfter = SampleAt( v, 10, 10, 0 );
            detail["scnr"] = { { "ok", r.ok }, { "error", U8( r.error ) }, { "square", sq }, { "bgBefore", bgBefore }, { "bgAfter", bgAfter } };
            scnrOk = r.ok && sq < 1e-6f && std::fabs( bgAfter - bgBefore ) < 1e-6f;
         }

         // Every error path: !ok, a precise message, and the image untouched.
         struct Case { const char* name; const char* process; nlohmann::json params; nlohmann::json tables; const char* expect; };
         const Case cases[] = {
            { "unknownProcess", "NoSuchProcessXYZ", nlohmann::json::object(), nlohmann::json(),
              "unknown process id 'NoSuchProcessXYZ'" },
            { "unknownParam", "PixelMath", { { "noSuchParam", 1 } }, nlohmann::json(),
              "unknown parameter PixelMath.noSuchParam" },
            { "badEnum", "SCNR", { { "colorToRemove", "Purple" } }, nlohmann::json(),
              "SCNR.colorToRemove: 'Purple' is not a valid value; use one of: Red, Green, Blue" },
            { "wrongType", "SCNR", { { "amount", "lots" } }, nlohmann::json(),
              "SCNR.amount: expected a number" },
            { "outOfRange", "SCNR", { { "amount", 5 } }, nlohmann::json(),
              "SCNR.amount: 5 is out of range [0, 1]" },
            { "tableAsScalar", "HistogramTransformation", { { "H", 1 } }, nlohmann::json(),
              "HistogramTransformation.H is a table parameter" },
            { "badRow", "HistogramTransformation", nlohmann::json::object(),
              { { "H", nlohmann::json::array( { nlohmann::json::array( { 0, 0.5, 1 } ) } ) } },
              "HistogramTransformation.H: row 0 has 3 values; expected 5" },
            // Not ImageIntegration: it CAN process views in PI 1.9.5 (Task 1
            // finding). Our own PICopilot process is global-only by contract.
            { "globalOnly", "PICopilot", nlohmann::json::object(), nlohmann::json(),
              "PICopilot can only run in the global context" },
            // A syntax error is only found by the core at execution time:
            // ExecuteOn() returns false (no exception, no modal -- verified
            // under Xvfb), and the view is left untouched.
            { "badExpression", "PixelMath", { { "expression", "$T*" } }, nlohmann::json(),
              "PixelMath did not complete on PICopilotApplyErr" },
            // Enumeration whose native element ids are unreadable (resolved
            // via PJSR introspection): an unknown id still gets the real list.
            { "badEnumPJSR", "PixelMath", { { "newImageColorSpace", "CMYK" } }, nlohmann::json(),
              "PixelMath.newImageColorSpace: 'CMYK' is not a valid value; use one of: SameAsTarget, RGB, Gray" },
            // Row count is checked BEFORE AllocateTableRows(): the core
            // answers a bad length with a modal dialog (seen live). H takes
            // 4 or 5 rows (core length limits).
            { "wrongRowCount", "HistogramTransformation", nlohmann::json::object(),
              { { "H", nlohmann::json::array( { nlohmann::json::array( { 0, 0.5, 1, 0, 1 } ) } ) } },
              "HistogramTransformation.H: 1 rows given; this table needs between 4 and 5 rows (columns: c0, m, c1, r0, r1)" },
            { "cellType", "HistogramTransformation", nlohmann::json::object(),
              { { "H", nlohmann::json::array( {
                  nlohmann::json::array( { 0, "x", 1, 0, 1 } ), nlohmann::json::array( { 0, 0.5, 1, 0, 1 } ),
                  nlohmann::json::array( { 0, 0.5, 1, 0, 1 } ), nlohmann::json::array( { 0, 0.5, 1, 0, 1 } ),
                  nlohmann::json::array( { 0, 0.5, 1, 0, 1 } ) } ) } },
              "HistogramTransformation.H[0].m: expected a number" },
            { "intOutOfType", "PixelMath", { { "newImageWidth", -5 } }, nlohmann::json(),
              "PixelMath.newImageWidth: -5 is below the minimum 0" },
            { "notAnInteger", "PixelMath", { { "newImageWidth", 2.5 } }, nlohmann::json(),
              "PixelMath.newImageWidth: expected an integer, got 2.5" },
         };
         AgentTestWindow tw( "PICopilotApplyErr" );
         View v = tw.MainView();
         const double before = ChannelMedian( v, 0 );
         nlohmann::json caseOut = nlohmann::json::array();
         for ( const Case& c : cases )
         {
            const ApplyProcessResult r = ApplyProcess( c.process, c.params, c.tables, v );
            const bool pass = !r.ok && r.error.Contains( String::UTF8ToUTF16( c.expect ) );
            caseOut.push_back( { { "case", c.name }, { "pass", pass }, { "error", U8( r.error ) } } );
            errorsOk = errorsOk && pass;
         }
         detail["errorCases"] = caseOut;
         errorsOk = errorsOk && std::fabs( ChannelMedian( v, 0 ) - before ) < 1e-12;

         {  // Busy view: locked by "someone else" -> immediate error, never a wait.
            const auto t0 = std::chrono::steady_clock::now();
            ApplyProcessResult r;
            {
               View locked = v;
               AutoViewLock lock( locked );
               r = ApplyProcess( "PixelMath", { { "expression", "$T*0.5" } }, nlohmann::json(), v );
            }
            const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
            detail["busy"] = { { "error", U8( r.error ) }, { "ms", ms } };
            busyOk = !r.ok && r.error.Contains( "is busy" ) && ms < 2000
                  && std::fabs( ChannelMedian( v, 0 ) - before ) < 1e-12;
         }

         {  // Human-readable change list (Guided dialog / log).
            nlohmann::json tables = nlohmann::json::object();
            tables["H"] = nlohmann::json::array( { nlohmann::json::array( { 0, 0.5, 1, 0, 1 } ) } );
            const String s = DescribeParameterChanges( { { "expression", "$T*0.5" } }, tables, 1000 );
            const String none = DescribeParameterChanges( nlohmann::json::object(), nlohmann::json(), 1000 );
            changesOk = s.Contains( "expression = $T*0.5" ) && s.Contains( "H = [[0,0.5,1,0,1]]" )
                     && none == "(all parameters at their defaults)";
         }

         {  // describe_process default for an enum is the element at the default INDEX.
            // Native element-id introspection is broken for this parameter
            // (Task 1), so this also proves the PJSR fallback feeds describe.
            const nlohmann::json d = DescribeProcess( "SCNR" );
            for ( const nlohmann::json& p : d.at( "parameters" ) )
               if ( p.at( "id" ) == "colorToRemove" )
               {
                  detail["scnrColorDescribe"] = p;
                  const nlohmann::json want = nlohmann::json::array( {
                     { { "id", "Red" }, { "value", 0 } }, { { "id", "Green" }, { "value", 1 } },
                     { { "id", "Blue" }, { "value", 2 } } } );
                  enumDefaultOk = p.value( "default", std::string() ) == "Green"
                               && p.value( "enumeration", nlohmann::json() ) == want && !p.contains( "error" );
               }
         }
         {  // Apply agrees with describe: a PJSR-resolved enum id on another process.
            AgentTestWindow tw( "PICopilotApplyPMEnum" );
            View v = tw.MainView();
            const ApplyProcessResult r = ApplyProcess( "PixelMath",
               { { "expression", "$T" }, { "newImageSampleFormat", "f32" } }, nlohmann::json(), v );
            detail["pmEnum"] = { { "ok", r.ok }, { "error", U8( r.error ) } };
            enumDefaultOk = enumDefaultOk && r.ok && r.parametersSet.value( "newImageSampleFormat", std::string() ) == "f32";
         }
      }
      catch ( const pcl::Exception& x ) { error = x.Message(); }
      catch ( const std::exception& x ) { error = String( x.what() ); }
      catch ( ... )                     { error = "unknown exception"; }

      const bool ok = pmOk && htOk && scnrOk && errorsOk && busyOk && changesOk && enumDefaultOk;
      out["applyDetail"] = detail;
      out["applyPixelMathOk"] = pmOk;
      out["applyTableOk"] = htOk;
      out["applyEnumOk"] = scnrOk;
      out["applyErrorsOk"] = errorsOk;
      out["applyBusyOk"] = busyOk;
      out["applyChangesOk"] = changesOk;
      out["catalogEnumDefaultOk"] = enumDefaultOk;
      out["applyError"] = U8( error );
      out["applyProcessOk"] = ok;
      allOk = allOk && ok;
   }

   // ---- inc4 sections end ----

   // Let the core finish the deferred teardown of the windows force-closed
   // above before control returns to --force-exit (same reasoning as the
   // drain at the end of RunVisionSelfTest). >= 250 ms between calls.
   for ( int i = 0; i < 4; ++i )
   {
      ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
      std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
   }

   return allOk;
}

} // namespace pcl
