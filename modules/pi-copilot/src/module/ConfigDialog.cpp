// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ConfigDialog.h"
#include "KeyStore.h"

#include <pcl/MessageBox.h>

namespace pcl
{

ConfigDialog::ConfigDialog()
{
   ApiKey_Label.SetText( "Anthropic API key:" );

   ApiKey_Edit.EnablePasswordMode();
   ApiKey_Edit.SetMinWidth( 400 );

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
   Global_Sizer.AddSpacing( 8 );
   Global_Sizer.Add( Buttons_Sizer );

   SetWindowTitle( "PI Copilot — API Key" );
   SetSizer( Global_Sizer );
   AdjustToContents();
   SetFixedSize();
}

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

} // namespace

void ConfigDialog::OK_Button_Click( Button& /*sender*/, bool /*checked*/ )
{
   String key = ApiKey_Edit.Text().Trimmed();

   if ( key.IsEmpty() )
   {
      // Explicit clear: an emptied field + OK removes the stored key.
      const String why = KeyStore::Clear();
      if ( !why.IsEmpty() )
         MessageBox( "<p>" + why + "</p>", "PI Copilot", StdIcon::Warning, StdButton::Ok ).Execute();
      result_ = String();
      Ok();
      return;
   }

   if ( !IsValidApiKey( key ) )
   {
      // Keep the dialog open so the user can correct the paste.
      MessageBox( "<p>The API key contains spaces, line breaks or other invalid characters.</p>"
                  "<p>Paste the key exactly as shown in the Anthropic Console. Nothing was saved.</p>",
                  "PI Copilot", StdIcon::Error, StdButton::Ok ).Execute();
      return;
   }

   result_ = key;
   const KeyStore::State st = KeyStore::Save( result_ );
   if ( st.where == KeyStore::Where::Settings )
      MessageBox( "<p>" + st.note + "</p>", "PI Copilot", StdIcon::Warning, StdButton::Ok ).Execute();
   Ok();
}

void ConfigDialog::Cancel_Button_Click( Button& /*sender*/, bool /*checked*/ )
{
   result_ = String();
   Cancel();
}

String ConfigDialog::Run( const String& currentKey )
{
   ApiKey_Edit.SetText( currentKey );
   result_ = String();
   Execute();
   return result_;
}

} // namespace pcl
