// Self-test harness driver. C++ ExecuteGlobal() runs the self-test and writes
// the result JSON to the private mktemp path the harness passes in
// PICOPILOT_SELFTEST_OUT (see PICopilotInstance.cpp); this script drives the
// process.
//
// MULTI-PHASE HARNESS (0.2.0.0). A process executed while PICopilot's
// executeGlobal() is running is NEVER recorded in History (measured in plan
// Task 1), so every step whose history a test inspects runs HERE, at top level,
// and the module only READS it:
//   1. top-level fixture code (executeOn / historyIndex undo+redo / save+reopen);
//   2. checkPhase( id, payload ): writes {"phase": id, "payload": payload} to
//      $PICOPILOT_SELFTEST_PHASE and runs PICopilot.executeGlobal(); the C++
//      handler registered for `id` (SelfTestPhaseHandlers() in
//      PICopilotJourneySelfTest.cpp) runs in-process and its result is kept in
//      SelfTestPhaseStore()[id] (one entry per call, in order);
//   3. the final executeGlobal() (no phase file) runs the whole self-test,
//      whose sections read SelfTestPhaseStore() and the fixture windows.
// Adding a phase = one handler entry in SelfTestPhaseHandlers() + fixture code
// and checkPhase() calls in a new block below (before the final run). A
// fixture block that throws calls harnessError(), which fails journeySpikeOk
// loudly via SelfTestPhaseStore()["errors"]. Fixture windows stay open; the
// section that reads them closes them.

var PHASE_FILE = getEnvironmentVariable( "PICOPILOT_SELFTEST_PHASE" );

// Per-block wall-clock timings (so a slow run is self-diagnosing): jsMark( label )
// closes the block still open and opens `label`; jsMark( null ) only closes.
// Written to $PICOPILOT_SELFTEST_JS_TIMINGS at the end; run-selftest.sh prints
// one line per block, next to the module's own per-section "sectionTimings".
var JS_T0 = Date.now(), jsTimings = [], jsOpen = null;
function jsMark( label )
{
   var now = Date.now();
   if ( jsOpen )
      jsTimings.push( { section: jsOpen.label, startS: ( jsOpen.t - JS_T0 )/1000, ms: now - jsOpen.t } );
   jsOpen = label ? { label: label, t: now } : null;
   // Rewritten at every mark, so a run that hangs or times out still shows the
   // block it was in (jsOpen) and every block before it.
   var path = getEnvironmentVariable( "PICOPILOT_SELFTEST_JS_TIMINGS" );
   if ( path.length > 0 )
      File.writeTextFile( path, JSON.stringify( { done: jsTimings,
         open: jsOpen ? { section: jsOpen.label, startS: ( jsOpen.t - JS_T0 )/1000 } : null } ) );
}
jsMark( "pre-phase (panel never opened)" );

function pumpEvents( ms )
{
   var t0 = Date.now();
   while ( Date.now() - t0 < ms ) { processEvents(); msleep( 20 ); }
}

function checkPhase( id, payload )
{
   if ( PHASE_FILE.length == 0 )
      throw new Error( "PICOPILOT_SELFTEST_PHASE is not set" );
   pumpEvents( 300 );
   File.writeTextFile( PHASE_FILE, JSON.stringify( { phase: id, payload: payload } ) );
   try
   {
      ( new PICopilot ).executeGlobal();
   }
   finally
   {
      if ( File.exists( PHASE_FILE ) )
         File.remove( PHASE_FILE );
   }
}

function harnessError( where, e )
{
   checkPhase( "harness.error", { where: where, error: String( e ) } );
}

function normXpsm( s )
{
   return s.replace( / (id|enabled)="[^"]*"/, "" );
}
// Journey spike pre-phase (plan Task 1): user-like actions BEFORE the self-test
// process runs, with the PI Copilot panel NEVER opened. The module's
// notification probe and OnLoad timer record what reaches them; the result of
// the actions themselves goes to $PICOPILOT_SELFTEST_PRE (read by section J0).
// The windows stay open: section J6 checks that the production JourneyService
// recorded them.
(function ()
{
   var out = { steps: [] };
   try
   {
      var pump = function ( ms ) { var t0 = Date.now(); while ( Date.now() - t0 < ms ) { processEvents(); msleep( 20 ); } };
      var w = new ImageWindow( 64, 64, 1, 32, true, false, "pcJourneyPre" );
      w.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ),
                     new FITSKeyword( "OBJECT", "'PreM42'", "" ),
                     new FITSKeyword( "FILTER", "'L'", "" ),
                     new FITSKeyword( "EXPTIME", "120", "" ),
                     new FITSKeyword( "SITELAT", "'+40 11 12'", "" ) ];
      w.show();
      pump( 700 );
      var v = w.mainView;
      var p1 = new PixelMath; p1.expression = "0.2"; p1.executeOn( v ); out.steps.push( "pm1" ); pump( 700 );
      var p2 = new PixelMath; p2.expression = "$T*1.5"; p2.executeOn( v ); out.steps.push( "pm2" ); pump( 700 );
      v.historyIndex = v.historyIndex - 1; out.steps.push( "undo" ); pump( 700 );
      v.historyIndex = v.historyIndex + 1; out.steps.push( "redo" ); pump( 700 );
      var p3 = new PixelMath; p3.expression = "$T"; p3.createNewImage = true; p3.newImageId = "pcJourneyPreNew";
      p3.executeOn( v ); out.steps.push( "createNew" ); pump( 1500 );
      out.historyIndex = v.historyIndex;
      out.length = v.processing.length;
   }
   catch ( e )
   {
      out.error = String( e );
   }
   var path = getEnvironmentVariable( "PICOPILOT_SELFTEST_PRE" );
   if ( path.length > 0 )
      File.writeTextFile( path, JSON.stringify( out ) );
})();

jsMark( "fixture j6.service" );
// Section J6 (plan Task 7): the production JourneyService recorded the
// pre-phase above. Flush it and pause it now, before any other fixture phase,
// so its ticks never interleave with the timing-sensitive phases and sections.
try
{
   checkPhase( "j6", { step: "service" } );
}
catch ( e )
{
   harnessError( "j6.service", e );
}

jsMark( "fixture probe.nestedEval" );
// ---- J0 fixture phases (plan Task 1) ----

// The spike probe's nested EvaluateScript has done its pre-phase job; keep it
// off for everything after (J0 (2b) re-enables it only for its measurement).
try
{
   checkPhase( "probe.nestedEval", { on: false } );
}
catch ( e )
{
   harnessError( "probe.nestedEval", e );
}

jsMark( "fixture j0.timerApply" );
// (4b) The production ApplyProcess run from a module Timer tick -- the panel's
// execution context -- on a top-level window: is the step recorded in History?
// This script only pumps events while the tick runs (under --force-exit PI
// exits when the script returns, so an idle event loop cannot be reached).
try
{
   ( function ()
   {
      var w = new ImageWindow( 32, 32, 1, 32, true, false, "pcSpikeTimerApply" );
      w.show();
      var v = w.mainView;
      var p0 = new PixelMath; p0.expression = "0.4"; p0.executeOn( v );   // a prior top-level step
      var lengthBefore = v.processing.length;
      // Handshake: the arm phase only STAGES the request. A tick can fire while
      // PixInsight is still finishing executeGlobal() (after the module's own
      // ExecuteGlobal returned), where History records nothing (measured,
      // fix round 2: with 10 ms ticks, 20/20 such applies changed the pixels
      // but recorded no step). The probe applies only once this go file
      // exists, and it is written here, i.e. after executeGlobal() fully
      // returned; then pump until the step appears (5 s cap; J0 fails loudly).
      var go = getEnvironmentVariable( "PICOPILOT_SELFTEST_SCRATCH" ) + "/timer-apply.go";
      checkPhase( "j0.timerApply.arm", { id: "pcSpikeTimerApply", goFile: go } );
      File.writeTextFile( go, "go" );
      var t0 = Date.now();
      while ( v.processing.length == lengthBefore && Date.now() - t0 < 5000 ) { processEvents(); msleep( 20 ); }
      File.remove( go );
      var pr = v.processing, last = pr.length > 0 ? pr.at( pr.length - 1 ) : null;
      var src = last ? last.toSource( "XPSM 1.0" ) : "";
      checkPhase( "j0.timerApply.check", {
         lengthBefore: lengthBefore, lengthAfter: pr.length, historyIndex: v.historyIndex,
         lastProcessId: last ? last.processId() : "",
         lastHasExpression: src.indexOf( "<parameter id=\"expression\">$T*0.5</parameter>" ) >= 0,
         lastHead: src.substring( 0, 200 ) } );
   } )();
}
catch ( e )
{
   harnessError( "j0.timerApply", e );
}

jsMark( "fixture hist (T-hist)" );
// ---- Section JH (Task T-hist): an applied process lands in History, or fails loudly ----
// The forced-10 ms repro. A process applied while PixInsight is still inside
// ANOTHER process execution changes the pixels but records no History step.
// Every cycle: a top-level window at 0.8 (one prior step), then the probe's
// module Timer (at 10 ms) applies PixelMath $T*0.5 through the production
// ApplyProcess:
//   tail   -- armed by checkPhase(): the first tick lands while PixInsight is
//             still finishing that executeGlobal() (measured 16/16 unrecorded);
//   during -- 80 ms after arming, while a PixelMath runs on a 4000x4000 view
//             (measured 13/13 unrecorded);
// ungated applies at once (the DETECT path: must be a loud error, never ok);
// gated waits for the tool loop's PREVENT gate (must land in History).
try
{
   ( function ()
   {
      checkPhase( "hist.timer", { intervalS: 0.01 } );
      var big = new ImageWindow( 4000, 4000, 3, 32, true, true, "pcHistBig" );
      big.show();
      var plan = [];
      [ [ "tail", false, 5 ], [ "tail", true, 5 ], [ "during", false, 3 ], [ "during", true, 3 ] ].forEach(
         function( k ) { for ( var i = 0; i < k[2]; ++i ) plan.push( { kind: k[0], gated: k[1], i: i } ); } );
      plan.forEach( function( c )
      {
         var id = "pcHist_" + c.kind + ( c.gated ? "G" : "U" ) + c.i;
         var w = new ImageWindow( 32, 32, 1, 32, true, false, id );
         w.show();
         var v = w.mainView;
         var p0 = new PixelMath; p0.expression = "0.8"; p0.executeOn( v );
         pumpEvents( 150 );
         var lengthBefore = v.processing.length, pxBefore = v.image.sample( 0, 0 );
         checkPhase( "hist.arm", { id: id, delayS: c.kind == "tail" ? 0 : 0.08, gated: c.gated } );
         if ( c.kind == "during" )
         {
            var ph = new PixelMath;
            ph.expression = "sin(cos(sin(cos(sin(cos($T+0.1))))))*exp(-$T)+ln(1+$T)*atan($T)";
            ph.executeOn( big.mainView );
         }
         var t0 = Date.now();
         while ( v.image.sample( 0, 0 ) == pxBefore && Date.now() - t0 < 5000 ) { processEvents(); msleep( 10 ); }
         pumpEvents( 100 );
         var pr = v.processing, last = pr.length > 0 ? pr.at( pr.length - 1 ) : null;
         var src = last ? last.toSource( "XPSM 1.0" ) : "";
         checkPhase( "hist.check", {
            id: id, kind: c.kind, gated: c.gated, lengthBefore: lengthBefore, lengthAfter: pr.length,
            pxBefore: pxBefore, pxAfter: v.image.sample( 0, 0 ), lastProcessId: last ? last.processId() : "",
            lastHasExpression: src.indexOf( "<parameter id=\"expression\">$T*0.5</parameter>" ) >= 0 } );
         w.forceClose();
      } );
      big.forceClose();

      // Process kinds (review round 1): DETECT must not cry wolf on a recorded
      // step that does not advance ModifyCount (measured: ImageIdentifier and
      // RGBWorkingSpace record a step with ModifyCount +0). Idle (gated)
      // applies from the module Timer: each must be ok with History +1; the
      // two ModifyCount-silent kinds also ungated in the tail window, where
      // they must be the loud error. ScreenTransferFunction adds no step.
      var kinds = [
         { process: "PixelMath", parameters: { expression: "$T" } },
         { process: "FITSHeader", tableParameters: { keywords: [ [ "PCHIST", "42", "T-hist" ] ] } },
         { process: "ImageIdentifier", parameters: { id: "pcHistRenamedG" } },
         { process: "Crop", parameters: { mode: "AbsolutePixels", leftMargin: -4, topMargin: -4, rightMargin: 0, bottomMargin: 0 } },
         { process: "Resample", parameters: { mode: "RelativeDimensions", xSize: 0.5, ySize: 0.5 } },
         { process: "RGBWorkingSpace", parameters: {} },
         { process: "AssignICCProfile", parameters: {} },
         { process: "Invert", parameters: {} },
         { process: "ScreenTransferFunction", parameters: {} },
         { process: "ImageIdentifier", parameters: { id: "pcHistRenamedU" }, ungated: true },
         { process: "RGBWorkingSpace", parameters: {}, ungated: true } ];
      kinds.forEach( function( k, i )
      {
         var id = "pcHistKind" + i;
         var w = new ImageWindow( 32, 32, 3, 32, true, true, id );
         w.show();
         var p0 = new PixelMath; p0.expression = "0.8"; p0.executeOn( w.mainView );
         pumpEvents( 150 );
         var v = w.mainView, l0 = v.processing.length, h0 = v.historyIndex;
         checkPhase( "hist.arm", { id: id, delayS: 0, gated: !k.ungated,
                                   spec: { process: k.process, parameters: k.parameters || {},
                                           tableParameters: k.tableParameters || {} } } );
         pumpEvents( 900 );
         v = w.mainView;
         var pr = v.processing, last = pr.length > 0 ? pr.at( pr.length - 1 ) : null;
         checkPhase( "hist.kind.check", { process: k.process, gated: !k.ungated, lengthBefore: l0, lengthAfter: pr.length,
                                          historyIndexBefore: h0, historyIndexAfter: v.historyIndex,
                                          lastProcessId: last ? last.processId() : "", idNow: v.id } );
         w.forceClose();
      } );

      // Previews (review round 2): a fresh preview and one with a prior step,
      // each gated (idle) and ungated (tail window). Main-image pixels are
      // read inside (2,2) and outside (20,20) the preview rectangle.
      [ [ true, false ], [ true, true ], [ false, false ], [ false, true ] ].forEach( function( k, i )
      {
         var id = "pcHistPvW" + i;
         var w = new ImageWindow( 32, 32, 1, 32, true, false, id );
         w.show();
         var p0 = new PixelMath; p0.expression = "0.8"; p0.executeOn( w.mainView );
         w.createPreview( new Rect( 0, 0, 16, 16 ), "pv" );
         if ( k[1] ) { var pp = new PixelMath; pp.expression = "0.6"; pp.executeOn( w.previewById( "pv" ) ); }
         pumpEvents( 150 );
         var read = function()
         {
            var m = w.mainView, pv = w.previewById( "pv" ), pr = pv.processing, last = "";
            if ( pr.length > 0 )
            {
               var src = pr.at( pr.length - 1 ).toSource( "XPSM 1.0" );
               var e = src.match( /<parameter id="expression">([^<]*)</ ), t = src.match( /start="([^"]*)"/ );
               last = pr.at( pr.length - 1 ).processId() + ":" + ( e ? e[1] : "" ) + "@" + ( t ? t[1] : "" );
            }
            return { mainIn: m.image.sample( 2, 2 ), mainOut: m.image.sample( 20, 20 ), mainLen: m.processing.length,
                     mainHi: m.historyIndex, pvPx: pv.image.sample( 2, 2 ), pvLen: pr.length, pvHi: pv.historyIndex,
                     pvLast: last };
         };
         var before = read();
         checkPhase( "hist.arm", { id: id + "->pv", delayS: 0, gated: k[0] } );
         pumpEvents( 900 );
         checkPhase( "hist.preview.check", { gated: k[0], priorStep: k[1], before: before, after: read() } );
         w.forceClose();
      } );
      checkPhase( "hist.timer", { intervalS: 0.2 } );
   } )();
}
catch ( e )
{
   harnessError( "hist", e );
   try { checkPhase( "hist.timer", { intervalS: 0.2 } ); } catch ( e2 ) {}
}

jsMark( "fixture j0.mc" );
// (4) ModifyCount across step / undo / redo: phase j0.mc reads it between steps.
try
{
   var mcWins = [ new ImageWindow( 32, 32, 1, 32, true, false, "pcSpikeMC" ),
                  new ImageWindow( 32, 32, 1, 32, true, false, "pcSpikeMCShown" ) ];
   mcWins[1].show();
   var mcIds = [ "pcSpikeMC", "pcSpikeMCShown" ];
   checkPhase( "j0.mc", { label: "start", ids: mcIds } );
   mcWins.forEach( function( w ) { var p = new PixelMath; p.expression = "0.25"; p.executeOn( w.mainView ); } );
   checkPhase( "j0.mc", { label: "step", ids: mcIds } );
   mcWins.forEach( function( w ) { w.mainView.historyIndex = w.mainView.historyIndex - 1; } );
   checkPhase( "j0.mc", { label: "undo", ids: mcIds } );
   mcWins.forEach( function( w ) { w.mainView.historyIndex = w.mainView.historyIndex + 1; } );
   checkPhase( "j0.mc", { label: "redo", ids: mcIds } );
}
catch ( e )
{
   harnessError( "j0.mc", e );
}

jsMark( "fixture j0.identity" );
// (5) Created-window identity + Ruling 28 keyword inheritance.
try
{
   ( function ()
   {
      function hasMasterType( w )
      {
         var k = w.keywords;
         for ( var i = 0; i < k.length; ++i )
            if ( k[i].name.trim() == "IMAGETYP" && k[i].strippedValue.trim() == "Master Light" )
               return true;
         return false;
      }
      var sw = new ImageWindow( 32, 32, 1, 32, true, false, "pcSpikeIdSrc" );
      sw.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ) ];
      var v = sw.mainView;
      var p = new PixelMath; p.expression = "$T"; p.createNewImage = true; p.newImageId = "pcSpikeIdNew";
      var before = v.processing.length;
      p.executeOn( v );
      var sourceGotStep = v.processing.length != before;
      // No source step: the reference is the executed instance, whose
      // toSource() then carries the same <time start=...>.
      var src = normXpsm( ( sourceGotStep ? v.processing.at( v.processing.length - 1 ) : p ).toSource( "XPSM 1.0" ) );
      var nv = View.viewById( "pcSpikeIdNew" );
      var made = nv.initialProcessing.length > 0 ? normXpsm( nv.initialProcessing.at( 0 ).toSource( "XPSM 1.0" ) ) : "";
      var inheritPM = hasMasterType( nv.window );
      var rgb = new ImageWindow( 16, 16, 3, 32, true, true, "pcSpikeRGB" );
      rgb.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ) ];
      var ce = new ChannelExtraction;
      ce.executeOn( rgb.mainView );
      var rp = rgb.mainView.processing;
      var ceSourceGotStep = rp.length > 0;
      var csrc = normXpsm( ( ceSourceGotStep ? rp.at( rp.length - 1 ) : ce ).toSource( "XPSM 1.0" ) );
      var parts = [], all = true;
      ImageWindow.windows.forEach( function( w )
      {
         var id = w.mainView.id;
         if ( id.indexOf( "pcSpikeRGB_" ) == 0 )
         {
            var ip = w.mainView.initialProcessing;
            var m = ip.length > 0 && normXpsm( ip.at( 0 ).toSource( "XPSM 1.0" ) ) == csrc;
            parts.push( { id: id, initialLength: ip.length, match: m, inherits: hasMasterType( w ) } );
            all = all && m;
         }
      } );
      var inheritCE = parts.length > 0;
      parts.forEach( function( q ) { inheritCE = inheritCE && q.inherits; } );
      checkPhase( "j0.identity", {
         pixelMath: { match: made == src, sourceGotStep: sourceGotStep, madeInitialLength: nv.initialProcessing.length,
                      madeHasStart: made.indexOf( "<time start=" ) >= 0,
                      madeHead: made.substring( 0, 160 ), srcHead: src.substring( 0, 160 ) },
         channelExtraction: { windows: parts, allMatch: all && parts.length == 3, sourceGotStep: ceSourceGotStep },
         keywordsInheritedPixelMath: inheritPM, keywordsInheritedChannelExtraction: inheritCE } );
   } )();
}
catch ( e )
{
   harnessError( "j0.identity", e );
}

jsMark( "fixture j0.long (500 steps)" );
// (7) A 500-step history for the read/parse cost (read in-process by J0).
try
{
   var longWin = new ImageWindow( 64, 64, 1, 32, true, false, "pcSpikeLong" );
   for ( var i = 0; i < 500; ++i )
   {
      var lp = new PixelMath; lp.expression = "$T*1.0"; lp.executeOn( longWin.mainView );
   }
}
catch ( e )
{
   harnessError( "j0.long", e );
}

jsMark( "fixture j0.reopen" );
// (7b) Save + reopen entry count (Ruling 27).
try
{
   ( function ()
   {
      // A private directory the harness created (and removes in cleanup()).
      var dir = getEnvironmentVariable( "PICOPILOT_SELFTEST_SCRATCH" );
      if ( dir.length == 0 || !File.directoryExists( dir ) )
         throw new Error( "PICOPILOT_SELFTEST_SCRATCH is not an existing directory" );
      var path = dir + "/reopen.xisf";
      var r;
      try
      {
         var w = new ImageWindow( 32, 32, 1, 32, true, false, "pcSpikeReopen" );
         for ( var i = 0; i < 5; ++i )
         {
            var p = new PixelMath; p.expression = "$T*1.0+" + (i*0.01); p.executeOn( w.mainView );
         }
         var saved = [], ip0 = w.mainView.initialProcessing, sp = w.mainView.processing;
         for ( var i = 0; i < ip0.length; ++i ) saved.push( normXpsm( ip0.at( i ).toSource( "XPSM 1.0" ) ) );
         for ( var i = 0; i < sp.length; ++i ) saved.push( normXpsm( sp.at( i ).toSource( "XPSM 1.0" ) ) );
         var savedInitialLength = ip0.length, savedProcessingLength = sp.length;
         if ( !w.saveAs( path, false, false, false, false ) )
            throw new Error( "saveAs failed" );
         w.forceClose();
         var ws = ImageWindow.open( path );
         if ( ws.length < 1 )
            throw new Error( "open failed" );
         var o = ws[0];
         var lengthBeforeRename = o.mainView.processing.length;
         o.mainView.id = "pcSpikeReopened";
         var pids = [], op = o.mainView.processing;
         for ( var i = 0; i < op.length; ++i ) pids.push( op.at( i ).processId() );
         var ip = o.mainView.initialProcessing;
         var extraId = "", extraLeads = false, extras = 0, ids = [];
         for ( var i = 0; i < ip.length; ++i )
         {
            ids.push( ip.at( i ).processId() );
            if ( saved.indexOf( normXpsm( ip.at( i ).toSource( "XPSM 1.0" ) ) ) < 0 )
            {
               ++extras; extraId = ip.at( i ).processId(); extraLeads = i == 0;
            }
         }
         r = { savedLength: saved.length, savedInitialLength: savedInitialLength, savedProcessingLength: savedProcessingLength,
               initialLength: ip.length, initialIds: ids, length: o.mainView.processing.length,
               lengthBeforeRename: lengthBeforeRename, processingIds: pids,
               processingHead: op.length > 0 ? op.at( 0 ).toSource( "XPSM 1.0" ).substring( 0, 300 ) : "",
               historyIndex: o.mainView.historyIndex, unmatched: extras, extraProcessId: extraId, extraLeads: extraLeads };
      }
      catch ( e )
      {
         r = { error: String( e ) };
      }
      finally
      {
         if ( File.exists( path ) ) File.remove( path );
      }
      checkPhase( "j0.reopen", r );
   } )();
}
catch ( e )
{
   harnessError( "j0.reopen", e );
}

jsMark( "fixture j2.hr" );
// ---- J2 fixture phases (plan Task 3: HistoryReader) ----
// pcHrA's history is made here, step by step; phase j2.hr reads it in between
// (PhaseHistoryReader, J2State). Section J2 closes every window made here.
try
{
   ( function ()
   {
      function pm( v, x ) { var p = new PixelMath; p.expression = x; p.executeOn( v ); }
      var w = new ImageWindow( 32, 32, 1, 32, true, false, "pcHrA" );
      var v = w.mainView;
      [ "0.1", "$T+0.1", "$T*2" ].forEach( function( x ) { pm( v, x ); } );
      checkPhase( "j2.hr", { step: "live0" } );
      v.historyIndex = v.historyIndex - 2;                        // undo two
      checkPhase( "j2.hr", { step: "undo" } );
      v.historyIndex = v.historyIndex + 2;                        // redo them
      checkPhase( "j2.hr", { step: "redo" } );
      v.historyIndex = v.historyIndex - 2;                        // undo two, then branch with two new steps
      pm( v, "$T-0.05" );
      pm( v, "$T*0.9" );
      checkPhase( "j2.hr", { step: "branch" } );
      var m = new ImageWindow( 32, 32, 1, 32, true, false, "pcHrMask" );
      w.mask = m; w.maskEnabled = true; w.maskInverted = true;
      pm( v, "$T" );
      w.removeMask();
      checkPhase( "j2.hr", { step: "mask" } );
      v.id = "pcHrRenamed";                                       // recorded as an ImageIdentifier step
      checkPhase( "j2.hr", { step: "rename" } );
      var dir = getEnvironmentVariable( "PICOPILOT_SELFTEST_SCRATCH" );
      if ( dir.length == 0 || !File.directoryExists( dir ) )
         throw new Error( "PICOPILOT_SELFTEST_SCRATCH is not an existing directory" );
      var path = dir + "/pcHrA.xisf";
      try
      {
         if ( !w.saveAs( path, false, false, false, false ) )
            throw new Error( "saveAs failed" );
         w.forceClose();
         var ws = ImageWindow.open( path );
         if ( ws.length < 1 )
            throw new Error( "open failed" );
         checkPhase( "j2.hr", { step: "reopen", id: ws[0].mainView.id } );
      }
      finally
      {
         if ( File.exists( path ) ) File.remove( path );
      }
   } )();
}
catch ( e )
{
   harnessError( "j2.hr", e );
}

jsMark( "fixture j2.utf8" );
// Review fix: a step whose parameter holds non-ASCII text (U+00E9 Latin-1,
// U+2014 BMP, the astral U+1F4F7), XML specials (< &&) and a newline, for the
// byte-exact round trip through HistoryReader. This file stays ASCII (\u
// escapes), and both texts travel to the module as hex UTF-16 code units:
// measured, PI's File.writeTextFile AND the core's recorded history change an
// astral character (U+1F4F7 comes back as U+1F4FD), so the payload carries
// the core's own record (read here, independently of HistoryReader) as the
// ground truth, plus what was sent.
try
{
   ( function ()
   {
      function codes( s ) { var r = []; for ( var i = 0; i < s.length; ++i ) r.push( s.charCodeAt( i ) ); return r; }
      var x = "iif($T<0.5 && 1,\n$T,0) /* \u00e9\u2014\ud83d\udcf7 */";
      var w = new ImageWindow( 16, 16, 1, 32, true, false, "pcHrUtf" );
      var p = new PixelMath; p.expression = x; p.executeOn( w.mainView );
      var pr = w.mainView.processing;
      var src = pr.at( pr.length - 1 ).toSource( "XPSM 1.0" );
      var open = "<parameter id=\"expression\">", a = src.indexOf( open ), b = src.indexOf( "</parameter>", a );
      var rec = a < 0 || b < 0 ? "" : src.substring( a + open.length, b ).replace( /&lt;/g, "<" ).replace( /&gt;/g, ">" )
                   .replace( /&quot;/g, "\"" ).replace( /&apos;/g, "'" ).replace( /&amp;/g, "&" );
      checkPhase( "j2.hr", { step: "utf8", sentCodes: codes( x ), recordedCodes: codes( rec ) } );
   } )();
}
catch ( e )
{
   harnessError( "j2.utf8", e );
}

jsMark( "fixture j2.long (500 steps)" );
// A 500-step history for Section J2's read-cost check.
try
{
   var hrLong = new ImageWindow( 32, 32, 1, 32, true, false, "pcHrLong" );
   for ( var i = 0; i < 500; ++i )
   {
      var hp = new PixelMath; hp.expression = "$T*1.0"; hp.executeOn( hrLong.mainView );
   }
}
catch ( e )
{
   harnessError( "j2.long", e );
}

jsMark( "fixture j6 (JourneyTracker)" );
// ---- J6 fixture phases (plan Task 7: JourneyTracker) ----
// Every step the tracker must record is made HERE; phase j6 {step} ticks
// Section J6's own tracker (temp library) in between and keeps the verdicts
// (J6State). Section J6 closes every window made here.
try
{
   ( function ()
   {
      function pm( id, x ) { var p = new PixelMath; p.expression = x; p.executeOn( View.viewById( id ) ); }
      function newImage( src, id )
      {
         var p = new PixelMath; p.expression = "$T"; p.createNewImage = true; p.newImageId = id;
         p.executeOn( View.viewById( src ) );
      }
      function j6( step, extra ) { var p = extra || {}; p.step = step; checkPhase( "j6", p ); }
      function closeIds( ids ) { ids.forEach( function( id ) { var w = ImageWindow.windowById( id ); if ( !w.isNull ) w.forceClose(); } ); }
      var existing = {};
      ImageWindow.windows.forEach( function( w ) { existing[w.mainView.id] = true; } );
      var dir = getEnvironmentVariable( "PICOPILOT_SELFTEST_SCRATCH" );
      if ( dir.length == 0 || !File.directoryExists( dir ) )
         throw new Error( "PICOPILOT_SELFTEST_SCRATCH is not an existing directory" );

      j6( "begin" );                                   // (b) ImageIntegration master -> pcTrkMaster
      pm( "pcTrkMaster", "$T*1.2" ); pm( "pcTrkMaster", "$T+0.01" );
      j6( "manual" );                                  // (c)
      var mv = View.viewById( "pcTrkMaster" );
      mv.historyIndex = mv.historyIndex - 1; j6( "undo" );   // (d)
      mv.historyIndex = mv.historyIndex + 1; j6( "redo" );
      mv.historyIndex = mv.historyIndex - 1;
      pm( "pcTrkMaster", "$T*0.95" ); pm( "pcTrkMaster", "$T*1.05" );
      j6( "branch" );
      j6( "copilotNote" ); pm( "pcTrkMaster", "$T+0.02" ); j6( "copilot" );   // (e)
      j6( "focus", { id: "pcTrkMaster" } ); newImage( "pcTrkMaster", "pcTrkClone" ); j6( "timing" );     // (f)
      j6( "focus", { id: "pcTrkMaster" } ); newImage( "pcTrkMaster", "pcTrkInherit" ); j6( "inherit" );  // (f2)
      j6( "focus", { id: "pcTrkMaster" } );                                  // (f3) seen before its history is attached
      pm( "pcTrkMaster", "$T*1.0" ); pm( "pcTrkMaster", "$T*1.0" ); newImage( "pcTrkMaster", "pcTrkLate" );
      j6( "late" );
      closeIds( [ "pcTrkLate" ] );

      var rgb = new ImageWindow( 48, 48, 3, 32, true, true, "pcTrkRgb" );     // (g)
      rgb.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ), new FITSKeyword( "OBJECT", "'TrkRGB'", "" ) ];
      j6( "rgbJoin" );
      var ce = new ChannelExtraction; ce.executeOn( rgb.mainView );
      j6( "rgb" );

      var cw = new ImageWindow( 48, 48, 3, 32, true, true, "pcTrkCC" );       // (h)
      var cc = new ChannelCombination;
      cc.channels = [ [ true, "pcTrkRgb_R" ], [ true, "pcTrkRgb_G" ], [ true, "pcTrkRgb_B" ] ];
      cc.executeOn( cw.mainView );
      var q = new ImageWindow( 64, 64, 1, 32, true, false, "pcTrkRef" );
      pm( "pcTrkRef", "pcTrkMaster*0.5" );
      j6( "reference" );

      var before = {};                                                        // (h2)
      ImageWindow.windows.forEach( function( w ) { before[w.mainView.id] = true; } );
      var gcc = new ChannelCombination;
      gcc.channels = [ [ true, "pcTrkRgb_R" ], [ true, "pcTrkRgb_G" ], [ true, "pcTrkRgb_B" ] ];
      gcc.executeGlobal();
      var gid = "";
      ImageWindow.windows.forEach( function( w ) { if ( !before[w.mainView.id] ) gid = w.mainView.id; } );
      j6( "ccGlobal", { id: gid } );

      j6( "copilotLink" );                                                    // (i)
      pm( "pcTrkMaster", "$T*1.01" ); j6( "defer" );                          // (j)
      j6( "rename" );                                                         // (k) -> pcTrkRenamed

      var path = dir + "/pcTrkRenamed.xisf";                                  // (l)
      if ( !ImageWindow.windowById( "pcTrkRenamed" ).saveAs( path, false, false, false, false ) )
         throw new Error( "saveAs " + path + " failed" );
      closeIds( [ "pcTrkClone", "pcTrkInherit", "pcTrkRef", "pcTrkCop", "pcTrkRenamed" ] );   // from JS, not C++
      j6( "reopenClose" );
      var ws = ImageWindow.open( path );
      if ( ws.length < 1 )
         throw new Error( "open " + path + " failed" );
      ws[0].show();
      // Observed: ImageWindow.open() from this (still running) script leaves the Process Console's abort
      // enabled, which the tracker's busy gate rightly reads as "a script is running". The fixture is the
      // user here: it ends the open as the GUI does, so the next phase's ticks are idle.
      console.abortEnabled = false;
      j6( "reopened" );
      pm( "pcTrkRenamed", "$T*0.99" ); j6( "reopenStep" );

      j6( "keywordOnly" );                                                    // (m)
      pm( "pcTrkRenamed", "$T*1.02" ); j6( "locked" );                        // (n)
      j6( "gapArm" );                                                         // (o)
      for ( var i = 0; i < 3; ++i )
      {
         pm( "pcTrkRenamed", "$T*1.0" );
         if ( i == 0 ) pm( "pcTrkRgb", "$T*1.0" );
         j6( "gapTick", { first: i == 0 } );
      }
      pm( "pcTrkRenamed", "$T*1.0" );   // the next change: the (real) reader now catches up
      j6( "gapEnd" );
      j6( "offBegin" ); pm( "pcTrkRenamed", "$T*1.0" ); j6( "off" );         // (p)

      var pw = ImageWindow.windowById( "pcTrkRenamed" );                      // (q)
      var pv = pw.createPreview( new Rect( 0, 0, 16, 16 ), "pcTrkPrev" );
      var pp = new PixelMath; pp.expression = "0"; pp.executeOn( pv );
      pw.deletePreview( pv );
      j6( "preview" );

      var bw = new ImageWindow( 9504, 6336, 3, 32, true, true, "pcTrkBig" );  // (r)
      bw.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ), new FITSKeyword( "OBJECT", "'TrkBig'", "" ) ];
      pm( "pcTrkBig", "0.1" );
      j6( "bigJoin" );
      pm( "pcTrkBig", "$T*1.1" ); j6( "big" );
      bw.forceClose(); bw = null;

      pm( "pcTrkRenamed", "$T*1.0" ); j6( "gate" );                          // review I4

      var uw = new ImageWindow( 32, 32, 1, 32, true, false, "pcTrkU" );      // review I3
      var fw = new ImageWindow( 16, 16, 1, 32, true, false, "pcTrkFile" );
      var fpath = dir + "/pcTrkFile.xisf";
      if ( !fw.saveAs( fpath, false, false, false, false ) )
         throw new Error( "saveAs " + fpath + " failed" );
      fw.forceClose(); fw = null;
      j6( "unrelatedMake" );
      pm( "pcTrkRenamed", "$T*1.0" ); pm( "pcTrkU", "$T*0.9" );
      var fo = ImageWindow.open( fpath );
      if ( fo.length < 1 )
         throw new Error( "open " + fpath + " failed" );
      console.abortEnabled = false;   // see the reopen above
      j6( "noFalseTiming", { fileId: fo[0].mainView.id } );
      fo = null;

      j6( "dupMaster" );                                                      // review I2
      j6( "joinFault" );                                                      // review I5
      j6( "startJourney" );                                                   // review I6

      [ true, false ].forEach( function( mc )                                 // (s)
      {
         j6( "scanMode", { mc: mc } );
         pm( "pcTrkRenamed", mc ? "$T*1.001" : "$T*1.002" );
         j6( "scanCheck", { mc: mc } );
      } );
      j6( "end" );                                                            // (t) + redaction
      // Every window this block made is closed here, from JS (not natively from C++ under JS wrappers).
      var made = [];
      ImageWindow.windows.forEach( function( w ) { if ( !existing[w.mainView.id] ) made.push( w.mainView.id ); } );
      closeIds( made );
   } )();
}
catch ( e )
{
   harnessError( "j6", e );
}

// ---- fixture phases end (add new phases above this line, each block starting with jsMark( "fixture <id>" )) ----

jsMark( "final executeGlobal (the full self-test)" );
try
{
   var P = new PICopilot;       // fails here if the process id isn't registered
   P.executeGlobal();           // C++ writes $PICOPILOT_SELFTEST_OUT
}
finally
{
   jsMark( null );
}
