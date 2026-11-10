// EZTools GUI smoke (-x IPC), last step: save the image EZHazeKill's Apply button corrected to
// $EZT_GUI_DIR/executed.xisf, then close every image window without asking (so File > Quit does not
// stop on "save changes?"). Writes save.done. ES5.

#include "gui-smoke-common.js"

eztIpc( "save", function()
{
   var w = ImageWindow.windowById( "ezt_smoke" );
   if ( w.isNull )
      throw new Error( "no window ezt_smoke" );
   if ( !w.saveAs( EZT_GUI_DIR + "/executed.xisf", false, false, false, false ) )
      throw new Error( "saveAs failed" );
   var ws = ImageWindow.windows;
   for ( var i = 0; i < ws.length; ++i )
      ws[i].forceClose();
   return "saved";
} );
