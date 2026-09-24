// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilotSelfTest_h
#define PICopilotSelfTest_h

#include <pcl/String.h>

namespace pcl
{

// Test-only: PICopilotInstance::ExecuteGlobal() calls this ONLY when the
// harness sets PICOPILOT_SELFTEST_OUT. Proves from C++:
//   1. PJSR via MetaModule::EvaluateScript
//   2. native ProcessInstance construction against the process registry
//   3. pcl::Settings round-trip (local space) on a throwaway key
//      ("PICopilot/SelfTestKey" -- never the user's stored API key)
//   4. AnthropicClient against the real API -- gated on the
//      PICOPILOT_TEST_API_KEY env var; skipped (and counted as passing)
//      when it's unset, so CI without a key still passes.
//   5. ChatThread with an invalid key: the AnthropicRequest (Control sink +
//      NetworkTransfer) is built on the root thread, the worker thread only
//      Perform()s the POST + parse -- must come back ok=false, 401.
//   6. Cancel/deadline against a local server that accepts and never
//      replies (PICOPILOT_SELFTEST_STALL_URL, set by the harness; reported
//      as stallSkipped when unset): RequestCancel() mid-stall must end the
//      turn with "request cancelled", and a 3 s deadline must end it with
//      "request timed out after 3 s" -- both well inside their bounds.
//   7. PICopilotInterface::PlainText() keeps a literal "</raw>" literal in
//      a real TextBox.
//   8. Increment-3 vision/grounding sections (PICopilotVisionSelfTest.cpp).
// Populates jsonOut with
// {evalResult, evalOk, processInstanceValid, keyStoreOk, anthropicOk,
//  anthropicSkipped, workerThreadOk, workerHttpStatus, workerError,
//  stallSkipped, cancelOk, cancelError, cancelSeconds, deadlineOk,
//  deadlineError, deadlineSeconds, plainTextOk, plainTextBack,
//  visionSmokeOk, smokeWindowOk, smokeReadOk, smokeRenderOk, smokeJpegOk,
//  smokeTempRemoved, smokeError, ok}
// and returns ok. Root-thread only (EvaluateScript and Control
// construction requirement).
bool RunSelfTest( String& jsonOut );

} // namespace pcl

#endif // PICopilotSelfTest_h
