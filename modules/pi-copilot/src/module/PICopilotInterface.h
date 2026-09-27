// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PICopilotInterface_h
#define __PICopilotInterface_h

#include "AnthropicClient.h"
#include "AgentSession.h"
#include "AgentTools.h"
#include "ChatThread.h"
#include "CopilotSettings.h"
#include "JourneyTools.h"

#include <pcl/AutoPointer.h>
#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Edit.h>
#include <pcl/Label.h>
#include <pcl/ProcessInterface.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/TextBox.h>
#include <pcl/Timer.h>

#include <chrono>
#include <pcl/ToolButton.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <set>
#include <string>

namespace pcl
{

// The journey strip's text (spec §7). Pure. A paused reason is cut to 160
// characters (the tooltip has the full status).
String JourneyStripText( const JourneyStatus& s );

class PICopilotInterface : public ProcessInterface
{
public:
   PICopilotInterface();
   virtual ~PICopilotInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   InterfaceFeatures Features() const override;

   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;

   // This interface doesn't reimplement NewProcess() (there is no process
   // state to generate an instance from), so per the ProcessInterface
   // contract it must declare itself a non-generator.
   bool IsInstanceGenerator() const override;

   // Image notifications (plan Task 1: the spike probe; Task 7: JourneyService;
   // Task T-hist: every one is process activity, ProcessActivity.h).
   // Handlers only queue / time-stamp; nothing is read here (global constraint).
   bool WantsImageNotifications() const override;
   void ImageCreated( const View& view ) override;
   void ImageUpdated( const View& view ) override;
   void ImageRenamed( const View& view ) override;
   void ImageDeleted( const View& view ) override;
   void ImageSaved( const View& view ) override;
   void ImageFocused( const View& view ) override;
   void ImageLocked( const View& view ) override;
   void ImageUnlocked( const View& view ) override;

   // Wraps arbitrary text for TextBox rich text as literal (<raw>) text.
   // Unlike TextBox::PlainText(), text containing its own "</raw>" (any
   // case/spacing) cannot end the raw block early: each such '<' is emitted
   // as a &lt; entity OUTSIDE the raw block, which TextBox renders as '<'.
   // Use it at EVERY site that inserts non-literal text into the chat log.
   static String PlainText( const String& text );

private:

   friend bool RunAgentSelfTest( nlohmann::json& out );
   friend bool RunJourneySelfTest( nlohmann::json& out );

   // Test-only (self-test Section A7): builds the GUI if it does not exist
   // yet, then measures whether the panel resizes both ways and the chat log
   // follows; restores the original size. Root thread only.
   nlohmann::json ProbeResizeForSelfTest();

   // Starts a user message's target: records the FullId of the active
   // window's current view (the view whose context/preview this message
   // carries) and forgets the previous message's inspected views.
   void BeginTurnTarget();

   // ── Chat state (UI thread only) ───────────────────────────────

   // The conversation + tool loop (UI thread only). Roles alternate and
   // every tool_use is answered; see AgentSession.
   AgentSession m_session;

   // The in-flight HTTP request, if any. Constructed and destroyed on the UI
   // thread only (see ChatThread), and never destroyed while still active.
   AutoPointer<ChatThread> m_thread;

   // Per user message: the prompt (restored on failure), the key and the mode
   // it was sent with (a mode change mid-loop applies to the NEXT message).
   String    m_pendingPrompt;
   String    m_apiKey;
   AgentMode m_turnMode = AgentMode::Copilot;

   // Per user message: the optional tools offered (run_pjsr = ⚙ Allow
   // scripts, read at Send like the model).
   ToolOptions m_turnTools;

   // The model this user message runs on (read from the ⚙ settings at Send)
   // and the one last named in the log (named again when it changes).
   IsoString m_turnModel;
   IsoString m_lastModel;

   // The last unknown-saved-model note shown (once per distinct note).
   String m_lastModelNote;

   // The last KeyStore note shown in the log (a migration or fallback
   // notice), so it is appended only once per distinct note.
   String m_lastKeyNote;

   // Per user message: the target view (see ToolContext::turnViewId) and the
   // views get_view_context inspected (see ToolContext::inspectedViews).
   IsoString             m_turnViewId;
   std::set<std::string> m_inspectedViews;

   // Stop pressed: cancel the request in flight, run no further tool.
   bool m_stopRequested = false;

   // True while tools run inside e_Poll_Timer. Processes pump events, so
   // Send/Stop/Clear/Timer can fire re-entrantly; this blocks a second turn.
   bool m_handlingResult = false;

   // The current request's reply has started rendering live (streamed text).
   bool m_replyShown = false;

   // Task T-hist PREVENT: a finished reply whose image-changing tools wait
   // for PixInsight to be idle (ProcessActivity.h). e_Poll_Timer keeps
   // ticking while held; Stop releases it (the tools then do not run).
   bool                                  m_resultHeld = false;
   AnthropicResult                       m_heldResult;
   bool                                  m_heldPartialReplyCut = false;
   std::chrono::steady_clock::time_point m_heldSince;
   bool                                  m_busyWaitNoted = false;

   // A user message is being worked on: a request in flight, a reply held
   // for idle, or tools running.
   bool TurnInProgress() const
   {
      return m_thread || m_handlingResult || m_resultHeld;
   }

   // Appends streamed text received since the last call (UI thread).
   void DrainStreamedText();

   void SendCurrentInput();
   void StartRequest();
   void FinishTurn();
   // Ends the user message with a non-continuing step: its notes
   // (DescribeTurnEnd), the prompt restored when asked, then FinishTurn().
   // partialReplyCut: a streamed reply was shown and then broke off.
   void EndTurn( const AgentStep& step, int httpStatus, bool partialReplyCut = false );
   void AppendToLog( const String& richText );
   // Tells the user when the history budget dropped older messages.
   void NoteTrimmed();
   void StopWorker();
   void SetBusy( bool busy, const String& caption = String() );
   ToolContext MakeToolContext();

   // Guided-mode confirmation (modal MessageBox, root thread).
   static bool ConfirmApply( const String& processId, const String& viewId, const String& changes );

   // ── Image journey (0.2.0.0) ───────────────────────────────────
   // What the journey tools and ★ use, rebuilt from JourneyService per turn /
   // click (store, tracker, keeper, ⚙ export folder, key, confirm). Holds no
   // View / ImageWindow: ids only.
   JourneyToolHost m_journeyHost;
   // ★ is running its keep flow (its confirm box pumps events): no second
   // keep and no new message until it returns.
   bool m_keepRunning = false;
   // The last strip update's ★ decision (self-test: IsEnabled() also
   // reflects the panel's own state, which PixInsight disables while a
   // process such as the self-test runs).
   bool m_keepAllowed = false;
   JourneyToolHost MakeJourneyHost();
   void RefreshJourneyHost();   // m_journeyHost = MakeJourneyHost()
   // The strip + ★ for the active image; UpdateJourneyStripFor() for a given
   // main view id (self-test). Read-only; root thread.
   void UpdateJourneyStrip();
   void UpdateJourneyStripFor( const IsoString& mainViewId );
   // JourneyService notes -> the chat log; held back while a streamed reply or
   // its tools are being written (the next timer tick shows them).
   void DrainJourneyNotes();
   IsoString ActiveMainViewId() const;
   // ★ on a main view: keep flow (summary -> confirm -> keep -> freeze); every
   // outcome, failures included, is one line in the chat log, also returned.
   String KeepJourneyOfView( const IsoString& mainViewId );
   static bool ConfirmKeeper( const String& summaryHtml );
   // Self-test: replaces the ★ confirm box (a modal cannot run headlessly).
   static std::function<bool( const String& )> s_confirmKeeperForSelfTest;

   // UI thread only: captures the turn view's (m_turnViewId) context +
   // preview (when "Include view" is checked) and returns the composed user
   // turn. Every capture problem is written to the chat log; the text always
   // sends.
   AnthropicMessage ComposeTurnWithActiveView( const String& prompt );

   // Default placement: flush to the ⚙ side (right by default), full height.
   // Applied once (see e_Show), and again when the ⚙ side changes.
   // Returns true only if the panel was actually resized and moved.
   bool ApplyDefaultPlacement();

   // ── GUI Controls ──────────────────────────────────────────────

   struct GUIData
   {
      GUIData( PICopilotInterface& );

      VerticalSizer   Global_Sizer;
      HorizontalSizer Top_Sizer;
      ComboBox        Mode_ComboBox;
      CheckBox        IncludeView_CheckBox;
      PushButton      Clear_Button;
      ToolButton      Config_ToolButton;
      HorizontalSizer Journey_Sizer;
      Label           JourneyStrip_Label;
      ToolButton      Keep_ToolButton;
      Timer           Journey_Timer;
      TextBox         ChatLog;
      HorizontalSizer Input_Sizer;
      Edit            ChatInput;
      PushButton      Send_Button;
      PushButton      Stop_Button;

      Timer           Poll_Timer;
   };

   GUIData* GUI = nullptr;

   // ── Event handlers ────────────────────────────────────────────

   void e_Send_Click( Button& sender, bool checked );
   void e_Input_ReturnPressed( Edit& sender );
   void e_Config_Click( Button& sender, bool checked );
   void e_Poll_Timer( Timer& sender );
   void e_Show( Control& sender );
   void e_Stop_Click( Button& sender, bool checked );
   void e_Clear_Click( Button& sender, bool checked );
   void e_Mode_ItemSelected( ComboBox& sender, int itemIndex );
   void e_Keep_Click( Button& sender, bool checked );
   void e_Strip_MousePress( Control& sender, const pcl::Point& pos, int button, unsigned buttons, unsigned modifiers );
   void e_Journey_Timer( Timer& sender );

   friend struct GUIData;
};

extern PICopilotInterface* ThePICopilotInterface;

} // namespace pcl

#endif // __PICopilotInterface_h
