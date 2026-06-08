/*
 * HazeKill.js  —  flat-background haze detector + corrector for PixInsight
 *
 * Use AFTER background extraction (GraXpert / DBE / GradientCorrection), on a
 * LINEAR image, when a flat-but-elevated, slightly-colored background remains
 * (the classic moon-washed "milky haze"). It:
 *   1. measures each channel's background (median = good proxy on a
 *      background-dominated frame),
 *   2. reports the per-channel levels and the color-cast spread,
 *   3. subtracts each channel down to a common neutral TARGET via PixelMath
 *      (full undo history; the expressions are visible in the process icon).
 *
 * It does NOT model a gradient — if the background still varies across the
 * frame, run GraXpert/DBE first. This only kills a flat per-channel pedestal.
 *
 * ASSUMES the target object does not fill most of the frame (median ~ sky).
 * For a big nebula/galaxy that dominates the frame, the median is biased high
 * and this will over-subtract — use a manual measurement instead.
 *
 * Run:  Script > Execute Script File…  (with the linear image active)
 */

#feature-id    HazeKill : Utilities > HazeKill

#define TARGET 0.05   // neutral background level all channels are moved to

function main()
{
   var win = ImageWindow.activeWindow;
   if ( win.isNull )
   {
      Console.criticalln( "HazeKill: no active image window." );
      return;
   }
   var view = win.mainView;
   var img  = view.image;
   var nc   = img.isColor ? 3 : 1;

   // --- measure per-channel background (median over the whole image) ---
   var bg = new Array( nc );
   for ( var c = 0; c < nc; ++c )
      bg[c] = img.median( new Rect, c, c );   // empty Rect = whole image, channel c..c

   // --- report ---
   var labels = img.isColor ? ["R","G","B"] : ["K"];
   Console.writeln( "<end><cbr>========== HazeKill ==========" );
   for ( var c = 0; c < nc; ++c )
      Console.writeln( format( "  %s background: %.5f", labels[c], bg[c] ) );

   if ( img.isColor )
   {
      var lo = Math.min( bg[0], bg[1], bg[2] );
      var hi = Math.max( bg[0], bg[1], bg[2] );
      Console.writeln( format( "  color-cast spread (max-min): %.5f%s",
                               hi - lo, (hi - lo) > 0.005 ? "   <-- cast present" : "   (already neutral)" ) );
   }
   Console.writeln( format( "  moving all channels -> %.3f", TARGET ) );

   // --- build the PixelMath correction:  $T - bg_c + TARGET  per channel ---
   var P = new PixelMath;
   P.useSingleExpression = !img.isColor;
   if ( img.isColor )
   {
      P.expression  = format( "$T - %.5f + %.5f", bg[0], TARGET );  // R
      P.expression1 = format( "$T - %.5f + %.5f", bg[1], TARGET );  // G
      P.expression2 = format( "$T - %.5f + %.5f", bg[2], TARGET );  // B
   }
   else
      P.expression  = format( "$T - %.5f + %.5f", bg[0], TARGET );

   P.rescale        = false;
   P.truncate       = true;    // clip out-of-range to [0,1]
   P.createNewImage = false;   // edit in place (undoable)

   Console.writeln( "  PixelMath:" );
   Console.writeln( "    R/K: " + P.expression );
   if ( img.isColor ) {
      Console.writeln( "    G:   " + P.expression1 );
      Console.writeln( "    B:   " + P.expression2 );
   }

   P.executeOn( view );

   Console.writeln( "  done — background neutralized. (Ctrl+Z to undo)" );
   Console.writeln( "==============================" );
}

main();
