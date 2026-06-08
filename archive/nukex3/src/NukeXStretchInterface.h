//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStretch Interface - GUI for stretch algorithm selection

#ifndef __NukeXStretchInterface_h
#define __NukeXStretchInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/NumericControl.h>
#include <pcl/ComboBox.h>
#include <pcl/CheckBox.h>
#include <pcl/SectionBar.h>
#include <pcl/Control.h>

#include "NukeXStretchInstance.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStretchInterface : public ProcessInterface
{
public:

   NukeXStretchInterface();
   virtual ~NukeXStretchInterface();

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

   NukeXStretchInstance m_instance;

   struct GUIData
   {
      GUIData( NukeXStretchInterface& );

      VerticalSizer     Global_Sizer;

      // Algorithm Section
      SectionBar        Algorithm_SectionBar;
      Control           Algorithm_Control;
      VerticalSizer     Algorithm_Sizer;
         HorizontalSizer   Algorithm_HSizer;
            Label             Algorithm_Label;
            ComboBox          Algorithm_ComboBox;
         Label             AlgorithmDescription_Label;

      // Parameters Section
      SectionBar        Parameters_SectionBar;
      Control           Parameters_Control;
      VerticalSizer     Parameters_Sizer;
         NumericControl    Contrast_NumericControl;
         NumericControl    Saturation_NumericControl;
         NumericControl    StretchStrength_NumericControl;
         NumericControl    Gamma_NumericControl;
         CheckBox          AutoBlackPoint_CheckBox;
         NumericControl    BlackPoint_NumericControl;
         NumericControl    WhitePoint_NumericControl;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void UpdateAlgorithmDescription();

   // Event handlers
   void e_ComboBoxItemSelected( ComboBox& sender, int itemIndex );
   void e_CheckBoxClick( Button& sender, bool checked );
   void e_NumericValueUpdated( NumericEdit& sender, double value );

   friend struct GUIData;
   friend class NukeXStretchProcess;
};

// ----------------------------------------------------------------------------

extern NukeXStretchInterface* TheNukeXStretchInterface;

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStretchInterface_h
