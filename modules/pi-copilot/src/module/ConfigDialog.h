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
#include <pcl/SpinBox.h>
#include <pcl/ToolButton.h>

namespace pcl
{

struct ConfigOutcome
{
   bool accepted = false;
   bool sideChanged = false;   // the caller re-applies the default placement
};

// The ⚙ export folder rule (Ruling 18): "" (off; blanks count as empty) or an
// absolute, EXISTING folder -- PI Copilot never creates it. Returns "" when
// acceptable, else the message shown to the user.
String ValidateExportFolderSetting( const String& dir );

// PI Copilot settings: API key (masked; its storage shown), model, Allow
// scripts (run_pjsr; off by default), default panel side, and the image
// journey fields (Record journeys, Export folder for keepers, Keep unsaved
// journeys N days). On OK:
//  - an export folder ValidateExportFolderSetting() refuses -> error box, the
//    dialog stays open, nothing is saved;
//  - the key is saved only if changed: emptied -> KeyStore::Clear(); any
//    character outside printable ASCII (0x21-0x7E; CR/LF would inject HTTP
//    header lines) -> error box, the dialog stays open, nothing is saved;
//    otherwise KeyStore::Save() (keyring, or Settings with a warning box);
//  - model, scripts, side and the journey fields are persisted
//    (CopilotSettings) and applied to the running JourneyService.
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
   CheckBox        RecordJourneys_CheckBox;
   HorizontalSizer Export_Sizer;
   Label           Export_Label;
   Edit            Export_Edit;
   ToolButton      Export_ToolButton;
   HorizontalSizer Days_Sizer;
   Label           Days_Label;
   SpinBox         Days_SpinBox;
   HorizontalSizer Buttons_Sizer;
   PushButton      OK_PushButton;
   PushButton      Cancel_PushButton;

   String        m_initialKey;
   PanelSide     m_initialSide = PanelSide::Right;
   int           m_initialModel = 0;   // combo index shown when the dialog opened
   ConfigOutcome m_outcome;

   void OK_Button_Click( Button& sender, bool checked );
   void Cancel_Button_Click( Button& sender, bool checked );
   void RunPjsr_Click( Button& sender, bool checked );
   void Export_Browse_Click( Button& sender, bool checked );
};

} // namespace pcl

#endif // PICopilot_ConfigDialog_h
