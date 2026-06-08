//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// This file is part of NukeX.
//
// NukeX is free software: you can redistribute it and/or modify it under
// the terms of the MIT License.

#ifndef __NukeXInterface_h
#define __NukeXInterface_h

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
#include <pcl/Timer.h>
#include <pcl/Thread.h>

#include "NukeXInstance.h"

namespace pcl
{

// ----------------------------------------------------------------------------

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
   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& /*flags*/ ) override;
   ProcessImplementation* NewProcess() const override;
   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;
   bool ImportProcess( const ProcessImplementation& ) override;
   bool WantsRealTimePreviewNotifications() const override;
   void RealTimePreviewOwnerChanged( ProcessInterface& ) override;
   bool GenerateRealTimePreview( UInt16Image&, const View&, const Rect&, int, String& ) const override;
   bool RequiresRealTimePreviewUpdate( const UInt16Image&, const View&, const Rect&, int ) const override;

private:

   NukeXInstance m_instance;

   // Real-time preview state
   mutable bool m_realTimePreviewUpdated;

   // GUI Elements
   struct GUIData
   {
      GUIData( NukeXInterface& );

      VerticalSizer     Global_Sizer;

      // Algorithm Section
      SectionBar        Algorithm_SectionBar;
      Control           Algorithm_Control;
      VerticalSizer     Algorithm_Sizer;
         HorizontalSizer   Algorithm_HSizer;
            Label             Algorithm_Label;
            ComboBox          Algorithm_ComboBox;
         HorizontalSizer   AutoOptions_HSizer;
            CheckBox          AutoSegment_CheckBox;
            CheckBox          AutoSelect_CheckBox;

      // Stretch Controls Section
      SectionBar        Stretch_SectionBar;
      Control           Stretch_Control;
      VerticalSizer     Stretch_Sizer;
         NumericControl    StretchStrength_NumericControl;
         NumericControl    BlackPoint_NumericControl;
         NumericControl    GlobalContrast_NumericControl;
         NumericControl    SaturationBoost_NumericControl;
         NumericControl    BlendRadius_NumericControl;

      // Tone Mapping Section
      SectionBar        ToneMapping_SectionBar;
      Control           ToneMapping_Control;
      VerticalSizer     ToneMapping_Sizer;
         HorizontalSizer   ToneMapping_Options_HSizer;
            CheckBox          EnableToneMapping_CheckBox;
            CheckBox          AutoBlackPoint_CheckBox;
         NumericControl    WhitePoint_NumericControl;
         NumericControl    Gamma_NumericControl;

      // Regions Section
      SectionBar        Regions_SectionBar;
      Control           Regions_Control;
      VerticalSizer     Regions_Sizer;
         HorizontalSizer   Regions_HSizer1;
            CheckBox          EnableStarCores_CheckBox;
            CheckBox          EnableStarHalos_CheckBox;
            CheckBox          EnableNebulaBright_CheckBox;
         HorizontalSizer   Regions_HSizer2;
            CheckBox          EnableNebulaFaint_CheckBox;
            CheckBox          EnableDustLanes_CheckBox;
            CheckBox          EnableBackground_CheckBox;
         HorizontalSizer   Regions_HSizer3;
            CheckBox          EnableGalaxyCore_CheckBox;
            CheckBox          EnableGalaxyHalo_CheckBox;
            CheckBox          EnableGalaxyArms_CheckBox;

      // Options Section
      SectionBar        Options_SectionBar;
      Control           Options_Control;
      VerticalSizer     Options_Sizer;
         HorizontalSizer   PreviewMode_HSizer;
            Label             PreviewMode_Label;
            ComboBox          PreviewMode_ComboBox;
         CheckBox          EnableLRGB_CheckBox;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void UpdateRealTimePreview();

   // Event handlers
   void e_ComboBoxItemSelected( ComboBox& sender, int itemIndex );
   void e_CheckBoxClick( Button& sender, bool checked );
   void e_NumericValueUpdated( NumericEdit& sender, double value );
   void e_SectionBarCheck( SectionBar& sender, bool checked );

   friend struct GUIData;
};

// ----------------------------------------------------------------------------

extern NukeXInterface* TheNukeXInterface;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXInterface_h
