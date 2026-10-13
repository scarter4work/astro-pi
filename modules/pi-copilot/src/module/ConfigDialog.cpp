// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ConfigDialog.h"
#include "CopilotSettings.h"
#include "DataConsent.h"
#include "JourneyTracker.h"
#include "KeyStore.h"
#include "ModelCatalog.h"

#include <pcl/File.h>
#include <pcl/FileDialog.h>
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

} // namespace

String ValidateExportFolderSetting( const String& dir )
{
   const String d = dir.Trimmed();
   if ( d.IsEmpty() )
      return String();
   if ( !d.StartsWith( '/' ) )
      return "The export folder must be an absolute folder (e.g. /mnt/qnap/astro_data/keepers): " + d;
   if ( File::Exists( d ) && !File::DirectoryExists( d ) )
      return "The export folder is a file, not a folder: " + d;
   if ( !File::DirectoryExists( d ) )
      return "The export folder does not exist: " + d + ". Create it (or mount the drive) first; PI Copilot never creates it.";
   return String();
}

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

   Consent_CheckBox.SetText( "Send data to Anthropic (required for chat and journey write-ups)" );
   Consent_CheckBox.OnClick( (Button::click_event_handler)&ConfigDialog::Consent_Click, *this );
   ConsentWhat_PushButton.SetText( String::UTF8ToUTF16( "What is sent\xE2\x80\xA6" ) );
   ConsentWhat_PushButton.OnClick( (Button::click_event_handler)&ConfigDialog::ConsentWhat_Click, *this );
   Consent_Sizer.SetSpacing( 6 );
   Consent_Sizer.Add( Consent_CheckBox );
   Consent_Sizer.AddStretch();
   Consent_Sizer.Add( ConsentWhat_PushButton );
   ConsentInfo_Label.EnableWordWrapping();

   Side_Label.SetText( "Default side:" );
   Side_ComboBox.AddItem( "Right" );
   Side_ComboBox.AddItem( "Left" );
   Side_ComboBox.SetToolTip( "<p>Changing it moves the panel to that edge of the primary screen now; after that, "
                             "PixInsight remembers where you put it.</p>" );
   Side_Sizer.SetSpacing( 6 );
   Side_Sizer.Add( Side_Label );
   Side_Sizer.Add( Side_ComboBox, 100 );

   RecordJourneys_CheckBox.SetText( "Record image journeys" );
   RecordJourneys_CheckBox.SetToolTip( "<p>Record each image's processing from the stacked master on (steps by you and by "
                                       "PI Copilot), locally on this computer. Location and observer keywords are never "
                                       "stored.</p>" );
   Export_Label.SetText( "Export folder for keepers:" );
   Export_Edit.SetToolTip( "<p>Optional. When set, each kept journey is also copied here, under "
                           "&lt;target&gt;/&lt;date&gt;-&lt;name&gt;. The folder must already exist.</p>" );
   Export_ToolButton.SetText( String::UTF8ToUTF16( "\xE2\x80\xA6" ) );
   Export_ToolButton.SetToolTip( "<p>Choose the folder.</p>" );
   Export_ToolButton.OnClick( (Button::click_event_handler)&ConfigDialog::Export_Browse_Click, *this );
   Export_Sizer.SetSpacing( 6 );
   Export_Sizer.Add( Export_Label );
   Export_Sizer.Add( Export_Edit, 100 );
   Export_Sizer.Add( Export_ToolButton );
   Days_Label.SetText( "Keep unsaved journeys for (days):" );
   Days_SpinBox.SetRange( 1, 3650 );
   Days_SpinBox.SetToolTip( "<p>Journeys you have not kept are deleted after this many days without changes. Kept "
                            "journeys are never deleted.</p>" );
   Days_Sizer.SetSpacing( 6 );
   Days_Sizer.Add( Days_Label );
   Days_Sizer.Add( Days_SpinBox );
   Days_Sizer.AddStretch();

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
   Global_Sizer.Add( Consent_Sizer );
   Global_Sizer.Add( ConsentInfo_Label );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( Side_Sizer );
   Global_Sizer.AddSpacing( 6 );
   Global_Sizer.Add( RecordJourneys_CheckBox );
   Global_Sizer.Add( Export_Sizer );
   Global_Sizer.Add( Days_Sizer );
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
   m_agreedInDialog = false;
   Consent_CheckBox.SetChecked( DataConsent::Granted() );
   ConsentInfo_Label.SetText( DataConsent::Granted()
      ? "You agreed on " + DataConsent::GrantedAt() + ". Uncheck and press OK to withdraw: nothing is sent after that."
      : String( "Not agreed: nothing is sent to Anthropic (api.anthropic.com) until you do." ) );
   m_initialSide = CopilotSettings::LoadPanelSide();
   Side_ComboBox.SetCurrentItem( int( m_initialSide ) );
   RecordJourneys_CheckBox.SetChecked( CopilotSettings::LoadRecordJourneys() );
   Export_Edit.SetText( CopilotSettings::LoadJourneyExportFolder() );
   Days_SpinBox.SetValue( CopilotSettings::LoadJourneyRetentionDays() );
   m_outcome = ConfigOutcome();
   AdjustToContents();
   SetFixedSize();
   Execute();
   return m_outcome;
}

void ConfigDialog::Consent_Click( Button&, bool checked )
{
   if ( !checked )
      return;   // withdrawn on OK
   // Agreeing is asked with the full list of what is sent (default: No).
   if ( DataConsent::Granted() || m_agreedInDialog )
      return;
   const bool yes = MessageBox( DataConsent::ConsentHtml(),
                                String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 data sent to Anthropic" ), StdIcon::Question,
                                StdButton::Yes, StdButton::No, StdButton::NoButton, 1/*default: No*/, 1/*Esc: No*/ ).Execute()
                    == StdButton::Yes;
   m_agreedInDialog = yes;
   if ( !yes )
      Consent_CheckBox.SetChecked( false );
}

void ConfigDialog::ConsentWhat_Click( Button&, bool )
{
   MessageBox( DataConsent::ConsentHtml( false ), String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 data sent to Anthropic" ),
               StdIcon::Information, StdButton::Ok ).Execute();
}

void ConfigDialog::OK_Button_Click( Button&, bool )
{
   // Ruling 18: a folder that cannot be written into is refused here, with the
   // dialog kept open, rather than failing at the first keep.
   const String folderProblem = ValidateExportFolderSetting( Export_Edit.Text() );
   if ( !folderProblem.IsEmpty() )
   {
      Tell( folderProblem + " Nothing was saved.", StdIcon::Error );
      return;
   }
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
   if ( Consent_CheckBox.IsChecked() && !DataConsent::Granted() && m_agreedInDialog )
      DataConsent::Grant();
   else if ( !Consent_CheckBox.IsChecked() && DataConsent::Granted() )
      DataConsent::Revoke();
   const PanelSide side = Side_ComboBox.CurrentItem() == 1 ? PanelSide::Left : PanelSide::Right;
   CopilotSettings::SavePanelSide( side );
   CopilotSettings::SaveRecordJourneys( RecordJourneys_CheckBox.IsChecked() );
   CopilotSettings::SaveJourneyExportFolder( Export_Edit.Text() );
   CopilotSettings::SaveJourneyRetentionDays( Days_SpinBox.Value() );
   JourneyService::Instance().ApplySettings();   // Record journeys takes effect now; days / folder are read per use
   m_outcome.accepted = true;
   m_outcome.sideChanged = side != m_initialSide;
   Ok();
}

void ConfigDialog::Export_Browse_Click( Button&, bool )
{
   GetDirectoryDialog d;
   d.SetCaption( "PI Copilot: export folder for kept journeys" );
   const String current = Export_Edit.Text().Trimmed();
   if ( !current.IsEmpty() && File::DirectoryExists( current ) )
      d.SetInitialPath( current );
   if ( d.Execute() )
      Export_Edit.SetText( d.Directory() );
}

void ConfigDialog::Cancel_Button_Click( Button&, bool )
{
   m_outcome = ConfigOutcome();
   Cancel();
}

} // namespace pcl
