// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ConfigDialog.h"
#include "KeyStore.h"

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

void ConfigDialog::OK_Button_Click( Button& /*sender*/, bool /*checked*/ )
{
   result_ = ApiKey_Edit.Text();
   KeyStore::Save( result_ );
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
