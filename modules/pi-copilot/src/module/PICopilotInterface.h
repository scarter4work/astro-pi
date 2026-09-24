// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PICopilotInterface_h
#define __PICopilotInterface_h

#include "AnthropicClient.h"
#include "ChatThread.h"

#include <pcl/AutoPointer.h>
#include <pcl/ComboBox.h>
#include <pcl/Edit.h>
#include <pcl/ProcessInterface.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/TextBox.h>
#include <pcl/Timer.h>
#include <pcl/ToolButton.h>

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

private:

   // ── Chat state (UI thread only) ───────────────────────────────
   //
   // m_history always alternates user/assistant: a user turn is appended
   // when a request starts, the assistant turn only when it succeeds, and
   // the unanswered user turn is REMOVED on error (it stays visible in the
   // log) so the next request never sends two consecutive user messages.
   Array<AnthropicMessage> m_history;

   // The in-flight turn, if any. Non-null == busy. Constructed and
   // destroyed on the UI thread only (see ChatThread), and never destroyed
   // while still active.
   AutoPointer<ChatThread> m_thread;

   void SendCurrentInput();
   void AppendToLog( const String& richText );
   void StopWorker();

   // ── GUI Controls ──────────────────────────────────────────────

   struct GUIData
   {
      GUIData( PICopilotInterface& );

      VerticalSizer   Global_Sizer;
      HorizontalSizer Top_Sizer;
      ComboBox        Mode_ComboBox;
      ToolButton      Config_ToolButton;
      TextBox         ChatLog;
      HorizontalSizer Input_Sizer;
      Edit            ChatInput;
      PushButton      Send_Button;

      Timer           Poll_Timer;
   };

   GUIData* GUI = nullptr;

   // ── Event handlers ────────────────────────────────────────────

   void e_Send_Click( Button& sender, bool checked );
   void e_Input_ReturnPressed( Edit& sender );
   void e_Config_Click( Button& sender, bool checked );
   void e_Poll_Timer( Timer& sender );

   friend struct GUIData;
};

extern PICopilotInterface* ThePICopilotInterface;

} // namespace pcl

#endif // __PICopilotInterface_h
