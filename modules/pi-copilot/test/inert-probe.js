// PI Copilot check-release-inert.sh: executeGlobal() with every PICOPILOT_SELFTEST_* variable set
// (by the harness); records what happened. Test-only, never shipped.
var out = getEnvironmentVariable( "INERT_OUT" );
var r = [];
try
{
   var ok = ( new PICopilot ).executeGlobal();
   r.push( "executeGlobal=" + ok );
}
catch ( e )
{
   r.push( "executeGlobal threw: " + String( e ) );
}
var verdict = getEnvironmentVariable( "PICOPILOT_SELFTEST_OUT" );
r.push( "selftestOutWritten=" + File.exists( verdict ) );
r.push( "phaseFileLeft=" + File.exists( getEnvironmentVariable( "PICOPILOT_SELFTEST_PHASE" ) ) );
var f = new File; f.createForWriting( out ); f.outTextLn( r.join( "\n" ) ); f.close();
