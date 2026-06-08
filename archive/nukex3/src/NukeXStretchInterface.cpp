//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStretchInterface.h"
#include "NukeXStretchProcess.h"
#include "NukeXStretchParameters.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStretchInterface* TheNukeXStretchInterface = nullptr;

// ----------------------------------------------------------------------------

NukeXStretchInterface::NukeXStretchInterface()
   : m_instance( TheNukeXStretchProcess )
{
   TheNukeXStretchInterface = this;
}

// ----------------------------------------------------------------------------

NukeXStretchInterface::~NukeXStretchInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

// ----------------------------------------------------------------------------

IsoString NukeXStretchInterface::Id() const
{
   return "NukeXStretch";
}

// ----------------------------------------------------------------------------

MetaProcess* NukeXStretchInterface::Process() const
{
   return TheNukeXStretchProcess;
}

// ----------------------------------------------------------------------------

String NukeXStretchInterface::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

InterfaceFeatures NukeXStretchInterface::Features() const
{
   return InterfaceFeature::Default;
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::ApplyInstance() const
{
   m_instance.LaunchOnCurrentView();
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::ResetInstance()
{
   NukeXStretchInstance defaultInstance( TheNukeXStretchProcess );
   ImportProcess( defaultInstance );
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::Launch( const MetaProcess& P, const ProcessImplementation* p, bool& dynamic, unsigned& /*flags*/ )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "NukeXStretch v3 \x2014 Image Stretching" );
      UpdateControls();
   }

   if ( p != nullptr )
      ImportProcess( *p );
   else
      ResetInstance();

   dynamic = false;
   return &P == TheNukeXStretchProcess;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStretchInterface::NewProcess() const
{
   return new NukeXStretchInstance( m_instance );
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const NukeXStretchInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not a NukeXStretch instance.";
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::RequiresInstanceValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::ImportProcess( const ProcessImplementation& p )
{
   m_instance.Assign( p );
   UpdateControls();
   return true;
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::UpdateControls()
{
   if ( GUI == nullptr )
      return;

   GUI->Algorithm_ComboBox.SetCurrentItem( m_instance.p_stretchAlgorithm );
   UpdateAlgorithmDescription();

   GUI->Contrast_NumericControl.SetValue( m_instance.p_contrast );
   GUI->Saturation_NumericControl.SetValue( m_instance.p_saturation );
   GUI->StretchStrength_NumericControl.SetValue( m_instance.p_stretchStrength );
   GUI->Gamma_NumericControl.SetValue( m_instance.p_gamma );
   GUI->AutoBlackPoint_CheckBox.SetChecked( m_instance.p_autoBlackPoint );
   GUI->BlackPoint_NumericControl.SetValue( m_instance.p_blackPoint );
   GUI->WhitePoint_NumericControl.SetValue( m_instance.p_whitePoint );

   GUI->BlackPoint_NumericControl.Enable( !m_instance.p_autoBlackPoint );
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::UpdateAlgorithmDescription()
{
   if ( GUI == nullptr )
      return;

   String desc;
   switch ( m_instance.p_stretchAlgorithm )
   {
   case NXSStretchAlgorithm::MTF:
      desc = "Midtone Transfer Function \x2014 classic PI stretch with midtone balance.";
      break;
   case NXSStretchAlgorithm::Histogram:
      desc = "Histogram equalization with adaptive clipping.";
      break;
   case NXSStretchAlgorithm::GHS:
      desc = "Generalized Hyperbolic Stretch \x2014 continuous family of stretch curves.";
      break;
   case NXSStretchAlgorithm::ArcSinh:
      desc = "Inverse hyperbolic sine \x2014 preserves color ratios in bright regions.";
      break;
   case NXSStretchAlgorithm::Log:
      desc = "Logarithmic stretch \x2014 good for bringing out faint detail.";
      break;
   case NXSStretchAlgorithm::Lumpton:
      desc = "Lupton et al. asinh mapping \x2014 designed for astronomical images.";
      break;
   case NXSStretchAlgorithm::RNC:
      desc = "Roger N. Clark's photometric color-preserving stretch.";
      break;
   case NXSStretchAlgorithm::Photometric:
      desc = "Photometric calibration-aware stretch using measured zero points.";
      break;
   case NXSStretchAlgorithm::OTS:
      desc = "Optimal Tone-mapping Stretch \x2014 adapts to local image statistics.";
      break;
   case NXSStretchAlgorithm::SAS:
      desc = "Statistical Adaptive Stretch \x2014 per-pixel adaptive curve.";
      break;
   case NXSStretchAlgorithm::Veralux:
      desc = "Veralux \x2014 balanced stretch preserving both faint and bright features.";
      break;
   case NXSStretchAlgorithm::Auto:
   default:
      desc = "Auto-select the best algorithm based on image statistics.";
      break;
   }

   GUI->AlgorithmDescription_Label.SetText( desc );
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::e_ComboBoxItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->Algorithm_ComboBox )
   {
      m_instance.p_stretchAlgorithm = itemIndex;
      UpdateAlgorithmDescription();
   }
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::e_CheckBoxClick( Button& sender, bool checked )
{
   if ( sender == GUI->AutoBlackPoint_CheckBox )
   {
      m_instance.p_autoBlackPoint = checked;
      GUI->BlackPoint_NumericControl.Enable( !checked );
   }
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::e_NumericValueUpdated( NumericEdit& sender, double value )
{
   if ( sender == GUI->Contrast_NumericControl )
      m_instance.p_contrast = value;
   else if ( sender == GUI->Saturation_NumericControl )
      m_instance.p_saturation = value;
   else if ( sender == GUI->StretchStrength_NumericControl )
      m_instance.p_stretchStrength = value;
   else if ( sender == GUI->Gamma_NumericControl )
      m_instance.p_gamma = value;
   else if ( sender == GUI->BlackPoint_NumericControl )
      m_instance.p_blackPoint = value;
   else if ( sender == GUI->WhitePoint_NumericControl )
      m_instance.p_whitePoint = value;
}

// ----------------------------------------------------------------------------
// GUIData Implementation
// ----------------------------------------------------------------------------

NukeXStretchInterface::GUIData::GUIData( NukeXStretchInterface& w )
{
   int labelWidth1 = w.Font().Width( String( "Stretch Strength:" ) + 'M' );
   int editWidth1 = w.Font().Width( String( '0', 10 ) );

   // =========================================================================
   // Algorithm Section
   // =========================================================================

   Algorithm_SectionBar.SetTitle( "Stretch Algorithm" );
   Algorithm_SectionBar.SetSection( Algorithm_Control );

   Algorithm_Label.SetText( "Algorithm:" );
   Algorithm_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   Algorithm_Label.SetMinWidth( labelWidth1 );

   Algorithm_ComboBox.AddItem( "MTF (Midtone Transfer)" );
   Algorithm_ComboBox.AddItem( "Histogram Equalization" );
   Algorithm_ComboBox.AddItem( "GHS (Generalized Hyperbolic)" );
   Algorithm_ComboBox.AddItem( "ArcSinh" );
   Algorithm_ComboBox.AddItem( "Logarithmic" );
   Algorithm_ComboBox.AddItem( "Lupton" );
   Algorithm_ComboBox.AddItem( "RNC (Roger N. Clark)" );
   Algorithm_ComboBox.AddItem( "Photometric" );
   Algorithm_ComboBox.AddItem( "OTS (Optimal Tone-mapping)" );
   Algorithm_ComboBox.AddItem( "SAS (Statistical Adaptive)" );
   Algorithm_ComboBox.AddItem( "Veralux" );
   Algorithm_ComboBox.AddItem( "Auto (Recommended)" );
   Algorithm_ComboBox.SetToolTip( "<p>Select the image stretching algorithm. "
                                   "<b>Auto</b> analyzes image statistics to choose the best one.</p>" );
   Algorithm_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXStretchInterface::e_ComboBoxItemSelected, w );

   Algorithm_HSizer.SetSpacing( 4 );
   Algorithm_HSizer.Add( Algorithm_Label );
   Algorithm_HSizer.Add( Algorithm_ComboBox, 100 );

   AlgorithmDescription_Label.SetTextColor( 0xFF808080 );
   AlgorithmDescription_Label.EnableWordWrapping();

   Algorithm_Sizer.SetSpacing( 4 );
   Algorithm_Sizer.Add( Algorithm_HSizer );
   Algorithm_Sizer.Add( AlgorithmDescription_Label );

   Algorithm_Control.SetSizer( Algorithm_Sizer );

   // =========================================================================
   // Parameters Section
   // =========================================================================

   Parameters_SectionBar.SetTitle( "Parameters" );
   Parameters_SectionBar.SetSection( Parameters_Control );

   Contrast_NumericControl.label.SetText( "Contrast:" );
   Contrast_NumericControl.label.SetMinWidth( labelWidth1 );
   Contrast_NumericControl.slider.SetRange( 0, 100 );
   Contrast_NumericControl.SetReal();
   Contrast_NumericControl.SetRange( TheNXSContrastParameter->MinimumValue(),
                                      TheNXSContrastParameter->MaximumValue() );
   Contrast_NumericControl.SetPrecision( TheNXSContrastParameter->Precision() );
   Contrast_NumericControl.edit.SetMinWidth( editWidth1 );
   Contrast_NumericControl.SetToolTip( "<p>Overall contrast of the stretch.</p>" );
   Contrast_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStretchInterface::e_NumericValueUpdated, w );

   Saturation_NumericControl.label.SetText( "Saturation:" );
   Saturation_NumericControl.label.SetMinWidth( labelWidth1 );
   Saturation_NumericControl.slider.SetRange( 0, 100 );
   Saturation_NumericControl.SetReal();
   Saturation_NumericControl.SetRange( TheNXSSaturationParameter->MinimumValue(),
                                        TheNXSSaturationParameter->MaximumValue() );
   Saturation_NumericControl.SetPrecision( TheNXSSaturationParameter->Precision() );
   Saturation_NumericControl.edit.SetMinWidth( editWidth1 );
   Saturation_NumericControl.SetToolTip( "<p>Color saturation boost applied after stretch.</p>" );
   Saturation_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStretchInterface::e_NumericValueUpdated, w );

   StretchStrength_NumericControl.label.SetText( "Stretch Strength:" );
   StretchStrength_NumericControl.label.SetMinWidth( labelWidth1 );
   StretchStrength_NumericControl.slider.SetRange( 0, 100 );
   StretchStrength_NumericControl.SetReal();
   StretchStrength_NumericControl.SetRange( TheNXSStretchStrengthParameter->MinimumValue(),
                                             TheNXSStretchStrengthParameter->MaximumValue() );
   StretchStrength_NumericControl.SetPrecision( TheNXSStretchStrengthParameter->Precision() );
   StretchStrength_NumericControl.edit.SetMinWidth( editWidth1 );
   StretchStrength_NumericControl.SetToolTip( "<p>How aggressively to stretch faint detail.</p>" );
   StretchStrength_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStretchInterface::e_NumericValueUpdated, w );

   Gamma_NumericControl.label.SetText( "Gamma:" );
   Gamma_NumericControl.label.SetMinWidth( labelWidth1 );
   Gamma_NumericControl.slider.SetRange( 0, 100 );
   Gamma_NumericControl.SetReal();
   Gamma_NumericControl.SetRange( TheNXSGammaParameter->MinimumValue(),
                                   TheNXSGammaParameter->MaximumValue() );
   Gamma_NumericControl.SetPrecision( TheNXSGammaParameter->Precision() );
   Gamma_NumericControl.edit.SetMinWidth( editWidth1 );
   Gamma_NumericControl.SetToolTip( "<p>Gamma curve exponent for tone response.</p>" );
   Gamma_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStretchInterface::e_NumericValueUpdated, w );

   AutoBlackPoint_CheckBox.SetText( "Auto Black Point" );
   AutoBlackPoint_CheckBox.SetToolTip( "<p>Automatically determine the black point from image statistics.</p>" );
   AutoBlackPoint_CheckBox.OnClick( (Button::click_event_handler)&NukeXStretchInterface::e_CheckBoxClick, w );

   BlackPoint_NumericControl.label.SetText( "Black Point:" );
   BlackPoint_NumericControl.label.SetMinWidth( labelWidth1 );
   BlackPoint_NumericControl.slider.SetRange( 0, 1000 );
   BlackPoint_NumericControl.SetReal();
   BlackPoint_NumericControl.SetRange( TheNXSBlackPointParameter->MinimumValue(),
                                        TheNXSBlackPointParameter->MaximumValue() );
   BlackPoint_NumericControl.SetPrecision( TheNXSBlackPointParameter->Precision() );
   BlackPoint_NumericControl.edit.SetMinWidth( editWidth1 );
   BlackPoint_NumericControl.SetToolTip( "<p>Manual black point level (0\x20131). Pixels at or below this "
                                          "value become pure black in the output.</p>" );
   BlackPoint_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStretchInterface::e_NumericValueUpdated, w );

   WhitePoint_NumericControl.label.SetText( "White Point:" );
   WhitePoint_NumericControl.label.SetMinWidth( labelWidth1 );
   WhitePoint_NumericControl.slider.SetRange( 0, 1000 );
   WhitePoint_NumericControl.SetReal();
   WhitePoint_NumericControl.SetRange( TheNXSWhitePointParameter->MinimumValue(),
                                        TheNXSWhitePointParameter->MaximumValue() );
   WhitePoint_NumericControl.SetPrecision( TheNXSWhitePointParameter->Precision() );
   WhitePoint_NumericControl.edit.SetMinWidth( editWidth1 );
   WhitePoint_NumericControl.SetToolTip( "<p>White point level (0\x20131). Pixels at or above this "
                                          "value become pure white.</p>" );
   WhitePoint_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStretchInterface::e_NumericValueUpdated, w );

   Parameters_Sizer.SetSpacing( 4 );
   Parameters_Sizer.Add( Contrast_NumericControl );
   Parameters_Sizer.Add( Saturation_NumericControl );
   Parameters_Sizer.Add( StretchStrength_NumericControl );
   Parameters_Sizer.Add( Gamma_NumericControl );
   Parameters_Sizer.Add( AutoBlackPoint_CheckBox );
   Parameters_Sizer.Add( BlackPoint_NumericControl );
   Parameters_Sizer.Add( WhitePoint_NumericControl );

   Parameters_Control.SetSizer( Parameters_Sizer );

   // =========================================================================
   // Global Layout
   // =========================================================================

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Algorithm_SectionBar );
   Global_Sizer.Add( Algorithm_Control );
   Global_Sizer.Add( Parameters_SectionBar );
   Global_Sizer.Add( Parameters_Control );

   w.SetSizer( Global_Sizer );

   w.EnsureLayoutUpdated();
   w.AdjustToContents();

   int minWidth = w.Font().Width( String( 'M', 60 ) );
   w.SetMinSize( minWidth, 400 );
}

// ----------------------------------------------------------------------------

} // namespace pcl
