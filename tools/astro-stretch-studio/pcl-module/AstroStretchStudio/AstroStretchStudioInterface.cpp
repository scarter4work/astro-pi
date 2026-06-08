// ----------------------------------------------------------------------------
// AstroStretchStudio Interface Implementation
// ----------------------------------------------------------------------------

#include "AstroStretchStudioInterface.h"
#include "AstroStretchStudioProcess.h"
#include "AstroStretchStudioParameters.h"

#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>
#include <pcl/RealTimePreview.h>

namespace pcl
{

// ----------------------------------------------------------------------------

AstroStretchStudioInterface* TheAstroStretchStudioInterface = nullptr;

// ----------------------------------------------------------------------------

AstroStretchStudioInterface::AstroStretchStudioInterface()
   : m_instance( TheAstroStretchStudioProcess )
{
   TheAstroStretchStudioInterface = this;
}

// ----------------------------------------------------------------------------

AstroStretchStudioInterface::~AstroStretchStudioInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

// ----------------------------------------------------------------------------

IsoString AstroStretchStudioInterface::Id() const
{
   return "AstroStretchStudio";
}

// ----------------------------------------------------------------------------

MetaProcess* AstroStretchStudioInterface::Process() const
{
   return TheAstroStretchStudioProcess;
}

// ----------------------------------------------------------------------------

IsoString AstroStretchStudioInterface::IconImageSVG() const
{
   return TheAstroStretchStudioProcess->IconImageSVG();
}

// ----------------------------------------------------------------------------

InterfaceFeatures AstroStretchStudioInterface::Features() const
{
   return InterfaceFeature::Default | InterfaceFeature::RealTimeButton;
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::ApplyInstance() const
{
   m_instance.LaunchOnCurrentView();
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::ResetInstance()
{
   m_instance.SetDefaultParameters();
   UpdateControls();
   UpdateRealTimePreview();
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInterface::Launch( const MetaProcess& P,
                                           const ProcessImplementation* p,
                                           bool& dynamic,
                                           unsigned& /*flags*/ )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "AstroStretch Studio" );
      UpdateImageList();
      UpdateControls();
   }

   if ( p != nullptr )
      ImportProcess( *p );

   dynamic = false;
   return &P == TheAstroStretchStudioProcess;
}

// ----------------------------------------------------------------------------

ProcessImplementation* AstroStretchStudioInterface::NewProcess() const
{
   return new AstroStretchStudioInstance( m_instance );
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInterface::ValidateProcess( const ProcessImplementation& p,
                                                    String& whyNot ) const
{
   if ( dynamic_cast<const AstroStretchStudioInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not an AstroStretchStudio instance.";
   return false;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInterface::RequiresInstanceValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInterface::ImportProcess( const ProcessImplementation& p )
{
   m_instance.Assign( p );
   UpdateControls();
   UpdateRealTimePreview();
   return true;
}

// ----------------------------------------------------------------------------
// Real-Time Preview
// ----------------------------------------------------------------------------

AstroStretchStudioInterface::RealTimeThread::RealTimeThread()
   : m_instance( TheAstroStretchStudioProcess )
{
}

void AstroStretchStudioInterface::RealTimeThread::Reset( const UInt16Image& image,
                                                          const AstroStretchStudioInstance& instance )
{
   image.ResetSelections();
   m_image.Assign( image );
   m_instance.Assign( instance );
}

void AstroStretchStudioInterface::RealTimeThread::Run()
{
   // Apply the stretch to the preview image
   m_instance.Preview( m_image );
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::RealTimePreviewUpdated( bool active )
{
   if ( GUI != nullptr )
      if ( active )
         RealTimePreview::SetOwner( *this );
      else
         RealTimePreview::SetOwner( ProcessInterface::Null() );
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInterface::RequiresRealTimePreviewUpdate( const UInt16Image&,
                                                                  const View&,
                                                                  const Rect&,
                                                                  int ) const
{
   return true;
}

// ----------------------------------------------------------------------------

bool AstroStretchStudioInterface::GenerateRealTimePreview( UInt16Image& image,
                                                            const View&,
                                                            const Rect&,
                                                            int,
                                                            String& ) const
{
   m_realTimeThread = new RealTimeThread;

   for ( ;; )
   {
      m_realTimeThread->Reset( image, m_instance );
      m_realTimeThread->Start();

      while ( m_realTimeThread->IsActive() )
      {
         ProcessEvents();

         if ( !IsRealTimePreviewActive() )
         {
            m_realTimeThread->Abort();
            m_realTimeThread->Wait();

            delete m_realTimeThread;
            m_realTimeThread = nullptr;
            return false;
         }
      }

      if ( !m_realTimeThread->IsAborted() )
      {
         image.Assign( m_realTimeThread->m_image );

         delete m_realTimeThread;
         m_realTimeThread = nullptr;
         return true;
      }
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::UpdateRealTimePreview()
{
   if ( IsRealTimePreviewActive() )
   {
      if ( m_realTimeThread != nullptr )
         m_realTimeThread->Abort();
      GUI->UpdateRealTimePreview_Timer.Start();
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::UpdateControls()
{
   if ( GUI == nullptr )
      return;

   // Algorithm selection
   GUI->Algorithm_ComboBox.SetCurrentItem( m_instance.p_algorithm );
   UpdateAlgorithmPanel();

   // OTS parameters
   GUI->OTS_ObjectType_ComboBox.SetCurrentItem( m_instance.p_otsObjectType );
   GUI->OTS_BackgroundTarget_NumericControl.SetValue( m_instance.p_otsBackgroundTarget );
   GUI->OTS_StretchIntensity_NumericControl.SetValue( m_instance.p_otsStretchIntensity );
   GUI->OTS_ProtectHighlights_NumericControl.SetValue( m_instance.p_otsProtectHighlights );
   GUI->OTS_PreserveColor_CheckBox.SetChecked( m_instance.p_otsPreserveColor );

   // SAS parameters
   GUI->SAS_NumScales_NumericControl.SetValue( m_instance.p_sasNumScales );
   GUI->SAS_BackgroundTarget_NumericControl.SetValue( m_instance.p_sasBackgroundTarget );
   GUI->SAS_FineScaleGain_NumericControl.SetValue( m_instance.p_sasFineScaleGain );
   GUI->SAS_MidScaleGain_NumericControl.SetValue( m_instance.p_sasMidScaleGain );
   GUI->SAS_CoarseScaleGain_NumericControl.SetValue( m_instance.p_sasCoarseScaleGain );
   GUI->SAS_CompressionAlpha_NumericControl.SetValue( m_instance.p_sasCompressionAlpha );
   GUI->SAS_HighlightProtection_NumericControl.SetValue( m_instance.p_sasHighlightProtection );
   GUI->SAS_NoiseThreshold_NumericControl.SetValue( m_instance.p_sasNoiseThreshold );
   GUI->SAS_FlattenBackground_CheckBox.SetChecked( m_instance.p_sasFlattenBackground );
   GUI->SAS_PreserveColor_CheckBox.SetChecked( m_instance.p_sasPreserveColor );

   // Zone HDR
   GUI->ZoneHDR_Enable_CheckBox.SetChecked( m_instance.p_zoneHDREnabled );
   GUI->ZoneHDR_PreviewMode_ComboBox.SetCurrentItem( m_instance.p_zonePreviewMode );

   // Update zone controls with current zone count
   UpdateZoneControls( m_instance.p_zoneCount );
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::UpdateAlgorithmPanel()
{
   if ( GUI == nullptr )
      return;

   // Show/hide algorithm panels based on selection
   GUI->OTS_Control.SetVisible( m_instance.p_algorithm == ASSAlgorithm::OTS );
   GUI->SAS_Control.SetVisible( m_instance.p_algorithm == ASSAlgorithm::SAS );
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::e_ItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->Algorithm_ComboBox )
   {
      m_instance.p_algorithm = itemIndex;
      UpdateAlgorithmPanel();
      UpdateRealTimePreview();
   }
   else if ( sender == GUI->OTS_ObjectType_ComboBox )
   {
      m_instance.p_otsObjectType = itemIndex;
      UpdateRealTimePreview();
   }
   else if ( sender == GUI->ZoneHDR_PreviewMode_ComboBox )
   {
      m_instance.p_zonePreviewMode = itemIndex;
      UpdateRealTimePreview();
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::e_ValueUpdated( NumericEdit& sender, double value )
{
   if ( sender == GUI->OTS_BackgroundTarget_NumericControl )
      m_instance.p_otsBackgroundTarget = value;
   else if ( sender == GUI->OTS_StretchIntensity_NumericControl )
      m_instance.p_otsStretchIntensity = value;
   else if ( sender == GUI->OTS_ProtectHighlights_NumericControl )
      m_instance.p_otsProtectHighlights = value;
   else if ( sender == GUI->SAS_NumScales_NumericControl )
      m_instance.p_sasNumScales = int( value );
   else if ( sender == GUI->SAS_BackgroundTarget_NumericControl )
      m_instance.p_sasBackgroundTarget = value;
   else if ( sender == GUI->SAS_FineScaleGain_NumericControl )
      m_instance.p_sasFineScaleGain = value;
   else if ( sender == GUI->SAS_MidScaleGain_NumericControl )
      m_instance.p_sasMidScaleGain = value;
   else if ( sender == GUI->SAS_CoarseScaleGain_NumericControl )
      m_instance.p_sasCoarseScaleGain = value;
   else if ( sender == GUI->SAS_CompressionAlpha_NumericControl )
      m_instance.p_sasCompressionAlpha = value;
   else if ( sender == GUI->SAS_HighlightProtection_NumericControl )
      m_instance.p_sasHighlightProtection = value;
   else if ( sender == GUI->SAS_NoiseThreshold_NumericControl )
      m_instance.p_sasNoiseThreshold = value;

   UpdateRealTimePreview();
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::e_Click( Button& sender, bool checked )
{
   if ( sender == GUI->OTS_PreserveColor_CheckBox )
      m_instance.p_otsPreserveColor = checked;
   else if ( sender == GUI->SAS_FlattenBackground_CheckBox )
      m_instance.p_sasFlattenBackground = checked;
   else if ( sender == GUI->SAS_PreserveColor_CheckBox )
      m_instance.p_sasPreserveColor = checked;
   else if ( sender == GUI->ZoneHDR_Enable_CheckBox )
   {
      m_instance.p_zoneHDREnabled = checked;
      if ( checked )
      {
         // Show default zones (detection disabled due to stability issues)
         UpdateZoneControls( 5 );
      }
      else
      {
         // When disabled, hide zone controls
         UpdateZoneControls( 0 );
      }
   }
   else if ( sender == GUI->ZoneHDR_DetectZones_Button )
   {
      // Show default zones (detection disabled due to stability issues)
      UpdateZoneControls( 5 );
   }
   else if ( sender == GUI->ZoneHDR_RefreshImages_Button )
   {
      // Refresh the list of open images (no-op for now)
   }

   UpdateRealTimePreview();
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::e_SectionToggle( SectionBar& sender, Control& section, bool start )
{
   if ( start )
      GUI->ZoneHDR_Control.SetVariableHeight();
   else
   {
      GUI->ZoneHDR_Control.SetFixedHeight();
      if ( GUI != nullptr )
      {
         SetVariableHeight();
         AdjustToContents();
         SetFixedHeight();
      }
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::e_UpdateRealTimePreview_Timer( Timer& sender )
{
   if ( m_realTimeThread != nullptr )
      if ( m_realTimeThread->IsActive() )
         return;

   if ( IsRealTimePreviewActive() )
      RealTimePreview::Update();
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::UpdateZoneControls( int zoneCount )
{
   if ( GUI == nullptr )
      return;

   zoneCount = Range( zoneCount, 0, GUIData::MaxZones );
   GUI->ActiveZoneCount = zoneCount;

   // Update m_instance zone count
   m_instance.p_zoneCount = zoneCount;

   // Show/hide zone controls based on count
   for ( int i = 0; i < GUIData::MaxZones; ++i )
   {
      GUIData::ZoneControls& zc = GUI->ZoneControls_Array[i];
      if ( i < zoneCount )
      {
         // Update zone values from instance
         zc.Zone_Intensity_NumericControl.SetValue( m_instance.p_zoneIntensity[i] );
         zc.Zone_Saturation_NumericControl.SetValue( m_instance.p_zoneSaturation[i] );
         zc.Zone_Solo_Button.SetChecked( m_instance.p_zoneSelectedIndex == i );
         zc.Zone_Control.Show();
      }
      else
      {
         zc.Zone_Control.Hide();
      }
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::UpdateZoneControlsWithNames( const Array<Zone>& zones )
{
   if ( GUI == nullptr )
      return;

   int zoneCount = Min( int( zones.Length() ), GUIData::MaxZones );
   GUI->ActiveZoneCount = zoneCount;
   m_instance.p_zoneCount = zoneCount;

   // Update zone controls with detected zone info
   for ( int i = 0; i < GUIData::MaxZones; ++i )
   {
      GUIData::ZoneControls& zc = GUI->ZoneControls_Array[i];
      if ( i < zoneCount )
      {
         const Zone& zone = zones[i];

         // Update zone name with luminance range info
         // Note: Don't use zone.name.c_str() with %s - PCL String::c_str() returns char16_t*
         String label = zone.name + String().Format( " (%.0f%%-%.0f%%)",
                                                     zone.lowerBound * 100,
                                                     zone.upperBound * 100 );
         zc.Zone_Name_Label.SetText( label );

         // Update slider values from instance
         zc.Zone_Intensity_NumericControl.SetValue( m_instance.p_zoneIntensity[i] );
         zc.Zone_Saturation_NumericControl.SetValue( m_instance.p_zoneSaturation[i] );
         zc.Zone_Solo_Button.SetChecked( m_instance.p_zoneSelectedIndex == i );
         zc.Zone_Control.Show();
      }
      else
      {
         zc.Zone_Control.Hide();
      }
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::UpdateImageList()
{
   if ( GUI == nullptr )
      return;

   // Remember current selection
   String currentSelection;
   int currentIndex = GUI->ZoneHDR_ImageSelect_ComboBox.CurrentItem();
   if ( currentIndex >= 0 && currentIndex < GUI->ZoneHDR_ImageSelect_ComboBox.NumberOfItems() )
      currentSelection = GUI->ZoneHDR_ImageSelect_ComboBox.ItemText( currentIndex );

   // Clear and repopulate
   GUI->ZoneHDR_ImageSelect_ComboBox.Clear();
   GUI->ZoneHDR_ImageSelect_ComboBox.AddItem( "<Active Window>" );

   // Get all open image windows
   Array<ImageWindow> windows = ImageWindow::AllWindows();
   int newIndex = 0; // Default to "<Active Window>"

   for ( size_t i = 0; i < windows.Length(); ++i )
   {
      if ( !windows[i].IsNull() )
      {
         View mainView = windows[i].MainView();
         if ( !mainView.IsNull() )
         {
            String id = mainView.Id();
            GUI->ZoneHDR_ImageSelect_ComboBox.AddItem( id );

            // Check if this was the previous selection
            if ( id == currentSelection )
               newIndex = int( i + 1 ); // +1 because of "<Active Window>" at index 0
         }
      }
   }

   GUI->ZoneHDR_ImageSelect_ComboBox.SetCurrentItem( newIndex );
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::DetectZonesFromCurrentView()
{
   if ( GUI == nullptr )
      return;

   // Get the selected image window
   ImageWindow window;
   int selectedIndex = GUI->ZoneHDR_ImageSelect_ComboBox.CurrentItem();

   if ( selectedIndex <= 0 )
   {
      // "<Active Window>" or invalid selection - use active window
      window = ImageWindow::ActiveWindow();
   }
   else
   {
      // Get the specific window by name
      int numItems = GUI->ZoneHDR_ImageSelect_ComboBox.NumberOfItems();
      if ( selectedIndex < numItems )
      {
         String selectedId = GUI->ZoneHDR_ImageSelect_ComboBox.ItemText( selectedIndex );
         Array<ImageWindow> windows = ImageWindow::AllWindows();
         for ( size_t i = 0; i < windows.Length(); ++i )
         {
            if ( !windows[i].IsNull() )
            {
               View mv = windows[i].MainView();
               if ( !mv.IsNull() && mv.Id() == selectedId )
               {
                  window = windows[i];
                  break;
               }
            }
         }
      }
      // If window not found, fall back to active window
      if ( window.IsNull() )
         window = ImageWindow::ActiveWindow();
   }

   if ( window.IsNull() )
   {
      // No valid window, show default 5 zones
      UpdateZoneControls( 5 );
      return;
   }

   View view = window.MainView();
   if ( view.IsNull() )
   {
      UpdateZoneControls( 5 );
      return;
   }

   // Read the image
   ImageVariant imageVar = view.Image();
   if ( !imageVar || imageVar.Width() == 0 || imageVar.Height() == 0 )
   {
      UpdateZoneControls( 5 );
      return;
   }

   // Extract luminance directly to our Image
   int width = imageVar.Width();
   int height = imageVar.Height();
   int numChannels = imageVar.NumberOfChannels();

   Image L;
   L.AllocateData( width, height, 1 );

   if ( numChannels >= 3 )
   {
      // Color image - compute luminance from RGB
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
         {
            double r = imageVar( x, y, 0 );
            double g = imageVar( x, y, 1 );
            double b = imageVar( x, y, 2 );
            L( x, y ) = float( 0.2126 * r + 0.7152 * g + 0.0722 * b );
         }
   }
   else
   {
      // Grayscale - copy directly
      for ( int y = 0; y < height; ++y )
         for ( int x = 0; x < width; ++x )
            L( x, y ) = float( imageVar( x, y, 0 ) );
   }

   // Run zone detection
   ZoneHDREngine engine;
   int numZones = engine.DetectZones( L );

   if ( numZones > 0 )
   {
      // Update UI with detected zones
      UpdateZoneControlsWithNames( engine.Zones() );
   }
   else
   {
      // Fallback to default zones
      UpdateZoneControls( 5 );
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::e_ZoneValueUpdated( NumericEdit& sender, double value )
{
   if ( GUI == nullptr )
      return;

   // Find which zone this slider belongs to
   for ( int i = 0; i < GUIData::MaxZones; ++i )
   {
      GUIData::ZoneControls& zc = GUI->ZoneControls_Array[i];
      if ( sender == zc.Zone_Intensity_NumericControl )
      {
         m_instance.p_zoneIntensity[i] = value;
         UpdateRealTimePreview();
         return;
      }
      else if ( sender == zc.Zone_Saturation_NumericControl )
      {
         m_instance.p_zoneSaturation[i] = value;
         UpdateRealTimePreview();
         return;
      }
   }
}

// ----------------------------------------------------------------------------

void AstroStretchStudioInterface::e_ZoneSoloClick( Button& sender, bool checked )
{
   if ( GUI == nullptr )
      return;

   // Find which zone's solo button was clicked
   for ( int i = 0; i < GUIData::MaxZones; ++i )
   {
      GUIData::ZoneControls& zc = GUI->ZoneControls_Array[i];
      if ( sender == zc.Zone_Solo_Button )
      {
         // Uncheck all other solo buttons
         for ( int j = 0; j < GUIData::MaxZones; ++j )
         {
            if ( j != i )
               GUI->ZoneControls_Array[j].Zone_Solo_Button.SetChecked( false );
         }

         // Update selected zone index
         m_instance.p_zoneSelectedIndex = checked ? i : -1;

         // If solo is checked, set preview mode to SoloZone
         if ( checked && m_instance.p_zonePreviewMode != ASSZonePreviewMode::SoloZone )
         {
            m_instance.p_zonePreviewMode = ASSZonePreviewMode::SoloZone;
            GUI->ZoneHDR_PreviewMode_ComboBox.SetCurrentItem( ASSZonePreviewMode::SoloZone );
         }

         UpdateRealTimePreview();
         return;
      }
   }
}

// ----------------------------------------------------------------------------
// GUI Construction
// ----------------------------------------------------------------------------

AstroStretchStudioInterface::GUIData::GUIData( AstroStretchStudioInterface& w )
{
   int labelWidth1 = w.Font().Width( String( "Highlight Protection:" ) + 'M' );

   // -------------------------------------------------------------------------
   // Algorithm Selection
   // -------------------------------------------------------------------------

   Algorithm_Label.SetText( "Algorithm:" );
   Algorithm_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   Algorithm_Label.SetMinWidth( labelWidth1 );

   Algorithm_ComboBox.AddItem( "Optimal Transport Stretch (OTS)" );
   Algorithm_ComboBox.AddItem( "Starlet Arctan Stretch (SAS)" );
   // Future algorithms will be added here
   Algorithm_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&AstroStretchStudioInterface::e_ItemSelected, w );

   Algorithm_Sizer.SetSpacing( 4 );
   Algorithm_Sizer.Add( Algorithm_Label );
   Algorithm_Sizer.Add( Algorithm_ComboBox, 100 );

   // -------------------------------------------------------------------------
   // OTS Panel - Optimal Transport Stretch
   // -------------------------------------------------------------------------

   OTS_ObjectType_Label.SetText( "Object Type:" );
   OTS_ObjectType_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   OTS_ObjectType_Label.SetMinWidth( labelWidth1 );

   OTS_ObjectType_ComboBox.AddItem( "Nebula" );
   OTS_ObjectType_ComboBox.AddItem( "Galaxy" );
   OTS_ObjectType_ComboBox.AddItem( "Star Cluster" );
   OTS_ObjectType_ComboBox.AddItem( "Dark Nebula" );
   OTS_ObjectType_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&AstroStretchStudioInterface::e_ItemSelected, w );

   OTS_ObjectType_Sizer.SetSpacing( 4 );
   OTS_ObjectType_Sizer.Add( OTS_ObjectType_Label );
   OTS_ObjectType_Sizer.Add( OTS_ObjectType_ComboBox, 100 );

   OTS_BackgroundTarget_NumericControl.label.SetText( "Background Target:" );
   OTS_BackgroundTarget_NumericControl.label.SetMinWidth( labelWidth1 );
   OTS_BackgroundTarget_NumericControl.slider.SetRange( 0, 1000 );
   OTS_BackgroundTarget_NumericControl.SetReal();
   OTS_BackgroundTarget_NumericControl.SetRange( 0.0, 0.5 );
   OTS_BackgroundTarget_NumericControl.SetPrecision( 4 );
   OTS_BackgroundTarget_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   OTS_StretchIntensity_NumericControl.label.SetText( "Stretch Intensity:" );
   OTS_StretchIntensity_NumericControl.label.SetMinWidth( labelWidth1 );
   OTS_StretchIntensity_NumericControl.slider.SetRange( 0, 1000 );
   OTS_StretchIntensity_NumericControl.SetReal();
   OTS_StretchIntensity_NumericControl.SetRange( 0.0, 2.0 );
   OTS_StretchIntensity_NumericControl.SetPrecision( 3 );
   OTS_StretchIntensity_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   OTS_ProtectHighlights_NumericControl.label.SetText( "Protect Highlights:" );
   OTS_ProtectHighlights_NumericControl.label.SetMinWidth( labelWidth1 );
   OTS_ProtectHighlights_NumericControl.slider.SetRange( 0, 1000 );
   OTS_ProtectHighlights_NumericControl.SetReal();
   OTS_ProtectHighlights_NumericControl.SetRange( 0.0, 1.0 );
   OTS_ProtectHighlights_NumericControl.SetPrecision( 3 );
   OTS_ProtectHighlights_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   OTS_PreserveColor_CheckBox.SetText( "Preserve Color" );
   OTS_PreserveColor_CheckBox.OnClick( (Button::click_event_handler)&AstroStretchStudioInterface::e_Click, w );

   OTS_PreserveColor_Sizer.AddUnscaledSpacing( labelWidth1 + 4 );
   OTS_PreserveColor_Sizer.Add( OTS_PreserveColor_CheckBox );
   OTS_PreserveColor_Sizer.AddStretch();

   OTS_Sizer.SetSpacing( 6 );
   OTS_Sizer.Add( OTS_ObjectType_Sizer );
   OTS_Sizer.Add( OTS_BackgroundTarget_NumericControl );
   OTS_Sizer.Add( OTS_StretchIntensity_NumericControl );
   OTS_Sizer.Add( OTS_ProtectHighlights_NumericControl );
   OTS_Sizer.Add( OTS_PreserveColor_Sizer );

   OTS_Control.SetSizer( OTS_Sizer );

   // -------------------------------------------------------------------------
   // SAS Panel - Starlet Arctan Stretch
   // -------------------------------------------------------------------------

   SAS_NumScales_NumericControl.label.SetText( "Number of Scales:" );
   SAS_NumScales_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_NumScales_NumericControl.slider.SetRange( 0, 7 );
   SAS_NumScales_NumericControl.SetInteger();
   SAS_NumScales_NumericControl.SetRange( 1, 8 );
   SAS_NumScales_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_BackgroundTarget_NumericControl.label.SetText( "Background Target:" );
   SAS_BackgroundTarget_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_BackgroundTarget_NumericControl.slider.SetRange( 0, 1000 );
   SAS_BackgroundTarget_NumericControl.SetReal();
   SAS_BackgroundTarget_NumericControl.SetRange( 0.0, 0.5 );
   SAS_BackgroundTarget_NumericControl.SetPrecision( 4 );
   SAS_BackgroundTarget_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_FineScaleGain_NumericControl.label.SetText( "Fine Scale Gain:" );
   SAS_FineScaleGain_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_FineScaleGain_NumericControl.slider.SetRange( 0, 500 );
   SAS_FineScaleGain_NumericControl.SetReal();
   SAS_FineScaleGain_NumericControl.SetRange( 0.0, 5.0 );
   SAS_FineScaleGain_NumericControl.SetPrecision( 2 );
   SAS_FineScaleGain_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_MidScaleGain_NumericControl.label.SetText( "Mid Scale Gain:" );
   SAS_MidScaleGain_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_MidScaleGain_NumericControl.slider.SetRange( 0, 500 );
   SAS_MidScaleGain_NumericControl.SetReal();
   SAS_MidScaleGain_NumericControl.SetRange( 0.0, 5.0 );
   SAS_MidScaleGain_NumericControl.SetPrecision( 2 );
   SAS_MidScaleGain_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_CoarseScaleGain_NumericControl.label.SetText( "Coarse Scale Gain:" );
   SAS_CoarseScaleGain_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_CoarseScaleGain_NumericControl.slider.SetRange( 0, 500 );
   SAS_CoarseScaleGain_NumericControl.SetReal();
   SAS_CoarseScaleGain_NumericControl.SetRange( 0.0, 5.0 );
   SAS_CoarseScaleGain_NumericControl.SetPrecision( 2 );
   SAS_CoarseScaleGain_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_CompressionAlpha_NumericControl.label.SetText( "Compression Alpha:" );
   SAS_CompressionAlpha_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_CompressionAlpha_NumericControl.slider.SetRange( 0, 500 );
   SAS_CompressionAlpha_NumericControl.SetReal();
   SAS_CompressionAlpha_NumericControl.SetRange( 0.5, 5.0 );
   SAS_CompressionAlpha_NumericControl.SetPrecision( 1 );
   SAS_CompressionAlpha_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_HighlightProtection_NumericControl.label.SetText( "Highlight Protection:" );
   SAS_HighlightProtection_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_HighlightProtection_NumericControl.slider.SetRange( 0, 1000 );
   SAS_HighlightProtection_NumericControl.SetReal();
   SAS_HighlightProtection_NumericControl.SetRange( 0.0, 1.0 );
   SAS_HighlightProtection_NumericControl.SetPrecision( 3 );
   SAS_HighlightProtection_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_NoiseThreshold_NumericControl.label.SetText( "Noise Threshold:" );
   SAS_NoiseThreshold_NumericControl.label.SetMinWidth( labelWidth1 );
   SAS_NoiseThreshold_NumericControl.slider.SetRange( 0, 1000 );
   SAS_NoiseThreshold_NumericControl.SetReal();
   SAS_NoiseThreshold_NumericControl.SetRange( 0.0, 0.01 );
   SAS_NoiseThreshold_NumericControl.SetPrecision( 5 );
   SAS_NoiseThreshold_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ValueUpdated, w );

   SAS_FlattenBackground_CheckBox.SetText( "Flatten Background" );
   SAS_FlattenBackground_CheckBox.OnClick( (Button::click_event_handler)&AstroStretchStudioInterface::e_Click, w );

   SAS_PreserveColor_CheckBox.SetText( "Preserve Color" );
   SAS_PreserveColor_CheckBox.OnClick( (Button::click_event_handler)&AstroStretchStudioInterface::e_Click, w );

   SAS_Checkboxes_Sizer.AddUnscaledSpacing( labelWidth1 + 4 );
   SAS_Checkboxes_Sizer.Add( SAS_FlattenBackground_CheckBox );
   SAS_Checkboxes_Sizer.AddSpacing( 12 );
   SAS_Checkboxes_Sizer.Add( SAS_PreserveColor_CheckBox );
   SAS_Checkboxes_Sizer.AddStretch();

   SAS_Sizer.SetSpacing( 6 );
   SAS_Sizer.Add( SAS_NumScales_NumericControl );
   SAS_Sizer.Add( SAS_BackgroundTarget_NumericControl );
   SAS_Sizer.Add( SAS_FineScaleGain_NumericControl );
   SAS_Sizer.Add( SAS_MidScaleGain_NumericControl );
   SAS_Sizer.Add( SAS_CoarseScaleGain_NumericControl );
   SAS_Sizer.Add( SAS_CompressionAlpha_NumericControl );
   SAS_Sizer.Add( SAS_HighlightProtection_NumericControl );
   SAS_Sizer.Add( SAS_NoiseThreshold_NumericControl );
   SAS_Sizer.Add( SAS_Checkboxes_Sizer );

   SAS_Control.SetSizer( SAS_Sizer );

   // -------------------------------------------------------------------------
   // Algorithm Parameters Container (stacked panels)
   // -------------------------------------------------------------------------

   AlgorithmParams_Sizer.Add( OTS_Control );
   AlgorithmParams_Sizer.Add( SAS_Control );

   AlgorithmParams_Control.SetSizer( AlgorithmParams_Sizer );

   // -------------------------------------------------------------------------
   // Zone HDR Section
   // -------------------------------------------------------------------------

   ZoneHDR_Enable_CheckBox.SetText( "Enable Zone HDR" );
   ZoneHDR_Enable_CheckBox.OnClick( (Button::click_event_handler)&AstroStretchStudioInterface::e_Click, w );

   ZoneHDR_DetectZones_Button.SetText( "Detect Zones" );
   ZoneHDR_DetectZones_Button.SetToolTip( "<p>Re-scan the current image to detect luminance zones.</p>" );
   ZoneHDR_DetectZones_Button.OnClick( (Button::click_event_handler)&AstroStretchStudioInterface::e_Click, w );

   ZoneHDR_Enable_Sizer.Add( ZoneHDR_Enable_CheckBox );
   ZoneHDR_Enable_Sizer.AddSpacing( 12 );
   ZoneHDR_Enable_Sizer.Add( ZoneHDR_DetectZones_Button );
   ZoneHDR_Enable_Sizer.AddStretch();

   ZoneHDR_ImageSelect_Label.SetText( "Image:" );
   ZoneHDR_ImageSelect_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   ZoneHDR_ImageSelect_ComboBox.SetToolTip( "<p>Select which image to analyze for zone detection.</p>" );
   ZoneHDR_ImageSelect_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&AstroStretchStudioInterface::e_ItemSelected, w );

   ZoneHDR_RefreshImages_Button.SetIcon( w.ScaledResource( ":/icons/refresh.png" ) );
   ZoneHDR_RefreshImages_Button.SetToolTip( "<p>Refresh the list of open images.</p>" );
   ZoneHDR_RefreshImages_Button.OnClick( (Button::click_event_handler)&AstroStretchStudioInterface::e_Click, w );

   ZoneHDR_ImageSelect_Sizer.Add( ZoneHDR_ImageSelect_Label );
   ZoneHDR_ImageSelect_Sizer.AddSpacing( 4 );
   ZoneHDR_ImageSelect_Sizer.Add( ZoneHDR_ImageSelect_ComboBox, 100 );
   ZoneHDR_ImageSelect_Sizer.AddSpacing( 4 );
   ZoneHDR_ImageSelect_Sizer.Add( ZoneHDR_RefreshImages_Button );
   ZoneHDR_ImageSelect_Sizer.AddStretch();

   ZoneHDR_PreviewMode_Label.SetText( "Preview:" );
   ZoneHDR_PreviewMode_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   ZoneHDR_PreviewMode_ComboBox.AddItem( "Off" );
   ZoneHDR_PreviewMode_ComboBox.AddItem( "Mask Overlay" );
   ZoneHDR_PreviewMode_ComboBox.AddItem( "Solo Zone" );
   ZoneHDR_PreviewMode_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&AstroStretchStudioInterface::e_ItemSelected, w );

   ZoneHDR_PreviewMode_Sizer.Add( ZoneHDR_PreviewMode_Label );
   ZoneHDR_PreviewMode_Sizer.AddSpacing( 4 );
   ZoneHDR_PreviewMode_Sizer.Add( ZoneHDR_PreviewMode_ComboBox );
   ZoneHDR_PreviewMode_Sizer.AddStretch();

   // Zone controls container (will be populated dynamically)
   // The zones will be added to the ScrollBox viewport dynamically
   ZoneHDR_Zones_ScrollBox.SetScaledFixedHeight( 200 );
   ZoneHDR_Zones_ScrollBox.Viewport().SetSizer( ZoneHDR_Zones_Sizer );

   // Initialize all zone controls (hidden by default)
   int zoneLabelWidth = w.Font().Width( String( "Saturation:" ) + 'M' );
   for ( int i = 0; i < MaxZones; ++i )
   {
      ZoneControls& zc = ZoneControls_Array[i];

      // Zone name label
      zc.Zone_Name_Label.SetText( String().Format( "Zone %d", i + 1 ) );
      zc.Zone_Name_Label.SetTextAlignment( TextAlign::Left | TextAlign::VertCenter );

      // Solo button (ToolButton is checkable by default)
      zc.Zone_Solo_Button.SetText( "Solo" );
      zc.Zone_Solo_Button.SetCheckable();
      zc.Zone_Solo_Button.OnClick( (Button::click_event_handler)&AstroStretchStudioInterface::e_ZoneSoloClick, w );

      zc.Zone_Header_Sizer.SetSpacing( 4 );
      zc.Zone_Header_Sizer.Add( zc.Zone_Name_Label );
      zc.Zone_Header_Sizer.AddStretch();
      zc.Zone_Header_Sizer.Add( zc.Zone_Solo_Button );

      // Intensity slider
      zc.Zone_Intensity_NumericControl.label.SetText( "Intensity:" );
      zc.Zone_Intensity_NumericControl.label.SetMinWidth( zoneLabelWidth );
      zc.Zone_Intensity_NumericControl.slider.SetRange( 0, 200 );
      zc.Zone_Intensity_NumericControl.SetReal();
      zc.Zone_Intensity_NumericControl.SetRange( -1.0, 1.0 );
      zc.Zone_Intensity_NumericControl.SetPrecision( 2 );
      zc.Zone_Intensity_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ZoneValueUpdated, w );

      // Saturation slider
      zc.Zone_Saturation_NumericControl.label.SetText( "Saturation:" );
      zc.Zone_Saturation_NumericControl.label.SetMinWidth( zoneLabelWidth );
      zc.Zone_Saturation_NumericControl.slider.SetRange( 0, 200 );
      zc.Zone_Saturation_NumericControl.SetReal();
      zc.Zone_Saturation_NumericControl.SetRange( -1.0, 1.0 );
      zc.Zone_Saturation_NumericControl.SetPrecision( 2 );
      zc.Zone_Saturation_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&AstroStretchStudioInterface::e_ZoneValueUpdated, w );

      zc.Zone_Sizer.SetSpacing( 4 );
      zc.Zone_Sizer.Add( zc.Zone_Header_Sizer );
      zc.Zone_Sizer.Add( zc.Zone_Intensity_NumericControl );
      zc.Zone_Sizer.Add( zc.Zone_Saturation_NumericControl );

      zc.Zone_Control.SetSizer( zc.Zone_Sizer );
      zc.Zone_Control.Hide();

      ZoneHDR_Zones_Sizer.Add( zc.Zone_Control );
   }

   ZoneHDR_Sizer.SetSpacing( 6 );
   ZoneHDR_Sizer.Add( ZoneHDR_Enable_Sizer );
   ZoneHDR_Sizer.Add( ZoneHDR_ImageSelect_Sizer );
   ZoneHDR_Sizer.Add( ZoneHDR_PreviewMode_Sizer );
   ZoneHDR_Sizer.Add( ZoneHDR_Zones_ScrollBox );

   ZoneHDR_Control.SetSizer( ZoneHDR_Sizer );

   ZoneHDR_SectionBar.SetTitle( "Zone HDR" );
   ZoneHDR_SectionBar.SetSection( ZoneHDR_Control );
   ZoneHDR_SectionBar.OnToggleSection( (SectionBar::section_event_handler)&AstroStretchStudioInterface::e_SectionToggle, w );

   // -------------------------------------------------------------------------
   // Real-time preview timer
   // -------------------------------------------------------------------------

   UpdateRealTimePreview_Timer.SetSingleShot();
   UpdateRealTimePreview_Timer.SetInterval( 0.025 ); // 25ms debounce
   UpdateRealTimePreview_Timer.OnTimer( (Timer::timer_event_handler)&AstroStretchStudioInterface::e_UpdateRealTimePreview_Timer, w );

   // -------------------------------------------------------------------------
   // Global Layout
   // -------------------------------------------------------------------------

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Algorithm_Sizer );
   Global_Sizer.Add( AlgorithmParams_Control );
   Global_Sizer.Add( ZoneHDR_SectionBar );
   Global_Sizer.Add( ZoneHDR_Control );

   w.SetSizer( Global_Sizer );

   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   w.SetMinSize();

   // Initially show OTS panel, hide SAS
   SAS_Control.Hide();
}

// ----------------------------------------------------------------------------

} // namespace pcl

// ----------------------------------------------------------------------------
