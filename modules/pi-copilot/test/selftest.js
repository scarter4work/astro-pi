// Increment-1 harness. C++ ExecuteGlobal() now owns writing the result
// file (see PICopilotSelfTest.cpp); this script just drives the process.
var P = new PICopilot;          // fails here if the process id isn't registered
P.executeGlobal();              // C++ writes /tmp/.picopilot_selftest.json
