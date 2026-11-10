// EZTools GUI smoke: helpers shared by the -r setup script and the -x IPC scripts. Test-only. ES5.

var EZT_GUI_DIR = getEnvironmentVariable( "EZT_GUI_DIR" );

function eztPump( ms )
{
   var t0 = Date.now();
   while ( Date.now() - t0 < ms )
   {
      processEvents();
      msleep( 20 );
   }
}

// PixInsight's linked AutoSTF (shadows -2.8 MADN, target background 0.25).
function eztAutoSTF( view )
{
   var img = view.image;
   var n = img.numberOfChannels;
   var med = 0, madn = 0;
   for ( var c = 0; c < n; ++c )
   {
      img.selectedChannel = c;
      med += img.median();
      madn += 1.4826*img.MAD();
   }
   img.resetSelections();
   med /= n;
   madn /= n;
   var c0 = Math.max( 0, med + -2.8*madn );
   var m = Math.mtf( 0.25, med - c0 );
   var stf = new ScreenTransferFunction;
   var row = [c0, 1, m, 0, 1];
   stf.STF = [row, row, row, [0, 1, 0.5, 0, 1]];
   stf.executeOn( view );
}

// Runs body(); writes <tag>.done with its result text, or <tag>.error with the exception.
function eztIpc( tag, body )
{
   try
   {
      var text = body();
      File.writeTextFile( EZT_GUI_DIR + "/" + tag + ".done", String( text === undefined ? "" : text ) );
   }
   catch ( e )
   {
      File.writeTextFile( EZT_GUI_DIR + "/" + tag + ".error", String( e ) );
   }
}
