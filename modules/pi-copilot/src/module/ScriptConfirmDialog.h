// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ScriptConfirmDialog_h
#define PICopilot_ScriptConfirmDialog_h

#include <pcl/Dialog.h>
#include <pcl/Label.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/TextBox.h>

namespace pcl
{

// Shows a model-written script IN FULL before it may run. Default button and
// Esc: Don't run. Shown for every run_pjsr call in every mode. Root thread.
// (The panel passes Ask as ToolContext::confirmScript; the headless self-test
// injects its own callback instead, since a modal cannot run there.)
class ScriptConfirmDialog : public Dialog
{
public:

   // True only when the user clicks "Run script".
   static bool Ask( const String& purpose, const String& code, const IsoString& targetViewId );

private:

   ScriptConfirmDialog( const String& purpose, const String& code, const IsoString& targetViewId );

   VerticalSizer   Global_Sizer;
   Label           Info_Label;
   TextBox         Code_TextBox;
   HorizontalSizer Buttons_Sizer;
   PushButton      Run_PushButton;
   PushButton      DontRun_PushButton;

   bool m_run = false;

   void e_Click( Button& sender, bool checked );
};

} // namespace pcl

#endif // PICopilot_ScriptConfirmDialog_h
