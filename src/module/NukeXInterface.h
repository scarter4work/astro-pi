// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeXInterface_h
#define __NukeXInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/PushButton.h>
#include <pcl/TreeBox.h>
#include <pcl/SectionBar.h>
#include <pcl/Control.h>
#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Edit.h>

#include "NukeXInstance.h"

namespace pcl
{

class NukeXInterface : public ProcessInterface
{
public:
   NukeXInterface();
   virtual ~NukeXInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   String IconImageSVGFile() const override;
   InterfaceFeatures Features() const override;

   void ApplyInstance() const override;
   void ResetInstance() override;

   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;
   ProcessImplementation* NewProcess() const override;

   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;
   bool ImportProcess( const ProcessImplementation& ) override;

private:

   NukeXInstance instance;

   // ── GUI Controls ──────────────────────────────────────────────

   struct GUIData
   {
      GUIData( NukeXInterface& );

      VerticalSizer  Global_Sizer;

      // Light frames
      SectionBar     LightFrames_SectionBar;
      Control        LightFrames_Control;
      VerticalSizer  LightFrames_Sizer;
      TreeBox        LightFrames_TreeBox;
      HorizontalSizer LightFrames_Buttons_Sizer;
      PushButton     LightFrames_Add_Button;
      PushButton     LightFrames_Remove_Button;
      PushButton     LightFrames_Clear_Button;
      PushButton     LightFrames_SelectAll_Button;
      Label          LightFrames_Count_Label;

      // Flat frames
      SectionBar     FlatFrames_SectionBar;
      Control        FlatFrames_Control;
      VerticalSizer  FlatFrames_Sizer;
      TreeBox        FlatFrames_TreeBox;
      HorizontalSizer FlatFrames_Buttons_Sizer;
      PushButton     FlatFrames_Add_Button;
      PushButton     FlatFrames_Remove_Button;
      PushButton     FlatFrames_Clear_Button;
      Label          FlatFrames_Count_Label;

      // Options
      SectionBar     Options_SectionBar;
      Control        Options_Control;
      VerticalSizer  Options_Sizer;
      HorizontalSizer PrimaryStretch_Sizer;
      Label          PrimaryStretch_Label;
      ComboBox       PrimaryStretch_ComboBox;
      HorizontalSizer FinishingStretch_Sizer;
      Label          FinishingStretch_Label;
      ComboBox       FinishingStretch_ComboBox;
      HorizontalSizer GPU_Sizer;
      CheckBox       EnableGPU_CheckBox;

      // Phase 8 rating controls. Rate-last-run re-opens RatingDialog against
      // instance.lastRun (populated by ExecuteGlobal). SuppressRating persists
      // via TheNukeXProcess->set_rating_popup_suppressed (PCL Settings).
      HorizontalSizer Rating_Sizer;
      PushButton     RateLastRun_Button;
      CheckBox       SuppressRating_CheckBox;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void UpdateLightFramesList();
   void UpdateFlatFramesList();

   // ── Event handlers ────────────────────────────────────────────
   void e_LightAdd( Button& sender, bool checked );
   void e_LightRemove( Button& sender, bool checked );
   void e_LightClear( Button& sender, bool checked );
   void e_LightSelectAll( Button& sender, bool checked );
   void e_LightNodeDoubleClicked( TreeBox& sender, TreeBox::Node& node, int col );
   void e_FlatAdd( Button& sender, bool checked );
   void e_FlatRemove( Button& sender, bool checked );
   void e_FlatClear( Button& sender, bool checked );
   void e_ItemSelected( ComboBox& sender, int itemIndex );
   void e_OptionToggled( Button& sender, bool checked );

   // Phase 8 rating controls.
   void e_RateLastRun( Button& sender, bool checked );
   void e_SuppressRating( Button& sender, bool checked );
};

extern NukeXInterface* TheNukeXInterface;

} // namespace pcl

#endif // __NukeXInterface_h
