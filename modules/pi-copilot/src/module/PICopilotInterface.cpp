// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInterface.h"
#include "PICopilotProcess.h"
#include "ConfigDialog.h"
#include "KeyStore.h"

namespace pcl
{

PICopilotInterface* ThePICopilotInterface = nullptr;

namespace
{

// The single system prompt for every chat turn.
const char* const kSystemPrompt =
   "You are PI Copilot, an assistant embedded in PixInsight, the astronomical "
   "image processing application. Help the user plan and understand their "
   "PixInsight workflow: processes, scripts, parameters and processing order "
   "for their astrophotography data. Be concise and concrete.";

// Gear glyph (U+2699), written as UTF-8 bytes. pcl::String( const char* )
// decodes ISO-8859-1, so any non-ASCII literal must go through UTF8ToUTF16.
const char* const kGearUtf8 = "\xE2\x9A\x99";

} // namespace

PICopilotInterface::PICopilotInterface()
{
   ThePICopilotInterface = this;
}

PICopilotInterface::~PICopilotInterface()
{
   // Never destroy a running Thread, and never let the Timer fire into a
   // half-destroyed interface. Wait() blocks until the in-flight request
   // finishes (bounded by the transfer's own timeouts); this only happens
   // at module teardown with a request mid-flight.
   StopWorker();
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

IsoString PICopilotInterface::Id() const
{
   return "PICopilot";
}

MetaProcess* PICopilotInterface::Process() const
{
   return ThePICopilotProcess;
}

InterfaceFeatures PICopilotInterface::Features() const
{
   return InterfaceFeature::None;
}

bool PICopilotInterface::IsInstanceGenerator() const
{
   return false;
}

bool PICopilotInterface::Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "PI Copilot" );
   }

   dynamic = false;
   return true;
}

// ── Chat flow ────────────────────────────────────────────────────

void PICopilotInterface::StopWorker()
{
   if ( GUI != nullptr )
      GUI->Poll_Timer.Stop();
   if ( m_thread )
   {
      if ( m_thread->IsActive() )
         m_thread->Wait();
      m_thread.Destroy();
   }
}

void PICopilotInterface::AppendToLog( const String& richText )
{
   GUI->ChatLog.End();
   GUI->ChatLog.Insert( richText );
   GUI->ChatLog.End();
}

void PICopilotInterface::SendCurrentInput()
{
   // Busy guard shared by the Send button AND the Return-key path: one
   // turn in flight at a time.
   if ( m_thread )
      return;

   String prompt = GUI->ChatInput.Text().Trimmed();
   if ( prompt.IsEmpty() )
      return;

   String key = KeyStore::Load();
   if ( key.IsEmpty() )
   {
      // Visible notice, never a silent no-op. The input is kept so the
      // user can resend after setting the key.
      AppendToLog( TextBox::PlainText(
         String::UTF8ToUTF16( "Set your Anthropic API key via the \xE2\x9A\x99 button." ) ) + "\n\n" );
      return;
   }

   AppendToLog( "<b>You:</b> " + TextBox::PlainText( prompt ) + "\n\n" );
   m_history.Add( AnthropicMessage{ IsoString( "user" ), prompt } );
   GUI->ChatInput.Clear();

   // ChatThread copies/serializes key, system prompt and this history
   // snapshot on THIS (UI) thread; the worker holds no reference to us.
   m_thread = new ChatThread( key, String( kSystemPrompt ), m_history );
   GUI->Send_Button.Disable();
   m_thread->Start();

   if ( !GUI->Poll_Timer.IsRunning() )
      GUI->Poll_Timer.Start();
}

// ── Event handlers ───────────────────────────────────────────────

void PICopilotInterface::e_Send_Click( Button&, bool )
{
   SendCurrentInput();
}

void PICopilotInterface::e_Input_ReturnPressed( Edit& )
{
   SendCurrentInput();
}

void PICopilotInterface::e_Config_Click( Button&, bool )
{
   // ConfigDialog::Run() persists the key itself on OK; nothing to do here.
   ConfigDialog d;
   d.Run( KeyStore::Load() );
}

void PICopilotInterface::e_Poll_Timer( Timer& )
{
   if ( !m_thread )
   {
      GUI->Poll_Timer.Stop();
      return;
   }

   // Wait until the worker has fully returned from Run(), so destroying
   // it below can never race its exit.
   if ( m_thread->IsActive() )
      return;

   AnthropicResult r;
   if ( !m_thread->TryTakeResult( r ) )
   {
      // Unreachable by construction (Run() always stores a result), but
      // never swallow it silently.
      r = AnthropicResult();
      r.error = "worker thread ended without a result";
   }

   if ( r.ok )
   {
      AppendToLog( "<b>Copilot:</b> " + TextBox::PlainText( r.text ) + "\n\n" );
      m_history.Add( AnthropicMessage{ IsoString( "assistant" ), r.text } );
   }
   else
   {
      AppendToLog( TextBox::PlainText( "Error " + String( r.httpStatus ) + ": " + r.error ) + "\n\n" );
      // Drop the unanswered user turn so history keeps alternating
      // user/assistant (the API rejects two consecutive user messages).
      if ( !m_history.IsEmpty() && m_history[m_history.Length()-1].role == "user" )
         m_history.RemoveLast();
   }

   m_thread.Destroy();
   GUI->Poll_Timer.Stop();
   GUI->Send_Button.Enable();
}

// ── GUI Construction ─────────────────────────────────────────────

PICopilotInterface::GUIData::GUIData( PICopilotInterface& w )
{
   Mode_ComboBox.AddItem( "Copilot" );
   Mode_ComboBox.AddItem( "Advisor" );
   Mode_ComboBox.AddItem( "Guided" );
   Mode_ComboBox.SetToolTip( "<p>Assistant mode.</p>" );

   Config_ToolButton.SetText( String::UTF8ToUTF16( kGearUtf8 ) );
   Config_ToolButton.SetToolTip( "<p>Set your Anthropic API key.</p>" );
   Config_ToolButton.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Config_Click, w );

   Top_Sizer.SetSpacing( 4 );
   Top_Sizer.Add( Mode_ComboBox );
   Top_Sizer.AddStretch();
   Top_Sizer.Add( Config_ToolButton );

   ChatLog.SetReadOnly();
   ChatLog.SetScaledMinSize( 500, 300 );

   ChatInput.OnReturnPressed( (Edit::edit_event_handler)&PICopilotInterface::e_Input_ReturnPressed, w );

   Send_Button.SetText( "Send" );
   Send_Button.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Send_Click, w );

   Input_Sizer.SetSpacing( 4 );
   Input_Sizer.Add( ChatInput, 100 );
   Input_Sizer.Add( Send_Button );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Top_Sizer );
   Global_Sizer.Add( ChatLog, 100 );
   Global_Sizer.Add( Input_Sizer );

   Poll_Timer.SetInterval( 0.2 );
   Poll_Timer.SetPeriodic( true );
   Poll_Timer.OnTimer( (Timer::timer_event_handler)&PICopilotInterface::e_Poll_Timer, w );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
}

} // namespace pcl
