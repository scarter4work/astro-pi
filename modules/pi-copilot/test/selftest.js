// Increment-1 harness. Writes a JSON verdict; console output never reaches
// stdout under --automation-mode.
var RESULT = "/tmp/.picopilot_selftest.json";
function write(o){ var f=new File; f.createForWriting(RESULT); f.outTextLn(JSON.stringify(o)); f.close(); }
try {
   var P = new PICopilot;          // fails here if the process id isn't registered
   var ok = P.executeGlobal();     // Task 3 makes this run the self-test
   write({ processConstructed: true, executeGlobalReturned: ok });
} catch (e) {
   write({ error: String(e) });
}
