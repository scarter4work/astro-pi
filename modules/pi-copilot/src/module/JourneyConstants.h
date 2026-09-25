// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_JourneyConstants_h
#define PICopilot_JourneyConstants_h

namespace pcl
{

// Every value below was DERIVED from plan Task 1's measurements
// (self-test key journeySpikeInfo). Changing one requires re-measuring.
// Measured 2026-09-25 (multi-phase harness, top-level history):
//   closedCreated/closedUpdated true/true; timerFromOnLoad true;
//   modifyCount 0,1,0,1 (start/step/undo/redo) on hidden AND shown windows;
//   stats60 blockMs stride 1/2/4 = 78.9/44.4/30.3; history500 readTailMs 1.29
//   (readAllMs 20.5, parseAllMs 10.2); reopen initialLength 6 == saved 6,
//   0 unmatched; keywordsInherited PixelMath false, ChannelExtraction true;
//   iiIdInHistory true. Budget: max(250, 50*ceil(1.5*(1.29+78.9+60)/50)) = 250.

// D6 (Ruling 22): true iff ImageCreated + ImageUpdated for pcJourneyPre reached
// the NEVER-OPENED interface in the pre-phase (journeySpikeInfo.closedCreated &&
// .closedUpdated).
constexpr bool   PICopilotJourneyNotificationsWork = true;

// Tracker timer period; the dirty queue is processed every tick.
constexpr double PICopilotJourneyTickSeconds = 0.5;

// Backstop scan period: 2.0 when notifications work, else 1.0 (Ruling 22).
constexpr double PICopilotJourneyScanSeconds = PICopilotJourneyNotificationsWork ? 2.0 : 1.0;

// true iff ModifyCount() changed on step, undo AND redo (journeySpikeInfo.modifyCount
// .tracksUndo). false: the scan batches historyIndex/length via one EvaluateScript.
constexpr bool   PICopilotJourneyScanUsesModifyCount = true;

// true iff the OnLoad timer ticked (journeySpikeInfo.timerFromOnLoad). false:
// JourneyService::Start() runs from the first image notification instead.
constexpr bool   PICopilotJourneyServiceStartsOnLoad = true;

// Smallest of 1, 2, 4 whose 60 MP RGB float block average took <= 500 ms
// (journeySpikeInfo.stats60.blockMsByStride). Ruling 7.
constexpr int    PICopilotJourneyStatsRowStride = 1;

// Per-step budget (ms) Task 7 asserts end to end on 60 MP:
// max( 250, 50*ceil( 1.5*( readTailMs + blockMs(stride) + 60 )/50 ) ).
constexpr int    PICopilotJourneyStepBudgetMs = 250;

// Timing-evidence slack after a step's end (Ruling 19).
constexpr double PICopilotJourneyTimingSlackSeconds = PICopilotJourneyScanSeconds + 1.0;

// Ruling 27 (P15): entries a save + reopen adds to initialProcessing beyond the
// saved history (journeySpikeInfo.reopenInitialLength - .reopenSavedLength).
// 0 when equal; 1 when one more (then the two values below describe it);
// anything else is BLOCKED. Consumed by Task 3's ReadViewHistory.
constexpr int         PICopilotJourneyReopenExtraSteps     = 0;
// When ExtraSteps == 1: journeySpikeInfo.reopenExtraProcessId ("" otherwise).
constexpr const char* PICopilotJourneyReopenExtraProcessId = "";
// When ExtraSteps == 1: journeySpikeInfo.reopenExtraLeads (true: it is
// initialProcessing[0]; false: the last entry). Ignored when ExtraSteps == 0.
constexpr bool        PICopilotJourneyReopenExtraLeads     = true;

// Ruling 28 (P14): journeySpikeInfo.keywordsInheritedPixelMath ||
// .keywordsInheritedChannelExtraction. Informational: Ruling 28's evaluation
// order (link evidence before the keyword master rules for a derived window)
// applies either way, and Task 7's J6 forces the inherited case.
constexpr bool        PICopilotJourneyCreatedWindowsInheritKeywords = true;

// Ruling 29 (P35): journeySpikeInfo.iiIdInHistory -- the result window's first
// history step carries integrationImageId == its own id and every auxiliary
// output's carries a different id. Consumed by Task 6's IsIntegrationAuxiliary.
constexpr bool        PICopilotJourneyIntegrationIdInHistory = true;

} // namespace pcl

#endif // PICopilot_JourneyConstants_h
