// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_HistoryStepTool_h
#define PICopilot_HistoryStepTool_h

#include "AgentTools.h"
#include "HistoryReader.h"

#include <functional>

#include <pcl/String.h>

#include <nlohmann/json.hpp>

namespace pcl
{

/*
 * history_step {direction: "undo"|"redo", count, view_id}: steps a view back
 * or forward through its OWN History, exactly like Edit > Undo / Redo (the
 * History Explorer), so the model can "undo the last two steps and try X
 * instead".
 *
 * API (measured headless, PI 1.9.5, slot 98, 2026-09-27): PCL has no undo /
 * redo call. PJSR has ImageWindow.undo() / redo() / undoAll() / redoAll()
 * (window level, no count, no target view) and View.historyIndex, which is
 * WRITABLE: assigning it moves the view to that position -- the pixels
 * follow, View.processing keeps its length (the redo tail stays), and
 * canGoBackward / canGoForward follow. An out-of-range assignment (above
 * processing.length, or negative) is SILENTLY IGNORED, so the tool validates
 * the count itself before assigning. historyIndex counts only this session's
 * steps (View.processing); the initialProcessing entries (the creation entry,
 * or the history loaded with a file) cannot be undone -- same as the GUI.
 * A preview has its own View.processing / historyIndex (Preview > Undo):
 * the tool steps THAT history and never the main image's; preview steps are
 * not part of an image journey (Ruling 25).
 *
 * Modes: Copilot steps at once (the user can step back with Redo / Undo);
 * Guided asks first (ToolContext::confirmHistory); Advisor refuses. A busy
 * or locked view, and a running script / EvaluateScript, are refused, never
 * waited for. The count is validated as int64 BEFORE it is compared with the
 * depth and narrowed (Task 10 R1). The result names every stepped History
 * entry (process id and step number) and the new position. No journey note
 * is posted: the journey tracker follows the moved historyIndex exactly as it
 * follows a user's undo / redo.
 *
 * Root thread only. Holds no View / ImageWindow beyond the call: the target is
 * re-resolved by id after the (event-pumping) confirmation dialog.
 */

// The Anthropic tool definition (offered in Copilot and Guided only).
nlohmann::json HistoryStepToolDefinition();

// Never throws; every failure is isError with a precise, model-correctable message.
ToolOutcome ExecuteHistoryStepTool( const nlohmann::json& input, const ToolContext& ctx );

// The Guided confirmation dialog's text (HTML): "Undo 2 History steps on
// <view>?" ("1 History step" for one; "Redo ..." for redo), then the (escaped)
// entries that would be stepped, one per line: "#<step> <process>, <HH:MM:SS>
// UTC: <parameter hint>" (hint path-free, <= 60 chars), then what can be
// taken back afterwards.
String HistoryStepDialogHtml( bool undo, int count, const String& viewId, const String& stepsText );

// Self-test seams (never set in production). The offset is added to the
// position the tool writes (proves the loud "did not move" branch); the hook
// edits the snapshot re-read after the Guided dialog (proves the identity
// re-check). Root thread only.
void SetHistoryStepIndexOffsetForSelfTest( int offset );
void SetHistoryStepRereadHookForSelfTest( std::function<void( HistorySnapshot& )> hook );

} // namespace pcl

#endif // PICopilot_HistoryStepTool_h
