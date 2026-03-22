//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack Interface - GUI for statistical stacking

#ifndef __NukeXStackInterface_h
#define __NukeXStackInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/NumericControl.h>
#include <pcl/ComboBox.h>
#include <pcl/CheckBox.h>
#include <pcl/PushButton.h>
#include <pcl/GroupBox.h>
#include <pcl/SectionBar.h>
#include <pcl/Control.h>
#include <pcl/TreeBox.h>

#include "NukeXStackInstance.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStackInterface : public ProcessInterface
{
public:

   NukeXStackInterface();
   virtual ~NukeXStackInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   String IconImageSVGFile() const override;
   InterfaceFeatures Features() const override;
   void ApplyInstance() const override;
   void ResetInstance() override;
   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& /*flags*/ ) override;
   ProcessImplementation* NewProcess() const override;
   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;
   bool ImportProcess( const ProcessImplementation& ) override;

private:

   NukeXStackInstance m_instance;

   struct GUIData
   {
      GUIData( NukeXStackInterface& );

      VerticalSizer     Global_Sizer;

      // Input Files Section
      SectionBar        InputFiles_SectionBar;
      Control           InputFiles_Control;
      VerticalSizer     InputFiles_Sizer;
         TreeBox           InputFiles_TreeBox;
         HorizontalSizer   InputFiles_Buttons_HSizer;
            PushButton        AddFiles_PushButton;
            PushButton        AddFolder_PushButton;
            PushButton        Remove_PushButton;
            PushButton        Clear_PushButton;
            PushButton        SelectAll_PushButton;
            PushButton        InvertSelection_PushButton;
         HorizontalSizer   InputFiles_Info_HSizer;
            Label             FileCount_Label;

      // Flat Files Section (optional calibration)
      SectionBar        FlatFiles_SectionBar;
      Control           FlatFiles_Control;
      VerticalSizer     FlatFiles_Sizer;
         TreeBox           FlatFiles_TreeBox;
         HorizontalSizer   FlatFiles_Buttons_HSizer;
            PushButton        AddFlats_PushButton;
            PushButton        RemoveFlats_PushButton;
            PushButton        ClearFlats_PushButton;
         HorizontalSizer   FlatFiles_Info_HSizer;
            Label             FlatCount_Label;

      // Outlier Rejection Section
      SectionBar        Outliers_SectionBar;
      Control           Outliers_Control;
      VerticalSizer     Outliers_Sizer;
         NumericControl    OutlierSigma_NumericControl;

      // Metadata Tiebreaker Section
      SectionBar        Quality_SectionBar;
      Control           Quality_Control;
      VerticalSizer     Quality_Sizer;
         CheckBox          EnableMetadataTiebreaker_CheckBox;

      // Output Section
      SectionBar        Output_SectionBar;
      Control           Output_Control;
      VerticalSizer     Output_Sizer;
         CheckBox          GenerateProvenance_CheckBox;
         CheckBox          GenerateDistMetadata_CheckBox;
         CheckBox          EnableAutoStretch_CheckBox;
         CheckBox          UseGPU_CheckBox;
         CheckBox          AdaptiveModels_CheckBox;
         NumericControl    BortleNumber_NumericControl;

      // Remediation Section
      SectionBar        Remediation_SectionBar;
      Control           Remediation_Control;
      VerticalSizer     Remediation_Sizer;
         CheckBox          EnableRemediation_CheckBox;
         CheckBox          EnableDustRemediation_CheckBox;
         CheckBox          EnableVignettingRemediation_CheckBox;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void UpdateFileList();
   void UpdateFileCountLabel();
   void AddFiles( const StringList& files );
   void UpdateFlatList();
   void UpdateFlatCountLabel();
   void AddFlatFiles( const StringList& files );

   // Event handlers
   void e_TreeBoxNodeActivated( TreeBox& sender, TreeBox::Node& node, int col );
   void e_TreeBoxNodeSelectionUpdated( TreeBox& sender );
   void e_TreeBoxNodeUpdated( TreeBox& sender, TreeBox::Node& node, int col );
   void e_ButtonClick( Button& sender, bool checked );
   void e_ComboBoxItemSelected( ComboBox& sender, int itemIndex );
   void e_CheckBoxClick( Button& sender, bool checked );
   void e_NumericValueUpdated( NumericEdit& sender, double value );
   void e_SectionToggle( SectionBar& sender, Control& section, bool start );

   friend struct GUIData;
   friend class NukeXStackProcess;
};

// ----------------------------------------------------------------------------

extern NukeXStackInterface* TheNukeXStackInterface;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackInterface_h
