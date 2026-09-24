// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInterface.h"
#include "PICopilotProcess.h"
#include "ConfigDialog.h"
#include "KeyStore.h"
#include "PanelPlacement.h"
#include "ViewCapture.h"
#include "VisionTurn.h"

#include <pcl/Console.h>
#include <pcl/GlobalSettings.h>
#include <pcl/ImageWindow.h>
#include <pcl/Settings.h>

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
   "for their astrophotography data. Be concise and concrete.\n\n"
   "A user message may begin with a [PixInsight view context] block (JSON: view "
   "identity, geometry, per-channel statistics, FITS keywords) and may include an "
   "image. The image is an automatically stretched (auto-STF) JPEG preview, "
   "downscaled to at most 1024 px, for DISPLAY ONLY: the underlying data is usually "
   "still LINEAR (unstretched). Base any statement about the data's levels, noise or "
   "clipping on the statistics in the context block, which describe the real data "
   "(mad is the raw median absolute deviation; multiply by 1.4826 for sigma). Only "
   "the latest message carries an image; earlier images are omitted from history.";

// Gear glyph (U+2699), written as UTF-8 bytes. pcl::String( const char* )
// decodes ISO-8859-1, so any non-ASCII literal must go through UTF8ToUTF16.
const char* const kGearUtf8 = "\xE2\x9A\x99";

// Send button captions: idle, and while a turn is in flight (U+2026 "...").
const char* const kSendText = "Send";
const char* const kBusyTextUtf8 = "Thinking\xE2\x80\xA6";

// One-time default placement (flush right, full height). Logical px.
// A marker setting, not "no saved geometry": installs from 0.1.0.x already
// have a saved floating geometry, so a first-launch test would never fire.
const char* const kPlacementMarkerKey = "PICopilot/DefaultPlacementApplied";
constexpr int kDefaultPanelWidth   = 420;
constexpr int kDefaultTopMargin    = 40;   // below the main menu/title bar
constexpr int kDefaultBottomMargin = 60;   // above a desktop taskbar
constexpr int kDefaultRightMargin  = 8;

} // namespace

PICopilotInterface::PICopilotInterface()
{
   ThePICopilotInterface = this;
}

PICopilotInterface::~PICopilotInterface()
{
   // Never destroy a running Thread, and never let the Timer fire into a
   // half-destroyed interface. StopWorker() cancels the in-flight request
   // (aborted from the transfer's progress callback) before Wait()ing, so
   // teardown is not held hostage by a stalled connection; this only
   // happens at module teardown with a request mid-flight.
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
      // PCL's InterfaceDispatcher::Launch restores the saved geometry right
      // AFTER this function returns (first launch only); the window is shown
      // after that, so OnShow is the first point where our placement wins.
      OnShow( (Control::event_handler)&PICopilotInterface::e_Show, *this );
   }

   dynamic = false;
   return true;
}

String PICopilotInterface::PlainText( const String& text )
{
   String out = "<raw>";
   const size_type n = text.Length();
   for ( size_type i = 0; i < n; ++i )
   {
      if ( text[i] == '<' )
      {
         // Does this '<' start a "</raw" closing tag (whitespace-tolerant,
         // case-insensitive)?
         size_type j = i + 1;
         while ( j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '\n' || text[j] == '\r') )
            ++j;
         if ( j < n && text[j] == '/' )
         {
            ++j;
            while ( j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '\n' || text[j] == '\r') )
               ++j;
            if ( j+3 <= n && text.Substring( j, 3 ).CompareIC( "raw" ) == 0 )
            {
               out += "</raw>&lt;<raw>";
               continue;
            }
         }
      }
      out += text[i];
   }
   out += "</raw>";
   return out;
}

// ── Chat flow ────────────────────────────────────────────────────

void PICopilotInterface::StopWorker()
{
   if ( GUI != nullptr )
      GUI->Poll_Timer.Stop();
   if ( m_thread )
   {
      if ( m_thread->IsActive() )
      {
         // Abort the transfer first; a bare Wait() on a stalled connection
         // would block until the request deadline.
         m_thread->RequestCancel();
         m_thread->Wait();
      }
      m_thread.Destroy();
   }
}

void PICopilotInterface::SetBusy( bool busy )
{
   // Non-streamed replies can take tens of seconds: show that a turn is in
   // flight on the (disabled) Send button itself.
   GUI->Send_Button.SetText( busy ? String::UTF8ToUTF16( kBusyTextUtf8 ) : String( kSendText ) );
   GUI->Send_Button.SetToolTip( busy
      ? String( "<p>Waiting for the reply (non-streamed; gives up after " )
            + String( PICopilotRequestTimeoutSeconds ) + " s).</p>"
      : String( "<p>Send the message (or press Return).</p>" ) );
   GUI->Send_Button.Enable( !busy );
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
      AppendToLog( PlainText(
         String::UTF8ToUTF16( "Set your Anthropic API key via the \xE2\x9A\x99 button." ) ) + "\n\n" );
      return;
   }

   AppendToLog( "<b>You:</b> " + PlainText( prompt ) + "\n\n" );
   // Root thread, BEFORE the worker starts: ImageWindow/View/Bitmap are
   // UIObjects. StripOlderImages() spares the LAST message, so it runs right
   // after the new turn -- the only one carrying pixels -- is appended.
   m_history.Add( ComposeTurnWithActiveView( prompt ) );
   StripOlderImages( m_history );
   // The BARE prompt (never the context-prefixed content) is what a failed
   // turn gives back for a resend.
   m_pendingPrompt = prompt;
   GUI->ChatInput.Clear();

   // ChatThread copies/serializes key, system prompt and this history
   // snapshot on THIS (UI) thread; the worker holds no reference to us.
   m_thread = new ChatThread( key, String( kSystemPrompt ), m_history );
   SetBusy( true );
   m_thread->Start();

   if ( !GUI->Poll_Timer.IsRunning() )
      GUI->Poll_Timer.Start();
}

AnthropicMessage PICopilotInterface::ComposeTurnWithActiveView( const String& prompt )
{
   if ( !GUI->IncludeView_CheckBox.IsChecked() )
      return ComposeUserTurn( prompt, nullptr, IsoString() );

   StringList notes;
   AnthropicMessage turn;
   ImageWindow window = ImageWindow::ActiveWindow();
   if ( window.IsNull() )
      turn = CaptureViewTurn( prompt, nullptr, notes );
   else
   {
      const View view = window.CurrentView();   // may be a preview
      turn = CaptureViewTurn( prompt, &view, notes );
   }
   for ( const String& note : notes )
      AppendToLog( PlainText( note ) + "\n\n" );
   return turn;
}

bool PICopilotInterface::ApplyDefaultPlacement()
{
   if ( !PixInsightSettings::IsGlobalVariableDefined( "Workspace/PrimaryScreenCenterX" )
     || !PixInsightSettings::IsGlobalVariableDefined( "Workspace/PrimaryScreenCenterY" ) )
   {
      Console().WarningLn( "PI Copilot: primary-screen geometry unavailable; default right-side placement skipped." );
      return false;
   }
   const PanelPlacement p = ComputeDefaultPanelPlacement(
      PixInsightSettings::GlobalInteger( "Workspace/PrimaryScreenCenterX" ),
      PixInsightSettings::GlobalInteger( "Workspace/PrimaryScreenCenterY" ),
      LogicalPixelsToPhysical( kDefaultPanelWidth ),
      LogicalPixelsToPhysical( kDefaultTopMargin ),
      LogicalPixelsToPhysical( kDefaultBottomMargin ),
      LogicalPixelsToPhysical( kDefaultRightMargin ) );
   if ( !p.ok )
   {
      Console().WarningLn( "PI Copilot: primary-screen geometry too small; default right-side placement skipped." );
      return false;
   }
   Resize( p.width, p.height );
   Move( p.x, p.y );
   // Persist immediately (not only at PI exit) so the placement survives a
   // crash and any later RestoreGeometry() reproduces it.
   SaveGeometry();
   return true;
}

// ── Event handlers ───────────────────────────────────────────────

void PICopilotInterface::e_Show( Control& )
{
   // One time only; afterwards the user's own moves/resizes are remembered
   // by PI's auto-save geometry (on by default, ProcessInterface.h:2549).
   bool applied = false;
   Settings::Read( kPlacementMarkerKey, applied );
   if ( applied )
      return;
   // Mark as applied only after a successful Move(): if the screen geometry
   // was unavailable or unusable, the next show tries again.
   if ( ApplyDefaultPlacement() )
      Settings::Write( kPlacementMarkerKey, true );
}

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
      // The truncation note is display-only; history keeps the model's own
      // text so it isn't fed back to the API as if the model had said it.
      String shown = r.text;
      if ( r.truncated )
         shown += " [truncated: max_tokens]";
      AppendToLog( "<b>Copilot:</b> " + PlainText( shown ) + "\n\n" );
      m_history.Add( AnthropicMessage{ IsoString( "assistant" ), r.text, IsoString() } );
   }
   else
   {
      AppendToLog( PlainText( "Error " + String( r.httpStatus ) + ": " + r.error ) + "\n\n" );
      // Drop the unanswered user turn so history keeps alternating
      // user/assistant (the API rejects two consecutive user messages).
      if ( !m_history.IsEmpty() && m_history[m_history.Length()-1].role == "user" )
         m_history.RemoveLast();
      // Give the failed prompt back for a resend -- unless the user has
      // already started typing something new.
      if ( GUI->ChatInput.Text().IsEmpty() )
         GUI->ChatInput.SetText( m_pendingPrompt );
   }

   m_pendingPrompt.Clear();
   m_thread.Destroy();
   GUI->Poll_Timer.Stop();
   SetBusy( false );
}

// ── GUI Construction ─────────────────────────────────────────────

PICopilotInterface::GUIData::GUIData( PICopilotInterface& w )
{
   Mode_ComboBox.AddItem( "Copilot" );
   Mode_ComboBox.AddItem( "Advisor" );
   Mode_ComboBox.AddItem( "Guided" );
   Mode_ComboBox.SetToolTip( "<p>Assistant mode.</p>" );

   IncludeView_CheckBox.SetText( "Include view" );
   IncludeView_CheckBox.SetChecked( true );
   IncludeView_CheckBox.SetToolTip( "<p>Send the active view with each message: an auto-stretched "
                                    "preview (display only) plus its geometry, statistics and FITS keywords.</p>" );

   Config_ToolButton.SetText( String::UTF8ToUTF16( kGearUtf8 ) );
   Config_ToolButton.SetToolTip( "<p>Set your Anthropic API key.</p>" );
   Config_ToolButton.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Config_Click, w );

   Top_Sizer.SetSpacing( 4 );
   Top_Sizer.Add( Mode_ComboBox );
   Top_Sizer.Add( IncludeView_CheckBox );
   Top_Sizer.AddStretch();
   Top_Sizer.Add( Config_ToolButton );

   ChatLog.SetReadOnly();
   ChatLog.SetScaledMinSize( 360, 200 );

   ChatInput.OnReturnPressed( (Edit::edit_event_handler)&PICopilotInterface::e_Input_ReturnPressed, w );

   Send_Button.SetText( kSendText );
   Send_Button.SetToolTip( "<p>Send the message (or press Return).</p>" );
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
