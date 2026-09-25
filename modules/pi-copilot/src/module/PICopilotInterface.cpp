// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInterface.h"
#include "PICopilotProcess.h"
#include "AgentSession.h"
#include "AgentTools.h"
#include "ConfigDialog.h"
#include "KeyStore.h"
#include "PanelPlacement.h"
#include "SystemPrompt.h"
#include "TurnEndNotes.h"
#include "ViewCapture.h"
#include "VisionTurn.h"

#include <pcl/Console.h>
#include <pcl/GlobalSettings.h>
#include <pcl/ImageWindow.h>
#include <pcl/MessageBox.h>
#include <pcl/Settings.h>

namespace pcl
{

PICopilotInterface* ThePICopilotInterface = nullptr;

namespace
{

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

// Resizable panel: explicit minimum (logical px); the maximum is left
// unbounded via Control::SetVariableSize() (int_max, Control.h:415-419).
constexpr int kMinPanelWidth  = 300;
constexpr int kMinPanelHeight = 260;

// Persisted assistant mode: Mode_ComboBox index == AgentMode value.
const char* const kModeKey = "PICopilot/Mode";

// One-time chat-log notice for the agent modes (0.1.0.x installs had a mode
// selector that did nothing). A marker setting, written once shown.
const char* const kModesNoticeMarkerKey = "PICopilot/AgentModesNoticeShown";
const char* const kModesNoticeUtf8 =
   "New in this version: PI Copilot can now work on your images. The mode selector at the top left sets how: "
   "Copilot applies processes directly when you ask (every change is recorded in the view's History, so you can "
   "undo it as usual); Guided shows each change and asks you first; Advisor is read-only and only gives advice. "
   "A mode change applies from your next message.";

// For MessageBox rich text (the Guided dialog): the model-chosen ids and the
// parameter text are shown literally.
String EscapeHtml( const String& s )
{
   String out;
   for ( size_type i = 0; i < s.Length(); ++i )
   {
      const char16_type c = s[i];
      if ( c == '&' )
         out += "&amp;";
      else if ( c == '<' )
         out += "&lt;";
      else if ( c == '>' )
         out += "&gt;";
      else if ( c == '\n' )
         out += "<br/>";
      else
         out += c;
   }
   return out;
}

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
   // Not None: InterfaceFeature::None "effectively suppresses the interface's
   // control bar" (ProcessInterface.h:144), and PixInsight's interface frame
   // puts the mouse size grip in that bar. InfoArea is the most inert flag: a
   // single-line text area, no Apply/Execute/Reset/drag object that would act
   // on this degenerate (global-only, no-parameter) process.
   return InterfaceFeature::InfoArea;
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

      bool noticeShown = false;
      Settings::Read( kModesNoticeMarkerKey, noticeShown );
      if ( !noticeShown )
      {
         AppendToLog( PlainText( String::UTF8ToUTF16( kModesNoticeUtf8 ) ) + "\n\n" );
         Settings::Write( kModesNoticeMarkerKey, true );
      }
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
   // Replies can take tens of seconds (thinking before the first streamed
   // text) and tools run between them: show that a message is being worked
   // on, on the Send button itself.
   GUI->Send_Button.SetText( busy ? String::UTF8ToUTF16( kBusyTextUtf8 ) : String( kSendText ) );
   GUI->Send_Button.SetToolTip( busy
      ? String( "<p>Working (each request gives up after " ) + String( PICopilotRequestTimeoutSeconds ) + " s).</p>"
      : String( "<p>Send the message (or press Return).</p>" ) );
   GUI->Send_Button.Enable( !busy );
   // The mode is fixed per message; the combo is locked while one runs.
   GUI->Mode_ComboBox.Enable( !busy );
   GUI->Clear_Button.Enable( !busy );
   if ( busy )
   {
      GUI->Stop_Button.Enable();
      GUI->Stop_Button.Show();
   }
   else
      GUI->Stop_Button.Hide();
}

void PICopilotInterface::AppendToLog( const String& richText )
{
   GUI->ChatLog.End();
   GUI->ChatLog.Insert( richText );
   GUI->ChatLog.End();
}

void PICopilotInterface::SendCurrentInput()
{
   // One user message in flight at a time -- including while tools run
   // (processes pump events, so this can be reached re-entrantly).
   if ( m_thread || m_handlingResult )
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
   // The view this message is about is fixed NOW; the tools re-resolve it by
   // id, so a click on another image while the request runs changes nothing.
   BeginTurnTarget();
   // Root thread, before any request: capture that view (increment 3).
   // The BARE prompt (never the context-prefixed content) is what a failed
   // message gives back for a resend.
   m_session.BeginUserTurn( ComposeTurnWithActiveView( prompt ) );
   m_pendingPrompt = prompt;
   m_apiKey = key;
   m_turnMode = AgentModeFromIndex( GUI->Mode_ComboBox.CurrentItem() );
   m_stopRequested = false;
   GUI->ChatInput.Clear();
   SetBusy( true );
   StartRequest();
}

void PICopilotInterface::StartRequest()
{
   String why;
   if ( !HistoryIsApiValid( m_session.History(), why ) )
   {
      // Never send a body the API would reject with an opaque 400. AbortTurn
      // rolls the session back to before this message; it is a Failed step.
      EndTurn( m_session.AbortTurn( "history invalid: " + why + " (nothing was sent)" ), 0 );
      return;
   }
   m_replyShown = false;
   try
   {
      // ChatThread serializes key, prompt, history snapshot and tools HERE (UI thread).
      m_thread = new ChatThread( m_apiKey, BuildSystemPrompt( m_turnMode ), m_session.History(),
                                 PICOPILOT_DEFAULT_MODEL, PICOPILOT_MESSAGES_URL,
                                 PICopilotRequestTimeoutSeconds, ToolDefinitions( m_turnMode ),
                                 ProductionRequestShape( PICOPILOT_DEFAULT_MODEL ) );
      m_thread->Start();
   }
   catch ( ... )
   {
      String what = "unknown error";
      try
      {
         throw;
      }
      catch ( const pcl::Exception& x ) { what = x.Message(); }
      catch ( const std::exception& x ) { what = String( x.what() ); }
      catch ( ... ) {}
      if ( m_thread )
      {
         if ( m_thread->IsActive() )
         {
            m_thread->RequestCancel();
            m_thread->Wait();
         }
         m_thread.Destroy();
      }
      EndTurn( m_session.AbortTurn( "internal error: could not start the request: " + what ), 0 );
      return;
   }
   if ( !GUI->Poll_Timer.IsRunning() )
      GUI->Poll_Timer.Start();
}

void PICopilotInterface::EndTurn( const AgentStep& step, int httpStatus )
{
   const TurnEndView v = DescribeTurnEnd( step, httpStatus );
   for ( const String& note : v.notes )
      AppendToLog( PlainText( note ) + "\n\n" );
   // Give the failed prompt back -- unless the user has already started
   // typing something new.
   if ( v.restoreInput && GUI->ChatInput.Text().IsEmpty() )
      GUI->ChatInput.SetText( m_pendingPrompt );
   FinishTurn();
}

void PICopilotInterface::FinishTurn()
{
   m_turnViewId.Clear();
   m_inspectedViews.clear();
   m_pendingPrompt.Clear();
   m_apiKey.Clear();
   m_stopRequested = false;
   GUI->Poll_Timer.Stop();
   SetBusy( false );
}

void PICopilotInterface::BeginTurnTarget()
{
   m_turnViewId.Clear();
   m_inspectedViews.clear();
   ImageWindow w = ImageWindow::ActiveWindow();
   if ( !w.IsNull() )
   {
      const View v = w.CurrentView();   // may be a preview
      if ( !v.IsNull() )
         m_turnViewId = v.FullId();
   }
}

ToolContext PICopilotInterface::MakeToolContext()
{
   ToolContext ctx;
   ctx.mode = m_turnMode;
   ctx.turnViewId = m_turnViewId;
   ctx.inspectedViews = &m_inspectedViews;
   ctx.confirm = &PICopilotInterface::ConfirmApply;
   return ctx;
}

bool PICopilotInterface::ConfirmApply( const String& processId, const String& viewId, const String& changes )
{
   const String text = "<p>Apply <b>" + EscapeHtml( processId ) + "</b> to <b>" + EscapeHtml( viewId ) + "</b>?</p>"
                     + "<p>" + EscapeHtml( changes ) + "</p>"
                     + "<p>You can undo it afterwards from the view's History.</p>";
   return MessageBox( text, String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 Guided mode" ), StdIcon::Question,
                      StdButton::Yes, StdButton::No, StdButton::NoButton, 1/*default: No*/, 1/*Esc: No*/ ).Execute()
          == StdButton::Yes;
}

AnthropicMessage PICopilotInterface::ComposeTurnWithActiveView( const String& prompt )
{
   if ( !GUI->IncludeView_CheckBox.IsChecked() )
      return ComposeUserTurn( prompt, nullptr, IsoString() );

   StringList notes;
   AnthropicMessage turn;
   const View view = m_turnViewId.IsEmpty() ? View::Null() : View::ViewById( m_turnViewId );
   if ( view.IsNull() )
      turn = CaptureViewTurn( prompt, nullptr, notes );
   else
      turn = CaptureViewTurn( prompt, &view, notes );
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

void PICopilotInterface::DrainStreamedText()
{
   if ( !m_thread )
      return;
   const String d = m_thread->TakeStreamedText();
   if ( d.IsEmpty() )
      return;
   if ( !m_replyShown )
   {
      AppendToLog( "<b>Copilot:</b> " );
      m_replyShown = true;
   }
   AppendToLog( PlainText( d ) );
}

void PICopilotInterface::e_Poll_Timer( Timer& )
{
   if ( !m_thread )
   {
      GUI->Poll_Timer.Stop();
      return;
   }
   // Streamed text arrives while the request runs: show it as it comes.
   DrainStreamedText();
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
      r.errorKind = RequestErrorKind::Internal;
      r.error = "worker thread ended without a result";
   }
   DrainStreamedText();   // whatever arrived after the last tick
   m_thread.Destroy();
   // Tools may run for seconds and pump events: never re-enter this handler.
   GUI->Poll_Timer.Stop();

   // The truncation note is display-only; history keeps the model's own text.
   if ( m_replyShown )
   {
      // The reply was rendered live; close it (and say so when it broke off).
      AppendToLog( (r.ok && r.truncated ? PlainText( " [truncated: max_tokens]" ) : String()) + "\n\n" );
      if ( !r.ok && !r.cancelled )
         AppendToLog( PlainText( "(the partial reply above was interrupted; it is not kept in the conversation)" ) + "\n\n" );
      m_replyShown = false;
   }
   else if ( r.ok && !r.text.IsEmpty() )   // not streamed, or no delta arrived before the end
      AppendToLog( "<b>Copilot:</b> " + PlainText( r.truncated ? r.text + " [truncated: max_tokens]" : r.text ) + "\n\n" );

   m_handlingResult = true;
   AgentStep s;
   {
      const ToolContext ctx = MakeToolContext();
      s = m_session.OnResponse( r,
         [&ctx]( const ToolCall& call ) { return ExecuteTool( call, ctx ); },
         [this]() { return m_stopRequested; },
         [this]( const String& line ) { AppendToLog( PlainText( line ) + "\n" ); } );
   }
   m_handlingResult = false;
   NoteTrimmed();

   if ( s.kind == AgentStep::SendAgain )
   {
      if ( !s.toolLog.IsEmpty() )
         AppendToLog( "\n" );
      StartRequest();
      return;
   }
   if ( !s.toolLog.IsEmpty() )
      AppendToLog( "\n" );
   EndTurn( s, r.httpStatus );
}

void PICopilotInterface::e_Stop_Click( Button&, bool )
{
   if ( !m_thread && !m_handlingResult )
      return;
   // A running process is never interrupted: the loop checks this flag
   // between tools; the HTTP request in flight (if any) is cancelled.
   m_stopRequested = true;
   if ( m_thread )
      m_thread->RequestCancel();
   GUI->Stop_Button.Disable();
}

void PICopilotInterface::e_Clear_Click( Button&, bool )
{
   // Never while a message is being worked on (the button is disabled then;
   // this also covers a re-entrant click while a process pumps events).
   if ( m_thread || m_handlingResult )
      return;
   m_session.Clear();
   GUI->ChatLog.Clear();
   AppendToLog( PlainText( "(new chat started: the model no longer sees the earlier conversation; your images are unchanged)" ) + "\n\n" );
}

void PICopilotInterface::NoteTrimmed()
{
   const size_type n = m_session.TakeTrimmedMessages();
   if ( n > 0 )
      AppendToLog( PlainText( String().Format( "(%u older messages are no longer sent to the model, to keep this "
                                               "conversation within its history budget. Your images are unchanged; "
                                               "press New chat to start fresh.)", unsigned( n ) ) ) + "\n\n" );
}

void PICopilotInterface::e_Mode_ItemSelected( ComboBox&, int itemIndex )
{
   // Read per message in SendCurrentInput(): takes effect on the next one.
   Settings::Write( kModeKey, itemIndex );
}

// ── Self-test probe ──────────────────────────────────────────────

nlohmann::json PICopilotInterface::ProbeResizeForSelfTest()
{
   nlohmann::json j;
   if ( GUI == nullptr )
   {
      bool dynamic = false;
      unsigned flags = 0;
      Launch( *ThePICopilotProcess, nullptr, dynamic, flags );
   }
   EnsureLayoutUpdated();
   const int w0 = Width(), h0 = Height(), log0 = GUI->ChatLog.Height();
   j["fixedWidth"] = IsFixedWidth();
   j["fixedHeight"] = IsFixedHeight();
   j["min"] = { MinWidth(), MinHeight() };
   j["max"] = { MaxWidth(), MaxHeight() };
   j["before"] = { w0, h0, log0 };

   Resize( w0 + 300, h0 + 300 );
   EnsureLayoutUpdated();
   const int w1 = Width(), h1 = Height(), log1 = GUI->ChatLog.Height();
   j["grown"] = { w1, h1, log1 };

   Resize( MinWidth(), MinHeight() );
   EnsureLayoutUpdated();
   const int w2 = Width(), h2 = Height();
   j["shrunk"] = { w2, h2, GUI->ChatLog.Height() };

   Resize( w0, h0 );
   EnsureLayoutUpdated();
   j["restored"] = { Width(), Height() };

   // The shrunk window must still hold the chat log's own minimum plus the
   // global sizer's margins (a window that shrinks past its content, e.g. to
   // 0x0 with min 0,0, is a defect), and the minimum must be exactly ours.
   const int margins = 2*GUI->Global_Sizer.Margin();
   const int minW = LogicalPixelsToPhysical( kMinPanelWidth );
   const int minH = LogicalPixelsToPhysical( kMinPanelHeight );
   j["expectedMin"] = { minW, minH };
   j["chatLogMin"] = { GUI->ChatLog.MinWidth(), GUI->ChatLog.MinHeight() };
   j["margins"] = margins;
   // Non-empty feature set: InterfaceFeature::None suppresses the interface
   // control bar (ProcessInterface.h:144), which carries the frame's size grip.
   j["features"] = unsigned( Features() );

   j["resizableOk"] = !IsFixedWidth() && !IsFixedHeight()
                   && w1 == w0 + 300 && h1 == h0 + 300 && log1 > log0          // grows, chat log follows
                   && w2 < w0 && h2 < h0                                        // shrinks
                   && MinWidth() == minW && MinHeight() == minH                 // exactly our minimum
                   && w2 == minW && h2 == minH                                  // stops there
                   && w2 >= GUI->ChatLog.MinWidth() + margins                   // never below its content
                   && h2 >= GUI->ChatLog.MinHeight() + margins
                   && unsigned( Features() ) != unsigned( InterfaceFeature::None );
   return j;
}

// ── GUI Construction ─────────────────────────────────────────────

PICopilotInterface::GUIData::GUIData( PICopilotInterface& w )
{
   Mode_ComboBox.AddItem( "Copilot" );
   Mode_ComboBox.AddItem( "Advisor" );
   Mode_ComboBox.AddItem( "Guided" );
   Mode_ComboBox.SetToolTip( "<p><b>Copilot</b>: applies processes to the active image directly "
                             "(every change is in the view's History; undo as usual).</p>"
                             "<p><b>Advisor</b>: read-only; looks and advises, never changes the image.</p>"
                             "<p><b>Guided</b>: proposes each process and asks you before it runs.</p>"
                             "<p>A change applies from your next message.</p>" );
   {
      int mode = 0;
      Settings::Read( kModeKey, mode );
      Mode_ComboBox.SetCurrentItem( int( AgentModeFromIndex( mode ) ) );
   }
   Mode_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&PICopilotInterface::e_Mode_ItemSelected, w );

   IncludeView_CheckBox.SetText( "Include view" );
   IncludeView_CheckBox.SetChecked( true );
   IncludeView_CheckBox.SetToolTip( "<p>Send the active view with each message: an auto-stretched "
                                    "preview (display only) plus its geometry, statistics and FITS keywords.</p>" );

   Clear_Button.SetText( "New chat" );
   Clear_Button.SetToolTip( "<p>Start a new chat: the model forgets this conversation and the log is cleared. "
                            "Your images and their History are not touched.</p>" );
   Clear_Button.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Clear_Click, w );

   Config_ToolButton.SetText( String::UTF8ToUTF16( kGearUtf8 ) );
   Config_ToolButton.SetToolTip( "<p>Set your Anthropic API key.</p>" );
   Config_ToolButton.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Config_Click, w );

   Top_Sizer.SetSpacing( 4 );
   Top_Sizer.Add( Mode_ComboBox );
   Top_Sizer.Add( IncludeView_CheckBox );
   Top_Sizer.AddStretch();
   Top_Sizer.Add( Clear_Button );
   Top_Sizer.Add( Config_ToolButton );

   ChatLog.SetReadOnly();
   // Small minimum so the panel can shrink; the log takes all spare space
   // (stretch 100 in Global_Sizer) and expands on both axes.
   ChatLog.SetScaledMinSize( 240, 120 );
   ChatLog.EnableExpansion( true/*horz*/, true/*vert*/ );

   ChatInput.OnReturnPressed( (Edit::edit_event_handler)&PICopilotInterface::e_Input_ReturnPressed, w );

   Send_Button.SetText( kSendText );
   Send_Button.SetToolTip( "<p>Send the message (or press Return).</p>" );
   Send_Button.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Send_Click, w );

   Stop_Button.SetText( "Stop" );
   Stop_Button.SetToolTip( "<p>Stop after the current step. A process that is already running always "
                           "finishes; the request in flight is cancelled.</p>" );
   Stop_Button.OnClick( (Button::click_event_handler)&PICopilotInterface::e_Stop_Click, w );
   Stop_Button.Hide();

   Input_Sizer.SetSpacing( 4 );
   Input_Sizer.Add( ChatInput, 100 );
   Input_Sizer.Add( Send_Button );
   Input_Sizer.Add( Stop_Button );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Top_Sizer );
   Global_Sizer.Add( ChatLog, 100 );
   Global_Sizer.Add( Input_Sizer );

   Poll_Timer.SetInterval( 0.1 );
   Poll_Timer.SetPeriodic( true );
   Poll_Timer.OnTimer( (Timer::timer_event_handler)&PICopilotInterface::e_Poll_Timer, w );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   // Freely resizable: no min == max pin can survive (IsFixedWidth() is
   // MinWidth()==MaxWidth(), Control.h:401-410). SetVariableSize() is PCL's
   // own idiom (min 0, max int_max; Control.h:415-419); then an explicit
   // minimum, as PCL's resizable dialogs do after AdjustToContents()
   // (MultiViewSelectionDialog.cpp:184-185). SaveGeometry() always writes
   // Width/Height; RestoreGeometry() applies them only on an axis that is not
   // fixed (ProcessInterface.cpp:149-186), so with the window resizable the
   // user's size is restored.
   w.SetVariableSize();
   w.SetScaledMinSize( kMinPanelWidth, kMinPanelHeight );
}

} // namespace pcl
