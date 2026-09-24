// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PICopilotInterface_h
#define __PICopilotInterface_h

#include "AnthropicClient.h"
#include "AgentSession.h"
#include "AgentTools.h"
#include "ChatThread.h"

#include <pcl/AutoPointer.h>
#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Edit.h>
#include <pcl/ProcessInterface.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/TextBox.h>
#include <pcl/Timer.h>
#include <pcl/ToolButton.h>

#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace pcl
{

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

   // Wraps arbitrary text for TextBox rich text as literal (<raw>) text.
   // Unlike TextBox::PlainText(), text containing its own "</raw>" (any
   // case/spacing) cannot end the raw block early: each such '<' is emitted
   // as a &lt; entity OUTSIDE the raw block, which TextBox renders as '<'.
   // Use it at EVERY site that inserts non-literal text into the chat log.
   static String PlainText( const String& text );

private:

   friend bool RunAgentSelfTest( nlohmann::json& out );

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

   // Per user message: the target view (see ToolContext::turnViewId) and the
   // views get_view_context inspected (see ToolContext::inspectedViews).
   IsoString             m_turnViewId;
   std::set<std::string> m_inspectedViews;

   // Stop pressed: cancel the request in flight, run no further tool.
   bool m_stopRequested = false;

   // True while tools run inside e_Poll_Timer. Processes pump events, so
   // Send/Stop/Clear/Timer can fire re-entrantly; this blocks a second turn.
   bool m_handlingResult = false;

   void SendCurrentInput();
   void StartRequest();
   void FinishTurn();
   // Ends the user message with a non-continuing step: its notes
   // (DescribeTurnEnd), the prompt restored when asked, then FinishTurn().
   void EndTurn( const AgentStep& step, int httpStatus );
   void AppendToLog( const String& richText );
   void StopWorker();
   void SetBusy( bool busy );
   ToolContext MakeToolContext();

   // Guided-mode confirmation (modal MessageBox, root thread).
   static bool ConfirmApply( const String& processId, const String& viewId, const String& changes );

   // UI thread only: captures the turn view's (m_turnViewId) context +
   // preview (when "Include view" is checked) and returns the composed user
   // turn. Every capture problem is written to the chat log; the text always
   // sends.
   AnthropicMessage ComposeTurnWithActiveView( const String& prompt );

   // One-time default placement: flush right, full height (see e_Show).
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

   friend struct GUIData;
};

extern PICopilotInterface* ThePICopilotInterface;

} // namespace pcl

#endif // __PICopilotInterface_h
