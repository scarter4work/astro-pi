// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ScriptConfirmDialog.h"
#include "PICopilotInterface.h"   // PlainText()

namespace pcl
{

namespace
{

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

} // namespace

ScriptConfirmDialog::ScriptConfirmDialog( const String& purpose, const String& code, const IsoString& targetViewId )
{
   Info_Label.EnableRichText();
   Info_Label.EnableWordWrapping();
   Info_Label.SetText( "<p><b>PI Copilot wants to run this script.</b></p>"
                       "<p>Purpose: " + EscapeHtml( purpose ) + "</p>"
                       "<p>Image it is about: " + (targetViewId.IsEmpty() ? String( "(none)" ) : EscapeHtml( String( targetViewId ) )) + "</p>"
                       "<p>The script runs with full access to PixInsight and your files. Once it starts it cannot "
                       "be stopped: if it never finishes, PixInsight has to be closed. Read the whole script before "
                       "you run it.</p>" );
   Code_TextBox.SetReadOnly();
   Code_TextBox.SetScaledMinSize( 640, 360 );
   Code_TextBox.SetText( PICopilotInterface::PlainText( code ) );

   Run_PushButton.SetText( "Run script" );
   Run_PushButton.OnClick( (Button::click_event_handler)&ScriptConfirmDialog::e_Click, *this );
   DontRun_PushButton.SetText( "Don't run" );
   DontRun_PushButton.SetDefault();   // Return and Esc both mean: don't run
   DontRun_PushButton.OnClick( (Button::click_event_handler)&ScriptConfirmDialog::e_Click, *this );
   Buttons_Sizer.SetSpacing( 8 );
   Buttons_Sizer.AddStretch();
   Buttons_Sizer.Add( Run_PushButton );
   Buttons_Sizer.Add( DontRun_PushButton );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Info_Label );
   Global_Sizer.Add( Code_TextBox, 100 );
   Global_Sizer.Add( Buttons_Sizer );

   SetWindowTitle( String::UTF8ToUTF16( "PI Copilot \xE2\x80\x94 Run a script?" ) );
   SetSizer( Global_Sizer );
   EnsureLayoutUpdated();
   AdjustToContents();

   // Keyboard focus starts on "Don't run" (Space activates the focused
   // button, Return the default one: both decline). Set again on show, in
   // case the window system moves focus while the dialog opens.
   DontRun_PushButton.Focus();
   OnShow( (Control::event_handler)&ScriptConfirmDialog::e_Show, *this );
}

void ScriptConfirmDialog::e_Show( Control& )
{
   DontRun_PushButton.Focus();
}

void ScriptConfirmDialog::e_Click( Button& sender, bool )
{
   m_run = &sender == &Run_PushButton;
   if ( m_run )
      Ok();
   else
      Cancel();
}

bool ScriptConfirmDialog::Ask( const String& purpose, const String& code, const IsoString& targetViewId )
{
   ScriptConfirmDialog d( purpose, code, targetViewId );
   d.m_run = false;
   return d.Execute() == StdDialogCode::Ok && d.m_run;
}

} // namespace pcl
