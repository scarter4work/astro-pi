// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ConfigDialog_h
#define PICopilot_ConfigDialog_h

#include "PanelPlacement.h"

#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Dialog.h>
#include <pcl/Edit.h>
#include <pcl/Label.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>

namespace pcl
{

struct ConfigOutcome
{
   bool accepted = false;
   bool sideChanged = false;   // the caller re-applies the default placement
};

// PI Copilot settings: API key (masked; its storage shown), model, Allow
// scripts (run_pjsr; off by default), default panel side. On OK:
//  - the key is saved only if changed: emptied -> KeyStore::Clear(); any
//    character outside printable ASCII (0x21-0x7E; CR/LF would inject HTTP
//    header lines) -> error box, the dialog stays open, nothing is saved;
//    otherwise KeyStore::Save() (keyring, or Settings with a warning box);
//  - model, scripts and side are persisted (CopilotSettings).
// Checking Allow scripts asks first (default: No) and explains what it allows.
// Cancel saves nothing. Every text shown in a MessageBox is HTML-escaped.
class ConfigDialog : public Dialog
{
public:

   ConfigDialog();

   ConfigOutcome Run();

private:

   VerticalSizer   Global_Sizer;
   Label           ApiKey_Label;
   Edit            ApiKey_Edit;
   Label           KeyWhere_Label;
   HorizontalSizer Model_Sizer;
   Label           Model_Label;
   ComboBox        Model_ComboBox;
   CheckBox        RunPjsr_CheckBox;
   Label           RunPjsrInfo_Label;
   HorizontalSizer Side_Sizer;
   Label           Side_Label;
   ComboBox        Side_ComboBox;
   HorizontalSizer Buttons_Sizer;
   PushButton      OK_PushButton;
   PushButton      Cancel_PushButton;

   String        m_initialKey;
   PanelSide     m_initialSide = PanelSide::Right;
   ConfigOutcome m_outcome;

   void OK_Button_Click( Button& sender, bool checked );
   void Cancel_Button_Click( Button& sender, bool checked );
   void RunPjsr_Click( Button& sender, bool checked );
};

} // namespace pcl

#endif // PICopilot_ConfigDialog_h
