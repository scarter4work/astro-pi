// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ConfigDialog_h
#define PICopilot_ConfigDialog_h

#include <pcl/Dialog.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/Edit.h>
#include <pcl/PushButton.h>

namespace pcl
{

// Modal dialog for entering/editing the user's own ("BYO") Anthropic API
// key. The key is masked in the edit field (Edit::EnablePasswordMode).
//
// On OK the field is trimmed, then:
//  - empty      -> the stored key is REMOVED (KeyStore::Clear()); Run()
//                  returns String(). This is how a user clears their key.
//  - invalid    -> any remaining character outside printable ASCII
//                  (0x21-0x7E: whitespace, CR/LF, other control or
//                  non-ASCII characters) is rejected with an error box and
//                  the dialog STAYS OPEN; nothing is saved. CR/LF in
//                  particular would inject extra HTTP header lines through
//                  NetworkTransfer::SetCustomHTTPHeaders().
//  - valid      -> persisted via KeyStore::Save() and returned.
// On Cancel: nothing is saved, and Run() returns String() -- the caller
// keeps whatever key it already had.
class ConfigDialog : public Dialog
{
public:

   ConfigDialog();

   // Pre-fills the edit field with currentKey, then runs the dialog
   // modally. Returns the saved key on OK, or String() on Cancel.
   String Run( const String& currentKey );

private:

   VerticalSizer   Global_Sizer;
   Label           ApiKey_Label;
   Edit            ApiKey_Edit;
   HorizontalSizer Buttons_Sizer;
   PushButton      OK_PushButton;
   PushButton      Cancel_PushButton;

   String result_;

   void OK_Button_Click( Button& sender, bool checked );
   void Cancel_Button_Click( Button& sender, bool checked );
};

} // namespace pcl

#endif // PICopilot_ConfigDialog_h
