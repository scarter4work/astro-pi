// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_UndoSelfTest_h
#define PICopilot_UndoSelfTest_h

#include <nlohmann/json.hpp>

namespace pcl
{

// Section JU: the history_step tool (undo / redo). Reads the fixture windows
// test/selftest.js made at top level (pcUndoUnit, pcUndoOther, pcUndoLive)
// and the "undo" phase results (SelfTestPhaseStore()["undo"]). Called from
// RunSelfTest() after the journey block. Root thread only.
bool RunUndoSelfTest( nlohmann::json& out );

// Phase "undo" (test/selftest.js "fixture undo (history_step)"): a private
// journey library + tracker that follows two keyword masters, one stepped by
// the USER (top-level historyIndex writes) and one by the history_step TOOL,
// compared after every step. payload.step: begin | recorded | toolUndo |
// toolRedo | compare | end. Registered in SelfTestPhaseHandlers().
nlohmann::json PhaseUndoTool( const nlohmann::json& payload );

} // namespace pcl

#endif // PICopilot_UndoSelfTest_h
