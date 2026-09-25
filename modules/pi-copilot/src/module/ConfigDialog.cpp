// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ConfigDialog.h"
#include "CopilotSettings.h"
#include "KeyStore.h"
#include "ModelCatalog.h"

#include <pcl/MessageBox.h>

namespace pcl
{

namespace
{

// Anthropic keys are printable ASCII. Anything else -- in particular CR/LF,
// which would split the x-api-key header line -- is rejected.
bool IsValidApiKey( const String& key )
{
   for ( String::const_iterator i = key.Begin(); i != key.End(); ++i )
      if ( *i < 0x21 || *i > 0x7E )
         return false;
   return true;
}

// MessageBox text is rich text: notes carry secret-tool's stderr and paths.
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
      else
         out += c;
   }
   return out;
}

// Disables the dialog's buttons for its lifetime (re-enabled even when a
// keyring call throws).
class ButtonsDisabled
{
public:
   ButtonsDisabled( PushButton& a, PushButton& b ) : m_a( a ), m_b( b )
   {
      m_a.Disable();
      m_b.Disable();
   }
   ~ButtonsDisabled()
   {
      m_a.Enable();
      m_b.Enable();
   }
   ButtonsDisabled( const ButtonsDisabled& ) = delete;
   ButtonsDisabled& operator =( const ButtonsDisabled& ) = delete;
private:
   PushButton& m_a;
   PushButton& m_b;
};

void Tell( const String& text, StdIcon::value_type icon )
{
   MessageBox( "<p>" + EscapeHtml( text ) + "</p>", "PI Copilot", icon, StdButton::Ok ).Execute();
}

const char* const kRunPjsrExplanation =
   "When allowed, PI Copilot may propose JavaScript to run inside PixInsight for jobs no process "
   "can do. Every script is shown to you in full and runs only if you click Run script. "
   "Scripts have full access to PixInsight and your files, and cannot be interrupted once running.";

} // namespace

ConfigDialog::ConfigDialog()
{
   ApiKey_Label.SetText( "Anthropic API key:" );
   ApiKey_Edit.EnablePasswordMode();
   ApiKey_Edit.SetMinWidth( 400 );
   KeyWhere_Label.EnableWordWrapping();

   Model_Label.SetText( "Model:" );
   for ( size_type i = 0; i < PICopilotModelCount; ++i )   // [0] is the default
      Model_ComboBox.AddItem( String( kPICopilotModels[i].label ) + (i == 0 ? " (default)" : "") );
   Model_ComboBox.SetToolTip( "<p>The Claude model PI Copilot uses from your next message on.</p>" );
   Model_Sizer.SetSpacing( 6 );
   Model_Sizer.Add( Model_Label );
   Model_Sizer.Add( Model_ComboBox, 100 );

   RunPjsr_CheckBox.SetText( "Allow scripts (run_pjsr)" );
   RunPjsr_CheckBox.OnClick( (Button::click_event_handler)&ConfigDialog::RunPjsr_Click, *this );
   RunPjsrInfo_Label.EnableWordWrapping();
   RunPjsrInfo_Label.SetText( kRunPjsrExplanation );

   Side_Label.SetText( "Default side:" );
   Side_ComboBox.AddItem( "Right" );
   Side_ComboBox.AddItem( "Left" );
   Side_ComboBox.SetToolTip( "<p>Changing it moves the panel to that edge of the primary screen now; after that, "
                             "PixInsight remembers where you put it.</p>" );
   Side_Sizer.SetSpacing( 6 );
   Side_Sizer.Add( Side_Label );
   Side_Sizer.Add( Side_ComboBox, 100 );

   OK_PushButton.SetText( "OK" );
   OK_PushButton.SetDefault();
   OK_PushButton.OnClick( (Button::click_event_handler)&ConfigDialog::OK_Button_Click, *this );
   Cancel_PushButton.SetText( "Cancel" );
   Cancel_PushButton.OnClick( (Button::click_event_handler)&ConfigDialog::Cancel_Button_Click, *this );
   Buttons_Sizer.SetSpacing( 8 );
   Buttons_Sizer.AddStretch();
   Buttons_Sizer.Add( OK_PushButton );
   Buttons_Sizer.Add( Cancel_PushButton );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( ApiKey_Label );
   Global_Sizer.Add( ApiKey_Edit );
   Global_Sizer.Add( KeyWhere_Label );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( Model_Sizer );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( RunPjsr_CheckBox );
   Global_Sizer.Add( RunPjsrInfo_Label );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( Side_Sizer );
   Global_Sizer.AddSpacing( 8 );
   Global_Sizer.Add( Buttons_Sizer );

   SetWindowTitle( String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 Settings" ) );
   SetSizer( Global_Sizer );
   AdjustToContents();
   SetFixedSize();
}

ConfigOutcome ConfigDialog::Run()
{
   // Label text is plain (Label rich text is off by default): no escaping.
   const KeyStore::State ks = KeyStore::Load();
   m_initialKey = ks.key;
   ApiKey_Edit.SetText( ks.key );
   String where = "Key: " + KeyStore::DescribeWhere( ks );
   if ( ks.where == KeyStore::Where::None && ks.note.IsEmpty() )
      where += " (if you saved one before, the system keyring may be locked)";
   if ( !ks.note.IsEmpty() )
      where += ". " + ks.note;
   KeyWhere_Label.SetText( where );
   // No note here: a saved model that is no longer offered shows as the
   // default, and its one-time note stays for the next message (see OK).
   const int mi = ModelIndex( CopilotSettings::LoadModel() );
   m_initialModel = mi < 0 ? 0 : mi;
   Model_ComboBox.SetCurrentItem( m_initialModel );
   RunPjsr_CheckBox.SetChecked( CopilotSettings::LoadRunPjsrEnabled() );
   m_initialSide = CopilotSettings::LoadPanelSide();
   Side_ComboBox.SetCurrentItem( int( m_initialSide ) );
   m_outcome = ConfigOutcome();
   AdjustToContents();
   SetFixedSize();
   Execute();
   return m_outcome;
}

void ConfigDialog::RunPjsr_Click( Button&, bool checked )
{
   if ( !checked )
      return;
   // Turning scripts ON is the one setting that widens what PI Copilot may
   // do: say exactly what it means and default to No.
   const bool yes = MessageBox( "<p><b>Allow scripts?</b></p><p>" + EscapeHtml( kRunPjsrExplanation ) + "</p>"
                                "<p>You can turn this off again here at any time.</p>",
                                "PI Copilot", StdIcon::Warning,
                                StdButton::Yes, StdButton::No, StdButton::NoButton, 1/*default: No*/, 1/*Esc: No*/ ).Execute()
                    == StdButton::Yes;
   if ( !yes )
      RunPjsr_CheckBox.SetChecked( false );
}

void ConfigDialog::OK_Button_Click( Button&, bool )
{
   const String key = ApiKey_Edit.Text().Trimmed();
   if ( !key.IsEmpty() && !IsValidApiKey( key ) )
   {
      // Keep the dialog open so the user can correct the paste.
      MessageBox( "<p>The API key contains spaces, line breaks or other invalid characters.</p>"
                  "<p>Paste the key exactly as shown in the Anthropic Console. Nothing was saved.</p>",
                  "PI Copilot", StdIcon::Error, StdButton::Ok ).Execute();
      return;
   }
   if ( key != m_initialKey )
   {
      // A keyring call can block on the desktop's unlock prompt: say so here.
      const String before = KeyWhere_Label.Text();
      KeyringWaitScope wait( [this, before]( bool waiting )
      {
         KeyWhere_Label.SetText( waiting ? String::UTF8ToUTF16( "Waiting for the system keyring\xE2\x80\xA6" ) : before );
      } );
      const ButtonsDisabled busy( OK_PushButton, Cancel_PushButton );
      if ( key.IsEmpty() )
      {
         const KeyStore::Cleared c = KeyStore::Clear();
         if ( !c.note.IsEmpty() )
            Tell( c.note, c.warning ? StdIcon::Warning : StdIcon::Information );
      }
      else
      {
         const KeyStore::State st = KeyStore::Save( key );
         if ( st.where == KeyStore::Where::Settings )
            Tell( st.note, StdIcon::Warning );
      }
   }
   // Saved only when the user changed it: OK on an untouched combo must not
   // overwrite a no-longer-offered saved model before its note is shown.
   const int mi = Model_ComboBox.CurrentItem();
   if ( mi != m_initialModel && mi >= 0 && size_type( mi ) < PICopilotModelCount )
      CopilotSettings::SaveModel( kPICopilotModels[mi].id );
   CopilotSettings::SaveRunPjsrEnabled( RunPjsr_CheckBox.IsChecked() );
   const PanelSide side = Side_ComboBox.CurrentItem() == 1 ? PanelSide::Left : PanelSide::Right;
   CopilotSettings::SavePanelSide( side );
   m_outcome.accepted = true;
   m_outcome.sideChanged = side != m_initialSide;
   Ok();
}

void ConfigDialog::Cancel_Button_Click( Button&, bool )
{
   m_outcome = ConfigOutcome();
   Cancel();
}

} // namespace pcl
