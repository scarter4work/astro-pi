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
// Populates jsonOut with
// {evalResult, evalOk, processInstanceValid, keyStoreOk, ok}
// and returns ok. Root-thread only (EvaluateScript requirement).
bool RunSelfTest( String& jsonOut );

} // namespace pcl

#endif // PICopilotSelfTest_h
