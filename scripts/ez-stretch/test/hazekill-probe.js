// EZHazeKill headless parity probe. Test-only, never shipped. ES5 only.
//
// Includes the shipped script with EZHAZEKILL_NO_MAIN defined, so the code
// under test is exactly EZHazeKill.js's hkApply() (sky measurement, model and
// the PixelMath it runs), then for every case: open the input, apply, save.
//
// Input:  $HK_PROBE_IN  -- { "cases": [ { name, input, output, amount, neutralize, target } ] }
// Output: $HK_PROBE_OUT -- { "ok": bool, "version": "...", "cases": [ { name, ok, error?,
//                            sky[], sigma[], cast[], offset[], eps, tEff, ped, stretched,
//                            tiles, skyTiles, seconds } ] }

#define EZHAZEKILL_NO_MAIN
#include "../src/scripts/EZ Stretch BSC/EZHazeKill.js"

function hkProbeMain()
{
   var inPath = getEnvironmentVariable( "HK_PROBE_IN" );
   var outPath = getEnvironmentVariable( "HK_PROBE_OUT" );
   var result = { ok: true, version: VERSION, cases: [] };
   try
   {
      var cfg = JSON.parse( File.readTextFile( inPath ) );
      for ( var i = 0; i < cfg.cases.length; ++i )
      {
         var c = cfg.cases[i];
         var r = { name: c.name, ok: false };
         result.cases.push( r );
         var w = null;
         try
         {
            var ws = ImageWindow.open( c.input );
            if ( ws.length != 1 )
               throw new Error( "opening " + c.input + " gave " + ws.length + " windows" );
            w = ws[0];
            var t0 = Date.now();
            var m = hkApply( w.mainView, c.amount, c.neutralize, c.target );
            r.seconds = ( Date.now() - t0 )/1000;
            r.sky = m.sky; r.sigma = m.sigma; r.cast = m.cast; r.offset = m.offset;
            r.eps = m.eps; r.tEff = m.tEff; r.ped = m.ped; r.stretched = m.stretched;
            r.tiles = m.tiles; r.skyTiles = m.skyTiles;
            if ( !w.saveAs( c.output, false, false, false, false ) )
               throw new Error( "saveAs " + c.output + " failed" );
            r.ok = true;
         }
         catch ( e )
         {
            r.error = e.toString();
            result.ok = false;
         }
         if ( w != null )
            w.forceClose();
      }
   }
   catch ( e )
   {
      result.ok = false;
      result.error = e.toString();
   }
   File.writeTextFile( outPath, JSON.stringify( result, null, 1 ) );
}

hkProbeMain();
