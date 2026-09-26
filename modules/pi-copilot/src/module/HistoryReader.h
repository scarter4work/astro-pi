// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_HistoryReader_h
#define PICopilot_HistoryReader_h

#include <pcl/String.h>
#include <pcl/XML.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pcl
{

// One processing-history step, parsed from its XPSM 1.0 serialization.
// parameters/tableParameters are in apply_process form (Ruling 3): typed via
// the INSTALLED process's parameter list, read-only parameters dropped.
struct HistoryStep
{
   int            combinedIndex = -1;          // 0-based over initialProcessing ++ processing
   std::string    processId;                   // XPSM class, e.g. "PixelMath", "Script"
   std::string    xpsm;                        // the instance element exactly as PI wrote it
   std::string    started;                     // <time start>, ISO 8601 UTC; "" when absent (Script steps)
   double         durationS = -1;              // <time span>; -1 when absent
   nlohmann::json parameters = nlohmann::json::object();
   nlohmann::json tableParameters = nlohmann::json::object();
   std::string    maskId;                      // "" = no mask
   bool           maskInverted = false;
   bool           replayable = true;
   std::string    parseNote;                   // why not replayable ("" otherwise)
   std::string    identity;                    // StepIdentity() (Ruling 4)
   // Ruling 29 (P35): the raw value of an integrationImageId parameter in the
   // XPSM, captured BEFORE read-only parameters are dropped ("" when absent).
   // ImageIntegration/DrizzleIntegration write the RESULT window's id there.
   std::string    integrationImageId;
};

// A view's history counts plus the steps read from combined index `from`.
struct HistorySnapshot
{
   bool   ok = false;
   // Another EvaluateScript is in progress (EvaluateScriptDepth() > 0,
   // EvalGuard.h): nothing was read. Not an error -- retry on the next tick.
   // ok == false, error empty, steps empty.
   bool   busy = false;
   String error;
   int    initialLength = 0;   // View.initialProcessing.length (history loaded from the file)
   int    length = 0;          // View.processing.length (this session, redoable steps included)
   int    historyIndex = 0;    // View.historyIndex (processing steps currently applied)
   int    from = 0;            // combined index of steps[0]
   // Ruling 27: the extra entry a save + reopen adds (Task 1 measured it) was
   // skipped; initialLength and every combined index exclude it.
   bool   droppedReopenExtra = false;
   std::vector<HistoryStep> steps;

   int ActiveCount() const { return initialLength + historyIndex; }
   int TotalCount() const  { return initialLength + length; }
};

// A recorded step of one image (non-superseded rows are what matter).
struct KnownStep
{
   int64       id = 0;
   int         seq = 0;          // combined index + 1
   std::string identity;
   std::string state;            // "active" | "undone" | "superseded"
};

struct HistoryDiff
{
   bool                     needFullRead = false;   // re-read with from = 0 and diff again (Ruling 5)
   std::vector<int64>       toActive;
   std::vector<int64>       toUndone;
   std::vector<int64>       toSuperseded;
   std::vector<HistoryStep> appended;
   std::vector<std::string> appendedState;          // "active" | "undone", parallel to appended
};

// processId + "@" + started + "#" + 16 hex digits of FNV-1a-64 over the
// canonical (key-sorted) JSON {"p": parameters, "t": tableParameters}.
std::string StepIdentity( const std::string& processId, const std::string& started,
                          const nlohmann::json& parameters, const nlohmann::json& tableParameters );

// FNV-1a-64 of the bytes of s as 16 lower-case hex digits. The one copy in the
// module: StepIdentity uses it, and Task 6's MasterFingerprint reuses it.
std::string Fnv1a64Hex( const std::string& s );

// Parses one XPSM <instance> element. Numbers parse locale-independently
// (std::from_chars); a <time span> that is not a number fails the step with a
// precise error; a boolean other than "true"/"false" (or a malformed number)
// makes the step not replayable with a precise parseNote. Root thread only (process catalog).
// Never throws: false + error for an element that is not a process instance.
// A step that parses but cannot be replayed faithfully is ok with
// replayable=false and parseNote (uninstalled process, Script step, block
// parameter, a parameter the installed process does not know, a table cell
// missing).
bool ParseXpsmElement( const XMLElement& instance, HistoryStep& step, String& error );

// Parses XPSM text holding exactly one <instance> element (sets step.xpsm).
bool ParseXpsmStep( const std::string& xpsm, HistoryStep& step, String& error );

// Reads a MAIN view's history through EvaluateScript: counts, then every
// step from combined index `from` (clamped to >= 0) to the end, each with its
// mask. Ruling 27: when PICopilotJourneyReopenExtraSteps == 1, the view's
// window has a file path, and the initialProcessing entry at the measured end
// (first when ...Leads, else last) has ...ProcessId, that entry is skipped:
// initialLength and every combined index exclude it (droppedReopenExtra).
// Root thread only; the caller has already checked the view is not busy.
// Never re-enters EvaluateScript: while another EvaluateScript is running
// (EvaluateScriptDepth() > 0) it returns at once with busy=true, ok=false and
// no error -- the caller retries on its next tick; otherwise it holds an
// EvalDepthGuard for its own call.
// Never throws: ok=false + error ("no view <id>", a parse error naming the
// step index, a script error).
HistorySnapshot ReadViewHistory( const IsoString& viewFullId, int from );

// max( 0, highest non-superseded seq - 1 ): re-read the last known step.
int HistoryReadFrom( const std::vector<KnownStep>& known );

// Pure (Ruling 5). A snapshot that is not ok (busy or failed) yields an
// EMPTY diff: no state change, needFullRead false -- retry the read later.
// known: this image's recorded rows (superseded ones are ignored). If snap.from > 0 and the step read at snap.from does not match
// the known step at that seq, needFullRead is set and nothing else. Otherwise:
// the first mismatch m (combined index) is found; known rows with seq > m are
// superseded; read steps with index >= m are appended; every surviving known
// row gets the state its seq implies (seq <= ActiveCount -> active, else undone).
HistoryDiff DiffHistory( const std::vector<KnownStep>& known, const HistorySnapshot& snap );

} // namespace pcl

#endif // PICopilot_HistoryReader_h
