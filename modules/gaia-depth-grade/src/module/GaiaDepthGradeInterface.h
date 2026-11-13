// The GaiaDepthGrade window (plan Task 17): the Script Dialog GaiaDepthGradeDialog.js as a
// process interface, control for control.
//
//   Target: [view list] [Browse...]
//   [preview 760 x 420, autostretched recombine; orange border when stale] [1:1 inset 256]
//   Look (live -- Preview to apply)          | Matching (changing these needs re-Prepare)
//     Brightness Size Contrast Saturation    |   detect_threshold_sigma, match_tolerance_px
//     Halo sigma p_low p_high                |   [x] Offline (local Gaia DR3) -- fast
//     [ ] Grade the nebula too ...           |
//     Nebula depth, atmospheric, structure   |
//   Advanced (collapsed): the engine knobs the sidecar's --config TOML reached, the split
//     (StarXTerminator / supplied views), the output mode of process execution, the
//     astrometric index and Gaia database files.
//   [Prepare] [Preview / Update]                     [Execute] [Close]
//   status line ([GDG] console mirroring, the QA summary, ERROR: ...)
//
// Prepare runs RunPrepare (PrepareSession.h): the PixInsight work (working copy, plate solve,
// StarXTerminator, the Gaia database or the online catalogue -- NetworkTransfer needs the main
// thread, plan C14) on the GUI thread; the PCL-free detection + cross-match on a pcl::Thread, with
// a StatusMonitor on the console's standard status and paint events (no user input) pumped
// meanwhile. Preview and Execute render on such a thread too. Preview and Execute re-validate the
// target first (Review Focus 4): the session keeps the target's id, never a View or ImageWindow.
// The standard Apply runs the instance's full ExecuteOn.
#ifndef __GaiaDepthGradeInterface_h
#define __GaiaDepthGradeInterface_h

#include "GaiaDepthGradeInstance.h"
#include "PrepareSession.h"

#include <pcl/Bitmap.h>
#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Edit.h>
#include <pcl/Label.h>
#include <pcl/NumericControl.h>
#include <pcl/ProcessInterface.h>
#include <pcl/PushButton.h>
#include <pcl/SectionBar.h>
#include <pcl/Sizer.h>
#include <pcl/SpinBox.h>
#include <pcl/TextBox.h>
#include <pcl/Timer.h>
#include <pcl/ToolButton.h>
#include <pcl/ViewList.h>

#include <memory>

namespace pcl
{

class GaiaDepthGradeInterface : public ProcessInterface
{
public:

   GaiaDepthGradeInterface();
   virtual ~GaiaDepthGradeInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   InterfaceFeatures Features() const override;
   void ResetInstance() override;
   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;
   ProcessImplementation* NewProcess() const override;
   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;
   bool ImportProcess( const ProcessImplementation& ) override;

   static constexpr int kPreviewWidth = 760;  // PREVIEW_W
   static constexpr int kPreviewHeight = 420; // PREVIEW_H
   static constexpr int kInsetSize = 256;     // INSET_PX

private:

   GaiaDepthGradeInstance          instance;
   std::unique_ptr<PrepareSession> m_session;
   bool       m_prepared = false;
   bool       m_previewStale = false; // the dialog's previewStale (true once a target is set)
   bool       m_busy = false;
   bool       m_updating = false;    // UpdateControls is writing the controls: their events are ignored
   IsoString  m_targetId;            // the selected target's main view id
   int        m_insetCx = 0, m_insetCy = 0;
   Bitmap     m_previewBmp = Bitmap::Null();
   Bitmap     m_insetBmp = Bitmap::Null();
   String     m_status;

   struct GUIData
   {
      GUIData( GaiaDepthGradeInterface& );

      VerticalSizer   Global_Sizer;
         HorizontalSizer Target_Sizer;
            Label           Target_Label;
            ViewList        Target_ViewList;
            PushButton      Browse_PushButton;
         HorizontalSizer Preview_Sizer;
            Control         Preview_Control;
            VerticalSizer   Inset_Sizer;
               Label           Inset_Label;
               Control         Inset_Control;
         HorizontalSizer Controls_Sizer;
            VerticalSizer   Live_Sizer;
               Label           Live_Label;
               NumericControl  Brightness_NumericControl;
               NumericControl  Size_NumericControl;
               NumericControl  Contrast_NumericControl;
               NumericControl  Saturation_NumericControl;
               NumericControl  HaloSigma_NumericControl;
               NumericControl  PLow_NumericControl;
               NumericControl  PHigh_NumericControl;
               CheckBox        Nebula_CheckBox;
               NumericControl  NebulaStrength_NumericControl;
               NumericControl  NebulaAtmos_NumericControl;
               NumericControl  NebulaStructure_NumericControl;
            VerticalSizer   Prep_Sizer;
               Label           Prep_Label;
               NumericEdit     Detect_NumericEdit;
               NumericEdit     Match_NumericEdit;
               CheckBox        Offline_CheckBox;
         SectionBar      Advanced_SectionBar;
         Control         Advanced_Control;
         VerticalSizer   Advanced_Sizer;
            NumericControl  HaloGain_NumericControl;
            HorizontalSizer Engine1_Sizer;
               NumericEdit     DetectFWHM_NumericEdit;
               NumericEdit     MinMatchRate_NumericEdit;
               NumericEdit     Neutral_NumericEdit;
            HorizontalSizer Engine2_Sizer;
               CheckBox        MagLimit_CheckBox;
               NumericEdit     MagLimit_NumericEdit;
               NumericEdit     ParallaxSNR_NumericEdit;
            HorizontalSizer Engine3_Sizer;
               Label           HaloMinRefs_Label;
               SpinBox         HaloMinRefs_SpinBox;
               NumericEdit     HaloRadius_NumericEdit;
               NumericEdit     HaloBeta_NumericEdit;
            HorizontalSizer Split_Sizer;
               Label           Split_Label;
               ComboBox        Split_ComboBox;
            HorizontalSizer Supplied_Sizer;
               Label           Stars_Label;
               ViewList        Stars_ViewList;
               Label           Starless_Label;
               ViewList        Starless_ViewList;
            HorizontalSizer Output_Sizer;
               Label           Output_Label;
               ComboBox        Output_ComboBox;
            HorizontalSizer Index_Sizer;
               Label           Index_Label;
               Edit            Index_Edit;
               ToolButton      Index_ToolButton;
            HorizontalSizer Files_Sizer;
               Label           Files_Label;
               TextBox         Files_TextBox;
         HorizontalSizer Buttons_Sizer;
            PushButton      Prepare_PushButton;
            PushButton      Preview_PushButton;
            PushButton      Execute_PushButton;
            PushButton      Close_PushButton;
         Label           Status_Label;
#ifdef GDG_PROBES
      Timer           State_Timer;   // test builds: the GUI smoke's state file (GDG_GUI_STATE)
#endif
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void SetStatus( const String& msg );
   void SetError( const String& msg );
   void SetTarget( const View& view );
   void MarkPrepareStale();
   void MarkPreviewStale();
   void SetBusy( bool busy );
   WorkRunner GuiRunner( const String& what );
   gdg::Log GuiLog();
   bool DoPrepare();
   bool DoPreview();
   bool DoExecute();
   void DoBrowse();
   void ReadFilesText();
#ifdef GDG_PROBES
   void WriteState() const;
   void RecordAction( const char* action, bool ok );
   StringList m_statusLog; // every status line, for the GUI smoke (statusHistory)
   StringList m_actionLog; // "prepare ok", "preview error", ...: every Prepare/Preview/Execute's outcome
#endif

   void e_Click( Button& sender, bool checked );
   void e_Check( Button& sender, Button::check_state state );
   void e_ValueUpdated( NumericEdit& sender, double value );
   void e_SpinValueUpdated( SpinBox& sender, int value );
   void e_ItemSelected( ComboBox& sender, int itemIndex );
   void e_ViewSelected( ViewList& sender, View& view );
   void e_EditCompleted( Edit& sender );
   void e_TextUpdated( TextBox& sender, const String& text );
   void e_Paint( Control& sender, const pcl::Rect& updateRect );
   void e_MousePress( Control& sender, const pcl::Point& pos, int button, unsigned buttons, unsigned modifiers );
   void e_ToggleSection( SectionBar& sender, Control& section, bool start );
#ifdef GDG_PROBES
   void e_Timer( Timer& );
#endif
};

extern GaiaDepthGradeInterface* TheGaiaDepthGradeInterface;

} // namespace pcl

#endif // __GaiaDepthGradeInterface_h
