// PI Copilot quit-in-turn test (test/quit-in-turn.sh keepmodal), -x: after the modal keep confirm closed,
// the panel keeps the focus and PixInsight's Ctrl+Q shortcut does not reach the main window (measured);
// bring the image back to the front so the final Quit is the main window's.
ImageWindow.windowById( "pcTurnM" ).bringToFront();
