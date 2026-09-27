// PI Copilot GUI smoke (test/gui-smoke.sh) -- shared helpers of its scripts.
var DIR = getEnvironmentVariable( "PICOPILOT_GUI_DIR" );
var PHASE_FILE = getEnvironmentVariable( "PICOPILOT_SELFTEST_PHASE" );

function pump( ms )
{
   var t0 = Date.now();
   while ( Date.now() - t0 < ms )
   {
      processEvents();
      msleep( 20 );
   }
}

function mark( name, text )
{
   File.writeTextFile( DIR + "/" + name, text === undefined ? String( Date.now() ) : text );
}

// Runs a module phase handler (PICopilotJourneySelfTest.cpp) in-process.
function phase( id, payload )
{
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
