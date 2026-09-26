// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ProcessActivity_h
#define PICopilot_ProcessActivity_h

#include <pcl/String.h>

namespace pcl
{

/*
 * "Is PixInsight still executing a process?" -- the signal the tool loop
 * waits on before it runs a tool that can change an image (Task T-hist).
 *
 * Why: a process executed while PixInsight is still inside ANOTHER process
 * execution changes the pixels but is NOT recorded in History (so Undo
 * cannot revert it) and ModifyCount does not advance. A module Timer tick
 * can fire there, because the running process (and the core's work after
 * it, before the call returns) pumps events. Measured headlessly with 10 ms
 * ticks (task-hist-report.md):
 *   - applied blindly while a PixelMath ran on ANOTHER 4000x4000 view:
 *     6/6 unrecorded; the view is write-locked and Console::AbortEnabled()
 *     is true meanwhile;
 *   - a global PixelMath (createNewImage): AbortEnabled() stays true for the
 *     whole execution INCLUDING the ~200 ms after its views were unlocked;
 *     gated on "abort enabled || a view locked": 10/10 recorded;
 *   - that gate alone still let 10/10 applies land unrecorded in the last
 *     tick of a view process (views already unlocked, the call not yet
 *     returned); that tick always carried image notifications
 *     (ImageLocked/ImageUnlocked/ImageUpdated), hence the quiet period:
 *     with it, 10/10 (view process) + 10/10 (global process) recorded.
 * PCL has no "a process is executing" API, so this is a composite of what
 * IS observable. It is BUSY when any of:
 *   1. the Process Console's abort is enabled (a process that runs with a
 *      status monitor, or a script that enabled abort, is executing);
 *   2. any open view (main view or preview, iconic windows included) is
 *      locked for reading or writing (non-blocking CanRead()/CanWrite());
 *   3. less than PICopilotApplyQuietSeconds have passed since the last
 *      process activity: an image notification (the interface forwards every
 *      one) or the end of a PICopilot process execution.
 * Known gap (measured 10/10 unrecorded with this gate, before PICopilot's
 * own executions were stamped): a global process that enables no abort,
 * locks no view and sends no notification is invisible; its short tail
 * (the first tick, ~3 ms after it) is left to ApplyProcess's DETECT check
 * (ProcessApply.h), which turns such an apply into a loud error. Root
 * thread only.
 */

// Minimum quiet time after the last process activity before an image may be
// changed (measured: 0.05 s already sufficed 3/3; 0.3 s = 3 panel ticks of
// margin, 10/10 + 10/10).
constexpr double PICopilotApplyQuietSeconds = 0.3;

// The tool loop tells the user it is waiting once a deferral lasts this long.
constexpr double PICopilotBusyWaitNoteSeconds = 3.0;

// Records process activity NOW. Called from every image notification the
// interface receives and when a PICopilot process execution ends. Handlers
// only record a time stamp here; nothing is read. Root thread.
void NoteProcessActivity();

struct ProcessActivityState
{
   bool   busy = false;
   String reason;   // plain text for the chat log, e.g. "image M42 is locked by a running process"
};

// Non-blocking; never throws (an exception while probing counts as busy).
ProcessActivityState CurrentProcessActivity();

} // namespace pcl

#endif // PICopilot_ProcessActivity_h
