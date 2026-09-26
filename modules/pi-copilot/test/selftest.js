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

// ---- fixture phases end (add new phases above this line) ----

var P = new PICopilot;          // fails here if the process id isn't registered
P.executeGlobal();              // C++ writes $PICOPILOT_SELFTEST_OUT
