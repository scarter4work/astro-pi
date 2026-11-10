// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ChildProcess_h
#define PICopilot_ChildProcess_h

// PCL-free on purpose (unit-tested without PixInsight: test/unit/test_child_process.cpp).

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace picopilot
{

struct ChildRun
{
   bool        started = false;    // the program was executed
   bool        finished = false;   // it ran to an exit (or could not be found: exitCode 127/126, like env)
   int         exitCode = -1;      // exit status; 128+signal when it crashed
   bool        crashed = false;    // ended by a signal
   bool        timedOut = false;   // killed at the deadline
   bool        inputError = false; // stdin could not be fully written (the child exited or closed it first)
   std::string out;                // the child's stdout, complete
   std::string err;                // the child's stderr, separately, at most maxErrBytes
   std::string error;              // why it did not finish (start failure, timeout); never contains input
};

// Runs `program` with `args` directly: posix_spawnp (PATH search when the name
// has no '/'), argv passed as-is, no shell and no other interpreter in between.
// The child gets the caller's environment minus LD_LIBRARY_PATH (PixInsight's
// own library path breaks system binaries), default signal dispositions and an
// empty signal mask. stdin carries `*input` and then end-of-file (or is
// /dev/null when input is null); stdout and stderr are separate pipes, so the
// two are never mixed. A program that cannot be found ends as finished with
// exitCode 127 (126 when it is not executable), exactly as /usr/bin/env did.
//
// Blocks until the child exits or timeoutMs passes (then SIGKILL, reaped).
// `poll` is called about every 20 ms while waiting (event pumping, wait
// notices). Never throws on child failure; never raises SIGPIPE.
ChildRun RunChild( const std::string& program, const std::vector<std::string>& args,
                   const std::string* input, int timeoutMs,
                   const std::function<void()>& poll, std::size_t maxErrBytes );

} // namespace picopilot

#endif // PICopilot_ChildProcess_h
