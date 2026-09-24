// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilotSelfTest_h
#define PICopilotSelfTest_h

#include <pcl/String.h>

namespace pcl
{

// Proves the hybrid execution paths from C++:
//   1. PJSR via MetaModule::EvaluateScript
//   2. native ProcessInstance construction against the process registry
//   3. pcl::Settings round-trip (local space) on a throwaway key
//   4. AnthropicClient against the real API -- gated on the
//      PICOPILOT_TEST_API_KEY env var; skipped (and counted as passing)
//      when it's unset, so CI without a key still passes.
//   5. ChatThread / AnthropicClient::Send on a worker pcl::Thread with an
//      invalid key -- must come back ok=false, httpStatus=401 (proves the
//      off-root-thread Control sink + NetworkTransfer POST work).
// Populates jsonOut with
// {evalResult, evalOk, processInstanceValid, keyStoreOk, anthropicOk,
//  anthropicSkipped, workerThreadOk, workerHttpStatus, workerError, ok}
// and returns ok. Root-thread only (EvaluateScript requirement).
bool RunSelfTest( String& jsonOut );

} // namespace pcl

#endif // PICopilotSelfTest_h
