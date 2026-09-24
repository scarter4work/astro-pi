// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotAgentSelfTest.h"
#include "PICopilotModule.h"
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
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

double ChannelMedian( View view, int c )
{
   AutoViewWriteLock lock( view );
   ImageVariant v = view.Image();
   return v.Median( v.Bounds(), c, c );
}

// [[maybe_unused]]: not called by Section A0 (Task 1) -- Tasks 2-7 use it to
// verify per-pixel results of PixelMath/SCNR/HistogramTransformation runs.
[[maybe_unused]] float SampleAt( View view, int x, int y, int c )
{
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
         const bool valid = pm.Validate( whyNot );
         info["pmValidateWhyNot"] = U8( whyNot );
         const bool can = pm.CanExecuteOn( view, whyNot );
         info["pmCanExecuteWhyNot"] = U8( whyNot );
         const bool ran = setOk && valid && can && pm.ExecuteOn( view );
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
