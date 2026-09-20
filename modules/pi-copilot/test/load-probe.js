// Proves PI got past module load. Writes to a private, unpredictable path
// passed by run-load.sh via PICOPILOT_LOAD_OUT (defense-in-depth against a
// planted /tmp symlink — this script is test-only, never shipped).
var out = getEnvironmentVariable( "PICOPILOT_LOAD_OUT" );
var f = new File; f.createForWriting( out ); f.outTextLn( "loaded" ); f.close();
