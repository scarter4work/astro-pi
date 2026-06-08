//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXInterface.h"
#include "NukeXProcess.h"
#include "NukeXParameters.h"

#include <pcl/RealTimePreview.h>
#include <pcl/Graphics.h>

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXInterface* TheNukeXInterface = nullptr;

// ----------------------------------------------------------------------------

NukeXInterface::NukeXInterface()
   : m_instance( TheNukeXProcess )
   , m_realTimePreviewUpdated( false )
{
   TheNukeXInterface = this;
}

// ----------------------------------------------------------------------------

NukeXInterface::~NukeXInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

// ----------------------------------------------------------------------------

IsoString NukeXInterface::Id() const
{
   return "NukeX";
}

// ----------------------------------------------------------------------------

MetaProcess* NukeXInterface::Process() const
{
   return TheNukeXProcess;
}

// ----------------------------------------------------------------------------

String NukeXInterface::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

InterfaceFeatures NukeXInterface::Features() const
{
   return InterfaceFeature::Default | InterfaceFeature::RealTimeButton;
}

// ----------------------------------------------------------------------------

void NukeXInterface::ApplyInstance() const
{
   m_instance.LaunchOnCurrentView();
}

// ----------------------------------------------------------------------------

void NukeXInterface::ResetInstance()
{
   NukeXInstance defaultInstance( TheNukeXProcess );
   ImportProcess( defaultInstance );
}

// ----------------------------------------------------------------------------

bool NukeXInterface::Launch( const MetaProcess& P, const ProcessImplementation*, bool& dynamic, unsigned& /*flags*/ )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "NukeX" );
      UpdateControls();
   }

   dynamic = false;
   return &P == TheNukeXProcess;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXInterface::NewProcess() const
{
   return new NukeXInstance( m_instance );
}

// ----------------------------------------------------------------------------

bool NukeXInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const NukeXInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not a NukeX instance.";
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXInterface::RequiresInstanceValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXInterface::ImportProcess( const ProcessImplementation& p )
{
   m_instance.Assign( p );
   UpdateControls();
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXInterface::WantsRealTimePreviewNotifications() const
{
   return true;
}

// ----------------------------------------------------------------------------

void NukeXInterface::RealTimePreviewOwnerChanged( ProcessInterface& )
{
   // Handle real-time preview ownership changes
}

// ----------------------------------------------------------------------------

bool NukeXInterface::GenerateRealTimePreview( UInt16Image& image, const View& view,
                                               const Rect& rect, int zoomLevel, String& info ) const
{
   m_realTimePreviewUpdated = false;

   if ( !view.IsMainView() )
      return false;

   // Convert to float image for processing
   Image floatImage( image );

   // Generate preview using the instance
   Image result = m_instance.GeneratePreview( floatImage, m_instance.p_previewMode );

   // Convert back to UInt16
   image.Apply( result );

   // Update info string
   if ( m_instance.p_processingMode == NXProcessingMode::FullyAutomatic )
      info = "NukeX Preview | FULLY AUTOMATIC";
   else
   {
      info = String().Format( "NukeX Preview | Strength: %.2f", m_instance.p_stretchStrength );

      if ( m_instance.p_autoSegment )
         info += " | Segmentation: ON";
      if ( m_instance.p_enableLRGB )
         info += " | LRGB";
   }

   m_realTimePreviewUpdated = true;
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXInterface::RequiresRealTimePreviewUpdate( const UInt16Image&, const View&,
                                                     const Rect&, int ) const
{
   return !m_realTimePreviewUpdated;
}

// ----------------------------------------------------------------------------

void NukeXInterface::UpdateControls()
{
   if ( GUI == nullptr )
      return;

   // Processing Mode
   bool isFullyAuto = ( m_instance.p_processingMode == NXProcessingMode::FullyAutomatic );
   GUI->Mode_ComboBox.SetCurrentItem( isFullyAuto ? 0 : 1 );

   // Disable all parameter sections when in fully automatic mode
   GUI->Algorithm_Control.Enable( !isFullyAuto );
   GUI->Stretch_Control.Enable( !isFullyAuto );
   GUI->ToneMapping_Control.Enable( !isFullyAuto );
   GUI->Regions_Control.Enable( !isFullyAuto );

   GUI->Algorithm_ComboBox.SetCurrentItem( m_instance.p_stretchAlgorithm );
   GUI->AutoSegment_CheckBox.SetChecked( m_instance.p_autoSegment );
   GUI->AutoSelect_CheckBox.SetChecked( m_instance.p_autoSelect );

   GUI->StretchStrength_NumericControl.SetValue( m_instance.p_stretchStrength );
   GUI->BlackPoint_NumericControl.SetValue( m_instance.p_blackPoint );
   GUI->GlobalContrast_NumericControl.SetValue( m_instance.p_globalContrast );
   GUI->SaturationBoost_NumericControl.SetValue( m_instance.p_saturationBoost );
   GUI->BlendRadius_NumericControl.SetValue( m_instance.p_blendRadius );

   GUI->EnableToneMapping_CheckBox.SetChecked( m_instance.p_enableToneMapping );
   GUI->AutoBlackPoint_CheckBox.SetChecked( m_instance.p_autoBlackPoint );
   GUI->WhitePoint_NumericControl.SetValue( m_instance.p_whitePoint );
   GUI->Gamma_NumericControl.SetValue( m_instance.p_gamma );

   // Enable/disable tone mapping controls based on enable state
   bool toneEnabled = m_instance.p_enableToneMapping;
   GUI->AutoBlackPoint_CheckBox.Enable( toneEnabled );
   GUI->WhitePoint_NumericControl.Enable( toneEnabled );
   GUI->Gamma_NumericControl.Enable( toneEnabled );

   // Disable manual black point if auto is enabled
   GUI->BlackPoint_NumericControl.Enable( !m_instance.p_autoBlackPoint || !toneEnabled );

   GUI->EnableStarCores_CheckBox.SetChecked( m_instance.p_enableStarCores );
   GUI->EnableStarHalos_CheckBox.SetChecked( m_instance.p_enableStarHalos );
   GUI->EnableNebulaBright_CheckBox.SetChecked( m_instance.p_enableNebulaBright );
   GUI->EnableNebulaFaint_CheckBox.SetChecked( m_instance.p_enableNebulaFaint );
   GUI->EnableDustLanes_CheckBox.SetChecked( m_instance.p_enableDustLanes );
   GUI->EnableGalaxyCore_CheckBox.SetChecked( m_instance.p_enableGalaxyCore );
   GUI->EnableGalaxyHalo_CheckBox.SetChecked( m_instance.p_enableGalaxyHalo );
   GUI->EnableGalaxyArms_CheckBox.SetChecked( m_instance.p_enableGalaxyArms );
   GUI->EnableBackground_CheckBox.SetChecked( m_instance.p_enableBackground );

   GUI->PreviewMode_ComboBox.SetCurrentItem( m_instance.p_previewMode );
   GUI->EnableLRGB_CheckBox.SetChecked( m_instance.p_enableLRGB );

   // Enable/disable auto-select based on algorithm selection
   bool isAuto = ( m_instance.p_stretchAlgorithm == NXStretchAlgorithm::Auto );
   GUI->AutoSelect_CheckBox.Enable( isAuto );
}

// ----------------------------------------------------------------------------

void NukeXInterface::UpdateRealTimePreview()
{
   m_realTimePreviewUpdated = false;
   if ( IsRealTimePreviewActive() )
   {
      RealTimePreview::Update();
   }
}

// ----------------------------------------------------------------------------

void NukeXInterface::e_ComboBoxItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->Mode_ComboBox )
   {
      m_instance.p_processingMode = ( itemIndex == 0 ) ?
         NXProcessingMode::FullyAutomatic : NXProcessingMode::Manual;
      UpdateControls();
      UpdateRealTimePreview();
   }
   else if ( sender == GUI->Algorithm_ComboBox )
   {
      m_instance.p_stretchAlgorithm = itemIndex;

      // Update auto-select checkbox state
      bool isAuto = ( itemIndex == NXStretchAlgorithm::Auto );
      GUI->AutoSelect_CheckBox.Enable( isAuto );
      if ( !isAuto )
      {
         m_instance.p_autoSelect = false;
         GUI->AutoSelect_CheckBox.SetChecked( false );
      }

      UpdateRealTimePreview();
   }
   else if ( sender == GUI->PreviewMode_ComboBox )
   {
      m_instance.p_previewMode = itemIndex;
      UpdateRealTimePreview();
   }
}

// ----------------------------------------------------------------------------

void NukeXInterface::e_CheckBoxClick( Button& sender, bool checked )
{
   if ( sender == GUI->AutoSegment_CheckBox )
      m_instance.p_autoSegment = checked;
   else if ( sender == GUI->AutoSelect_CheckBox )
      m_instance.p_autoSelect = checked;
   else if ( sender == GUI->EnableLRGB_CheckBox )
      m_instance.p_enableLRGB = checked;
   else if ( sender == GUI->EnableToneMapping_CheckBox )
   {
      m_instance.p_enableToneMapping = checked;
      // Update dependent controls
      GUI->AutoBlackPoint_CheckBox.Enable( checked );
      GUI->WhitePoint_NumericControl.Enable( checked );
      GUI->Gamma_NumericControl.Enable( checked );
      GUI->BlackPoint_NumericControl.Enable( !m_instance.p_autoBlackPoint || !checked );
   }
   else if ( sender == GUI->AutoBlackPoint_CheckBox )
   {
      m_instance.p_autoBlackPoint = checked;
      GUI->BlackPoint_NumericControl.Enable( !checked );
   }
   else if ( sender == GUI->EnableStarCores_CheckBox )
      m_instance.p_enableStarCores = checked;
   else if ( sender == GUI->EnableStarHalos_CheckBox )
      m_instance.p_enableStarHalos = checked;
   else if ( sender == GUI->EnableNebulaBright_CheckBox )
      m_instance.p_enableNebulaBright = checked;
   else if ( sender == GUI->EnableNebulaFaint_CheckBox )
      m_instance.p_enableNebulaFaint = checked;
   else if ( sender == GUI->EnableDustLanes_CheckBox )
      m_instance.p_enableDustLanes = checked;
   else if ( sender == GUI->EnableGalaxyCore_CheckBox )
      m_instance.p_enableGalaxyCore = checked;
   else if ( sender == GUI->EnableGalaxyHalo_CheckBox )
      m_instance.p_enableGalaxyHalo = checked;
   else if ( sender == GUI->EnableGalaxyArms_CheckBox )
      m_instance.p_enableGalaxyArms = checked;
   else if ( sender == GUI->EnableBackground_CheckBox )
      m_instance.p_enableBackground = checked;

   UpdateRealTimePreview();
}

// ----------------------------------------------------------------------------

void NukeXInterface::e_NumericValueUpdated( NumericEdit& sender, double value )
{
   if ( sender == GUI->StretchStrength_NumericControl )
      m_instance.p_stretchStrength = value;
   else if ( sender == GUI->BlackPoint_NumericControl )
      m_instance.p_blackPoint = value;
   else if ( sender == GUI->GlobalContrast_NumericControl )
      m_instance.p_globalContrast = value;
   else if ( sender == GUI->SaturationBoost_NumericControl )
      m_instance.p_saturationBoost = value;
   else if ( sender == GUI->BlendRadius_NumericControl )
      m_instance.p_blendRadius = value;
   else if ( sender == GUI->WhitePoint_NumericControl )
      m_instance.p_whitePoint = value;
   else if ( sender == GUI->Gamma_NumericControl )
      m_instance.p_gamma = value;

   UpdateRealTimePreview();
}

// ----------------------------------------------------------------------------

void NukeXInterface::e_SectionBarCheck( SectionBar& sender, bool checked )
{
   // Handle section bar toggles if needed
}

// ----------------------------------------------------------------------------
// GUIData Implementation
// ----------------------------------------------------------------------------

NukeXInterface::GUIData::GUIData( NukeXInterface& w )
{
   int labelWidth1 = w.Font().Width( String( "Stretch Strength:" ) + 'M' );
   int editWidth1 = w.Font().Width( String( '0', 10 ) );

   // Processing Mode Section
   Mode_SectionBar.SetTitle( "Processing Mode" );
   Mode_SectionBar.SetSection( Mode_Control );

   Mode_Label.SetText( "Mode:" );
   Mode_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   Mode_Label.SetMinWidth( labelWidth1 );

   Mode_ComboBox.AddItem( "Fully Automatic" );
   Mode_ComboBox.AddItem( "Manual" );
   Mode_ComboBox.SetToolTip( "<p><b>Fully Automatic</b>: NukeX analyzes your image and determines all "
      "processing parameters automatically using ML segmentation and image statistics. "
      "Just press the button and let NukeX do the rest.</p>"
      "<p><b>Manual</b>: Full control over all processing parameters.</p>" );
   Mode_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::e_ComboBoxItemSelected, w );

   Mode_HSizer.SetSpacing( 6 );
   Mode_HSizer.Add( Mode_Label );
   Mode_HSizer.Add( Mode_ComboBox, 100 );

   Mode_Sizer.SetSpacing( 4 );
   Mode_Sizer.Add( Mode_HSizer );

   Mode_Control.SetSizer( Mode_Sizer );

   // Algorithm Section
   Algorithm_SectionBar.SetTitle( "Algorithm" );
   Algorithm_SectionBar.SetSection( Algorithm_Control );

   Algorithm_Label.SetText( "Algorithm:" );
   Algorithm_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   Algorithm_Label.SetMinWidth( labelWidth1 );

   Algorithm_ComboBox.AddItem( "MTF (Midtones Transfer)" );
   Algorithm_ComboBox.AddItem( "Histogram Transformation" );
   Algorithm_ComboBox.AddItem( "GHS (Generalized Hyperbolic)" );
   Algorithm_ComboBox.AddItem( "ArcSinh" );
   Algorithm_ComboBox.AddItem( "Logarithmic" );
   Algorithm_ComboBox.AddItem( "Lumpton (SDSS HDR)" );
   Algorithm_ComboBox.AddItem( "RNC (Color Stretch)" );
   Algorithm_ComboBox.AddItem( "Photometric" );
   Algorithm_ComboBox.AddItem( "OTS (Optimal Transfer)" );
   Algorithm_ComboBox.AddItem( "SAS (Statistical Adaptive)" );
   Algorithm_ComboBox.AddItem( "Veralux" );
   Algorithm_ComboBox.AddItem( "Auto (AI Selected)" );
   Algorithm_ComboBox.SetToolTip( "<p>Select the stretch algorithm to apply.</p>"
                                   "<p><b>Auto</b>: Uses AI segmentation to select optimal algorithm per region.</p>" );
   Algorithm_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::e_ComboBoxItemSelected, w );

   Algorithm_HSizer.SetSpacing( 4 );
   Algorithm_HSizer.Add( Algorithm_Label );
   Algorithm_HSizer.Add( Algorithm_ComboBox, 100 );

   AutoSegment_CheckBox.SetText( "Auto Segmentation" );
   AutoSegment_CheckBox.SetToolTip( "<p>Use AI-powered semantic segmentation to identify distinct regions "
                                     "(stars, nebula, background, etc.) in the image.</p>" );
   AutoSegment_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   AutoSelect_CheckBox.SetText( "Auto Algorithm Selection" );
   AutoSelect_CheckBox.SetToolTip( "<p>Automatically select the best stretch algorithm for each detected region.</p>" );
   AutoSelect_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   AutoOptions_HSizer.SetSpacing( 16 );
   AutoOptions_HSizer.Add( AutoSegment_CheckBox );
   AutoOptions_HSizer.Add( AutoSelect_CheckBox );
   AutoOptions_HSizer.AddStretch();

   Algorithm_Sizer.SetSpacing( 4 );
   Algorithm_Sizer.Add( Algorithm_HSizer );
   Algorithm_Sizer.Add( AutoOptions_HSizer );

   Algorithm_Control.SetSizer( Algorithm_Sizer );

   // Stretch Controls Section
   Stretch_SectionBar.SetTitle( "Stretch Controls" );
   Stretch_SectionBar.SetSection( Stretch_Control );

   StretchStrength_NumericControl.label.SetText( "Stretch Strength:" );
   StretchStrength_NumericControl.label.SetMinWidth( labelWidth1 );
   StretchStrength_NumericControl.slider.SetRange( 0, 200 );
   StretchStrength_NumericControl.SetReal();
   StretchStrength_NumericControl.SetRange( TheNXStretchStrengthParameter->MinimumValue(),
                                             TheNXStretchStrengthParameter->MaximumValue() );
   StretchStrength_NumericControl.SetPrecision( TheNXStretchStrengthParameter->Precision() );
   StretchStrength_NumericControl.edit.SetMinWidth( editWidth1 );
   StretchStrength_NumericControl.SetToolTip( "<p>Overall stretch intensity multiplier.</p>" );
   StretchStrength_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXInterface::e_NumericValueUpdated, w );

   BlackPoint_NumericControl.label.SetText( "Black Point:" );
   BlackPoint_NumericControl.label.SetMinWidth( labelWidth1 );
   BlackPoint_NumericControl.slider.SetRange( 0, 200 );
   BlackPoint_NumericControl.SetReal();
   BlackPoint_NumericControl.SetRange( TheNXBlackPointParameter->MinimumValue(),
                                        TheNXBlackPointParameter->MaximumValue() );
   BlackPoint_NumericControl.SetPrecision( TheNXBlackPointParameter->Precision() );
   BlackPoint_NumericControl.edit.SetMinWidth( editWidth1 );
   BlackPoint_NumericControl.SetToolTip( "<p>Clip pixels below this value to black.</p>" );
   BlackPoint_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXInterface::e_NumericValueUpdated, w );

   GlobalContrast_NumericControl.label.SetText( "Global Contrast:" );
   GlobalContrast_NumericControl.label.SetMinWidth( labelWidth1 );
   GlobalContrast_NumericControl.slider.SetRange( 0, 200 );
   GlobalContrast_NumericControl.SetReal();
   GlobalContrast_NumericControl.SetRange( TheNXGlobalContrastParameter->MinimumValue(),
                                            TheNXGlobalContrastParameter->MaximumValue() );
   GlobalContrast_NumericControl.SetPrecision( TheNXGlobalContrastParameter->Precision() );
   GlobalContrast_NumericControl.edit.SetMinWidth( editWidth1 );
   GlobalContrast_NumericControl.SetToolTip( "<p>Final global contrast adjustment. 1.0 = no change.</p>" );
   GlobalContrast_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXInterface::e_NumericValueUpdated, w );

   SaturationBoost_NumericControl.label.SetText( "Saturation:" );
   SaturationBoost_NumericControl.label.SetMinWidth( labelWidth1 );
   SaturationBoost_NumericControl.slider.SetRange( 0, 200 );
   SaturationBoost_NumericControl.SetReal();
   SaturationBoost_NumericControl.SetRange( TheNXSaturationBoostParameter->MinimumValue(),
                                             TheNXSaturationBoostParameter->MaximumValue() );
   SaturationBoost_NumericControl.SetPrecision( TheNXSaturationBoostParameter->Precision() );
   SaturationBoost_NumericControl.edit.SetMinWidth( editWidth1 );
   SaturationBoost_NumericControl.SetToolTip( "<p>Color saturation adjustment. 1.0 = no change.</p>" );
   SaturationBoost_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXInterface::e_NumericValueUpdated, w );

   BlendRadius_NumericControl.label.SetText( "Blend Radius:" );
   BlendRadius_NumericControl.label.SetMinWidth( labelWidth1 );
   BlendRadius_NumericControl.slider.SetRange( 0, 500 );
   BlendRadius_NumericControl.SetReal();
   BlendRadius_NumericControl.SetRange( TheNXBlendRadiusParameter->MinimumValue(),
                                         TheNXBlendRadiusParameter->MaximumValue() );
   BlendRadius_NumericControl.SetPrecision( TheNXBlendRadiusParameter->Precision() );
   BlendRadius_NumericControl.edit.SetMinWidth( editWidth1 );
   BlendRadius_NumericControl.SetToolTip( "<p>Smoothing radius for blending between regions (pixels).</p>" );
   BlendRadius_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXInterface::e_NumericValueUpdated, w );

   Stretch_Sizer.SetSpacing( 4 );
   Stretch_Sizer.Add( StretchStrength_NumericControl );
   Stretch_Sizer.Add( BlackPoint_NumericControl );
   Stretch_Sizer.Add( GlobalContrast_NumericControl );
   Stretch_Sizer.Add( SaturationBoost_NumericControl );
   Stretch_Sizer.Add( BlendRadius_NumericControl );

   Stretch_Control.SetSizer( Stretch_Sizer );

   // Tone Mapping Section
   ToneMapping_SectionBar.SetTitle( "Tone Mapping" );
   ToneMapping_SectionBar.SetSection( ToneMapping_Control );

   EnableToneMapping_CheckBox.SetText( "Enable Tone Mapping" );
   EnableToneMapping_CheckBox.SetToolTip( "<p>Apply final tone adjustments for optimal display.</p>" );
   EnableToneMapping_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   AutoBlackPoint_CheckBox.SetText( "Auto Black Point" );
   AutoBlackPoint_CheckBox.SetToolTip( "<p>Automatically determine the black point from image statistics.</p>" );
   AutoBlackPoint_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   ToneMapping_Options_HSizer.SetSpacing( 16 );
   ToneMapping_Options_HSizer.Add( EnableToneMapping_CheckBox );
   ToneMapping_Options_HSizer.Add( AutoBlackPoint_CheckBox );
   ToneMapping_Options_HSizer.AddStretch();

   WhitePoint_NumericControl.label.SetText( "White Point:" );
   WhitePoint_NumericControl.label.SetMinWidth( labelWidth1 );
   WhitePoint_NumericControl.slider.SetRange( 0, 100 );
   WhitePoint_NumericControl.SetReal();
   WhitePoint_NumericControl.SetRange( TheNXWhitePointParameter->MinimumValue(),
                                        TheNXWhitePointParameter->MaximumValue() );
   WhitePoint_NumericControl.SetPrecision( TheNXWhitePointParameter->Precision() );
   WhitePoint_NumericControl.edit.SetMinWidth( editWidth1 );
   WhitePoint_NumericControl.SetToolTip( "<p>Maximum output value (white point).</p>" );
   WhitePoint_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXInterface::e_NumericValueUpdated, w );

   Gamma_NumericControl.label.SetText( "Gamma:" );
   Gamma_NumericControl.label.SetMinWidth( labelWidth1 );
   Gamma_NumericControl.slider.SetRange( 0, 200 );
   Gamma_NumericControl.SetReal();
   Gamma_NumericControl.SetRange( TheNXGammaParameter->MinimumValue(),
                                   TheNXGammaParameter->MaximumValue() );
   Gamma_NumericControl.SetPrecision( TheNXGammaParameter->Precision() );
   Gamma_NumericControl.edit.SetMinWidth( editWidth1 );
   Gamma_NumericControl.SetToolTip( "<p>Output gamma correction. 1.0 = linear.</p>" );
   Gamma_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXInterface::e_NumericValueUpdated, w );

   ToneMapping_Sizer.SetSpacing( 4 );
   ToneMapping_Sizer.Add( ToneMapping_Options_HSizer );
   ToneMapping_Sizer.Add( WhitePoint_NumericControl );
   ToneMapping_Sizer.Add( Gamma_NumericControl );

   ToneMapping_Control.SetSizer( ToneMapping_Sizer );

   // Regions Section
   Regions_SectionBar.SetTitle( "Region Controls" );
   Regions_SectionBar.SetSection( Regions_Control );

   EnableStarCores_CheckBox.SetText( "Star Cores" );
   EnableStarCores_CheckBox.SetToolTip( "<p>Enable processing of bright star cores.</p>" );
   EnableStarCores_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   EnableStarHalos_CheckBox.SetText( "Star Halos" );
   EnableStarHalos_CheckBox.SetToolTip( "<p>Enable processing of star halos and diffraction patterns.</p>" );
   EnableStarHalos_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   EnableNebulaBright_CheckBox.SetText( "Bright Nebula" );
   EnableNebulaBright_CheckBox.SetToolTip( "<p>Enable processing of bright nebular regions.</p>" );
   EnableNebulaBright_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   Regions_HSizer1.SetSpacing( 16 );
   Regions_HSizer1.Add( EnableStarCores_CheckBox );
   Regions_HSizer1.Add( EnableStarHalos_CheckBox );
   Regions_HSizer1.Add( EnableNebulaBright_CheckBox );
   Regions_HSizer1.AddStretch();

   EnableNebulaFaint_CheckBox.SetText( "Faint Nebula" );
   EnableNebulaFaint_CheckBox.SetToolTip( "<p>Enable processing of faint nebulosity and IFN.</p>" );
   EnableNebulaFaint_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   EnableDustLanes_CheckBox.SetText( "Dust Lanes" );
   EnableDustLanes_CheckBox.SetToolTip( "<p>Enable processing of dark nebulae and dust lanes.</p>" );
   EnableDustLanes_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   EnableBackground_CheckBox.SetText( "Background" );
   EnableBackground_CheckBox.SetToolTip( "<p>Enable processing of sky background.</p>" );
   EnableBackground_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   Regions_HSizer2.SetSpacing( 16 );
   Regions_HSizer2.Add( EnableNebulaFaint_CheckBox );
   Regions_HSizer2.Add( EnableDustLanes_CheckBox );
   Regions_HSizer2.Add( EnableBackground_CheckBox );
   Regions_HSizer2.AddStretch();

   EnableGalaxyCore_CheckBox.SetText( "Galaxy Core" );
   EnableGalaxyCore_CheckBox.SetToolTip( "<p>Enable processing of bright galactic nuclei.</p>" );
   EnableGalaxyCore_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   EnableGalaxyHalo_CheckBox.SetText( "Galaxy Halo" );
   EnableGalaxyHalo_CheckBox.SetToolTip( "<p>Enable processing of faint galactic halos.</p>" );
   EnableGalaxyHalo_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   EnableGalaxyArms_CheckBox.SetText( "Galaxy Arms" );
   EnableGalaxyArms_CheckBox.SetToolTip( "<p>Enable processing of spiral arm structures.</p>" );
   EnableGalaxyArms_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   Regions_HSizer3.SetSpacing( 16 );
   Regions_HSizer3.Add( EnableGalaxyCore_CheckBox );
   Regions_HSizer3.Add( EnableGalaxyHalo_CheckBox );
   Regions_HSizer3.Add( EnableGalaxyArms_CheckBox );
   Regions_HSizer3.AddStretch();

   Regions_Sizer.SetSpacing( 4 );
   Regions_Sizer.Add( Regions_HSizer1 );
   Regions_Sizer.Add( Regions_HSizer2 );
   Regions_Sizer.Add( Regions_HSizer3 );

   Regions_Control.SetSizer( Regions_Sizer );

   // Options Section
   Options_SectionBar.SetTitle( "Options" );
   Options_SectionBar.SetSection( Options_Control );

   PreviewMode_Label.SetText( "Preview Mode:" );
   PreviewMode_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   PreviewMode_Label.SetMinWidth( labelWidth1 );

   PreviewMode_ComboBox.AddItem( "Before/After" );
   PreviewMode_ComboBox.AddItem( "Region Map" );
   PreviewMode_ComboBox.AddItem( "Individual Regions" );
   PreviewMode_ComboBox.AddItem( "Stretched Result" );
   PreviewMode_ComboBox.SetToolTip( "<p>Select preview visualization mode.</p>" );
   PreviewMode_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::e_ComboBoxItemSelected, w );

   PreviewMode_HSizer.SetSpacing( 4 );
   PreviewMode_HSizer.Add( PreviewMode_Label );
   PreviewMode_HSizer.Add( PreviewMode_ComboBox, 100 );

   EnableLRGB_CheckBox.SetText( "LRGB Mode (Stretch Luminance Only)" );
   EnableLRGB_CheckBox.SetToolTip( "<p>Process luminance channel separately and recombine with color.</p>"
                                    "<p>This preserves color information while stretching only the luminance.</p>" );
   EnableLRGB_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_CheckBoxClick, w );

   Options_Sizer.SetSpacing( 4 );
   Options_Sizer.Add( PreviewMode_HSizer );
   Options_Sizer.Add( EnableLRGB_CheckBox );

   Options_Control.SetSizer( Options_Sizer );

   // Global Layout
   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Mode_SectionBar );
   Global_Sizer.Add( Mode_Control );
   Global_Sizer.Add( Algorithm_SectionBar );
   Global_Sizer.Add( Algorithm_Control );
   Global_Sizer.Add( Stretch_SectionBar );
   Global_Sizer.Add( Stretch_Control );
   Global_Sizer.Add( ToneMapping_SectionBar );
   Global_Sizer.Add( ToneMapping_Control );
   Global_Sizer.Add( Regions_SectionBar );
   Global_Sizer.Add( Regions_Control );
   Global_Sizer.Add( Options_SectionBar );
   Global_Sizer.Add( Options_Control );

   w.SetSizer( Global_Sizer );

   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   w.SetFixedSize();
}

// ----------------------------------------------------------------------------

} // namespace pcl
