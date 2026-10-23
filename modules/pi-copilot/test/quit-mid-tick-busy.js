// PI Copilot quit-mid-tick test (test/quit-mid-tick.sh), step 2 (-x IPC): the images are
// closed (no save prompt at Quit), then the main thread is kept busy WITHOUT pumping
// events for 3 s. The driver clicks File > Quit meanwhile, so when the event loop
// resumes the journey timer and that click are both pending: the timer is dispatched
// first, and its tick's busy gate (Console::AbortEnabled() pumps events) handles the
// Quit -- PixInsight runs every module's OnUnload inside JourneyTracker::Tick.
var all = ImageWindow.windows;
for ( var i = 0; i < all.length; ++i )
   all[i].forceClose();
var t0 = Date.now(), x = 0;
while ( Date.now() - t0 < 3000 )
   x += Math.sqrt( t0 + x );
