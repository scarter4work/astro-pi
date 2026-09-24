// Self-test harness driver. C++ ExecuteGlobal() runs the self-test and writes
// the result JSON to the private mktemp path the harness passes in
// PICOPILOT_SELFTEST_OUT (see PICopilotInstance.cpp); this script just drives
// the process.
var P = new PICopilot;          // fails here if the process id isn't registered
P.executeGlobal();              // C++ writes $PICOPILOT_SELFTEST_OUT
