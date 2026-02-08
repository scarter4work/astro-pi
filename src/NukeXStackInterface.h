//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack Interface - GUI for intelligent pixel selection

#ifndef __NukeXStackInterface_h
#define __NukeXStackInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/NumericControl.h>
#include <pcl/SpinBox.h>
#include <pcl/ComboBox.h>
#include <pcl/CheckBox.h>
#include <pcl/PushButton.h>
#include <pcl/ToolButton.h>
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

   // GUI Elements
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

      // Selection Strategy Section
      SectionBar        Strategy_SectionBar;
      Control           Strategy_Control;
      VerticalSizer     Strategy_Sizer;
         HorizontalSizer   Strategy_HSizer;
            Label             Strategy_Label;
            ComboBox          Strategy_ComboBox;
         Label             StrategyDescription_Label;

      // ML Segmentation Section
      SectionBar        MLSegmentation_SectionBar;
      Control           MLSegmentation_Control;
      VerticalSizer     MLSegmentation_Sizer;
         CheckBox          EnableMLSegmentation_CheckBox;
         NumericControl    MinConfidence_NumericControl;
         HorizontalSizer   Context_HSizer;
            CheckBox          UseSpatialContext_CheckBox;
            CheckBox          UseTargetContext_CheckBox;

      // Transition Smoothing Section
      SectionBar        Smoothing_SectionBar;
      Control           Smoothing_Control;
      VerticalSizer     Smoothing_Sizer;
         CheckBox          EnableSmoothing_CheckBox;
         NumericControl    SmoothingStrength_NumericControl;
         NumericControl    TransitionThreshold_NumericControl;
         HorizontalSizer   SmoothingParams_HSizer;
            Label             TileSize_Label;
            SpinBox           TileSize_SpinBox;
            Label             SmoothingRadius_Label;
            SpinBox           SmoothingRadius_SpinBox;

      // Outlier Rejection Section
      SectionBar        Outliers_SectionBar;
      Control           Outliers_Control;
      VerticalSizer     Outliers_Sizer;
         NumericControl    OutlierSigma_NumericControl;
         CheckBox          GenerateMetadata_CheckBox;
         CheckBox          EnableAutoStretch_CheckBox;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void UpdateFileList();
   void UpdateFileCountLabel();
   void UpdateStrategyDescription();
   void AddFiles( const StringList& files );

   // Event handlers
   void e_TreeBoxNodeActivated( TreeBox& sender, TreeBox::Node& node, int col );
   void e_TreeBoxNodeSelectionUpdated( TreeBox& sender );
   void e_TreeBoxNodeUpdated( TreeBox& sender, TreeBox::Node& node, int col );
   void e_ButtonClick( Button& sender, bool checked );
   void e_ComboBoxItemSelected( ComboBox& sender, int itemIndex );
   void e_CheckBoxClick( Button& sender, bool checked );
   void e_NumericValueUpdated( NumericEdit& sender, double value );
   void e_SpinBoxValueUpdated( SpinBox& sender, int value );

   friend struct GUIData;
};

// ----------------------------------------------------------------------------

extern NukeXStackInterface* TheNukeXStackInterface;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackInterface_h
