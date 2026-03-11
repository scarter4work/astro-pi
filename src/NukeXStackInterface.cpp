//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStackInterface.h"
#include "NukeXStackProcess.h"
#include "NukeXStackParameters.h"

#include <pcl/FileDialog.h>
#include <pcl/MessageBox.h>
#include <pcl/FileInfo.h>

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStackInterface* TheNukeXStackInterface = nullptr;

// ----------------------------------------------------------------------------

NukeXStackInterface::NukeXStackInterface()
   : m_instance( TheNukeXStackProcess )
{
   TheNukeXStackInterface = this;
}

// ----------------------------------------------------------------------------

NukeXStackInterface::~NukeXStackInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

// ----------------------------------------------------------------------------

IsoString NukeXStackInterface::Id() const
{
   return "NukeXStack";
}

// ----------------------------------------------------------------------------

MetaProcess* NukeXStackInterface::Process() const
{
   return TheNukeXStackProcess;
}

// ----------------------------------------------------------------------------

String NukeXStackInterface::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

InterfaceFeatures NukeXStackInterface::Features() const
{
   return InterfaceFeature::Default;
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::ApplyInstance() const
{
   m_instance.LaunchGlobal();
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::ResetInstance()
{
   NukeXStackInstance defaultInstance( TheNukeXStackProcess );
   ImportProcess( defaultInstance );
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::Launch( const MetaProcess& P, const ProcessImplementation* p, bool& dynamic, unsigned& /*flags*/ )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "NukeXStack v3 \x2014 Statistical Stacking" );
      UpdateControls();
   }

   if ( p != nullptr )
      ImportProcess( *p );
   else
      ResetInstance();

   dynamic = false;
   return &P == TheNukeXStackProcess;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStackInterface::NewProcess() const
{
   return new NukeXStackInstance( m_instance );
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const NukeXStackInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not a NukeXStack instance.";
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::RequiresInstanceValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::ImportProcess( const ProcessImplementation& p )
{
   m_instance.Assign( p );
   UpdateControls();
   return true;
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::UpdateControls()
{
   if ( GUI == nullptr )
      return;

   UpdateFileList();

   // Outlier rejection
   GUI->OutlierSigma_NumericControl.SetValue( m_instance.p_outlierSigmaThreshold );

   // Quality weighting
   GUI->EnableQualityWeighting_CheckBox.SetChecked( m_instance.p_enableQualityWeighting );
   GUI->QualityMode_ComboBox.SetCurrentItem( m_instance.p_qualityWeightMode );

   bool qwEnabled = m_instance.p_enableQualityWeighting;
   GUI->QualityMode_ComboBox.Enable( qwEnabled );
   GUI->FWHMWeight_NumericControl.Enable( qwEnabled );
   GUI->EccentricityWeight_NumericControl.Enable( qwEnabled );
   GUI->SkyBackgroundWeight_NumericControl.Enable( qwEnabled );
   GUI->HFRWeight_NumericControl.Enable( qwEnabled );
   GUI->AltitudeWeight_NumericControl.Enable( qwEnabled );

   GUI->FWHMWeight_NumericControl.SetValue( m_instance.p_fwhmWeight );
   GUI->EccentricityWeight_NumericControl.SetValue( m_instance.p_eccentricityWeight );
   GUI->SkyBackgroundWeight_NumericControl.SetValue( m_instance.p_skyBackgroundWeight );
   GUI->HFRWeight_NumericControl.SetValue( m_instance.p_hfrWeight );
   GUI->AltitudeWeight_NumericControl.SetValue( m_instance.p_altitudeWeight );

   // Output
   GUI->GenerateProvenance_CheckBox.SetChecked( m_instance.p_generateProvenance );
   GUI->GenerateDistMetadata_CheckBox.SetChecked( m_instance.p_generateDistMetadata );
   GUI->EnableAutoStretch_CheckBox.SetChecked( m_instance.p_enableAutoStretch );
   GUI->UseGPU_CheckBox.SetChecked( m_instance.p_useGPU );
   GUI->AdaptiveModels_CheckBox.SetChecked( m_instance.p_adaptiveModels );
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::UpdateFileList()
{
   if ( GUI == nullptr )
      return;

   GUI->InputFiles_TreeBox.DisableUpdates();
   GUI->InputFiles_TreeBox.Clear();

   for ( size_t i = 0; i < m_instance.p_inputFrames.size(); ++i )
   {
      const InputFrameData& frame = m_instance.p_inputFrames[i];

      TreeBox::Node* node = new TreeBox::Node( GUI->InputFiles_TreeBox );
      node->SetCheckable( true );
      node->Check( frame.enabled );
      node->SetText( 1, File::ExtractName( frame.path ) + File::ExtractExtension( frame.path ) );
      node->SetText( 2, frame.path );

      FileInfo info( frame.path );
      if ( info.Exists() )
      {
         double sizeMB = info.Size() / (1024.0 * 1024.0);
         node->SetText( 3, String().Format( "%.1f MB", sizeMB ) );
      }
      else
      {
         node->SetText( 3, "Not found" );
      }
   }

   GUI->InputFiles_TreeBox.EnableUpdates();
   UpdateFileCountLabel();
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::UpdateFileCountLabel()
{
   if ( GUI == nullptr )
      return;

   int total = static_cast<int>( m_instance.p_inputFrames.size() );
   int enabled = 0;
   for ( const auto& frame : m_instance.p_inputFrames )
      if ( frame.enabled )
         ++enabled;

   GUI->FileCount_Label.SetText( String().Format( "%d files, %d enabled", total, enabled ) );
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::AddFiles( const StringList& files )
{
   for ( const String& file : files )
   {
      bool exists = false;
      for ( const auto& frame : m_instance.p_inputFrames )
      {
         if ( frame.path == file )
         {
            exists = true;
            break;
         }
      }

      if ( !exists )
         m_instance.p_inputFrames.push_back( InputFrameData( file, true ) );
   }

   UpdateFileList();
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_TreeBoxNodeActivated( TreeBox& /*sender*/, TreeBox::Node& /*node*/, int /*col*/ )
{
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_TreeBoxNodeSelectionUpdated( TreeBox& /*sender*/ )
{
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_TreeBoxNodeUpdated( TreeBox& sender, TreeBox::Node& node, int col )
{
   if ( col == 0 )
   {
      int index = sender.ChildIndex( &node );
      if ( index >= 0 && static_cast<size_t>( index ) < m_instance.p_inputFrames.size() )
      {
         m_instance.p_inputFrames[index].enabled = node.IsChecked();
         UpdateFileCountLabel();
      }
   }
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_ButtonClick( Button& sender, bool /*checked*/ )
{
   if ( sender == GUI->AddFiles_PushButton )
   {
      OpenFileDialog dlg;
      dlg.SetCaption( "Select Input Frames" );
      dlg.SetFilter( FileFilter( "All supported formats", ".fit .fits .fts .xisf .tif .tiff" ) );
      dlg.EnableMultipleSelections();

      if ( dlg.Execute() )
         AddFiles( dlg.FileNames() );
   }
   else if ( sender == GUI->AddFolder_PushButton )
   {
      GetDirectoryDialog dlg;
      dlg.SetCaption( "Select Folder with Frames" );

      if ( dlg.Execute() )
      {
         StringList files;
         File::Find find( dlg.Directory() + "/*" );
         FindFileInfo info;
         while ( find.NextItem( info ) )
         {
            if ( !info.IsDirectory() )
            {
               String ext = File::ExtractExtension( info.name ).Lowercase();
               if ( ext == ".fit" || ext == ".fits" || ext == ".fts" ||
                    ext == ".xisf" || ext == ".tif" || ext == ".tiff" )
               {
                  files.Add( dlg.Directory() + "/" + info.name );
               }
            }
         }

         if ( !files.IsEmpty() )
            AddFiles( files );
      }
   }
   else if ( sender == GUI->Remove_PushButton )
   {
      IndirectArray<TreeBox::Node> selected = GUI->InputFiles_TreeBox.SelectedNodes();
      if ( !selected.IsEmpty() )
      {
         Array<int> indices;
         for ( const TreeBox::Node* node : selected )
            indices.Add( GUI->InputFiles_TreeBox.ChildIndex( node ) );

         indices.Sort();
         for ( int i = static_cast<int>( indices.Length() ) - 1; i >= 0; --i )
         {
            int idx = indices[i];
            if ( idx >= 0 && static_cast<size_t>( idx ) < m_instance.p_inputFrames.size() )
               m_instance.p_inputFrames.erase( m_instance.p_inputFrames.begin() + idx );
         }

         UpdateFileList();
      }
   }
   else if ( sender == GUI->Clear_PushButton )
   {
      if ( !m_instance.p_inputFrames.empty() )
      {
         if ( MessageBox( "Remove all input frames?", "NukeXStack",
                          StdIcon::Question, StdButton::Yes, StdButton::No ).Execute() == StdButton::Yes )
         {
            m_instance.p_inputFrames.clear();
            UpdateFileList();
         }
      }
   }
   else if ( sender == GUI->SelectAll_PushButton )
   {
      for ( auto& frame : m_instance.p_inputFrames )
         frame.enabled = true;
      UpdateFileList();
   }
   else if ( sender == GUI->InvertSelection_PushButton )
   {
      for ( auto& frame : m_instance.p_inputFrames )
         frame.enabled = !frame.enabled;
      UpdateFileList();
   }
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_ComboBoxItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->QualityMode_ComboBox )
      m_instance.p_qualityWeightMode = itemIndex;
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_CheckBoxClick( Button& sender, bool checked )
{
   if ( sender == GUI->EnableQualityWeighting_CheckBox )
   {
      m_instance.p_enableQualityWeighting = checked;
      GUI->QualityMode_ComboBox.Enable( checked );
      GUI->FWHMWeight_NumericControl.Enable( checked );
      GUI->EccentricityWeight_NumericControl.Enable( checked );
      GUI->SkyBackgroundWeight_NumericControl.Enable( checked );
      GUI->HFRWeight_NumericControl.Enable( checked );
      GUI->AltitudeWeight_NumericControl.Enable( checked );
   }
   else if ( sender == GUI->GenerateProvenance_CheckBox )
   {
      m_instance.p_generateProvenance = checked;
   }
   else if ( sender == GUI->GenerateDistMetadata_CheckBox )
   {
      m_instance.p_generateDistMetadata = checked;
   }
   else if ( sender == GUI->EnableAutoStretch_CheckBox )
   {
      m_instance.p_enableAutoStretch = checked;
   }
   else if ( sender == GUI->UseGPU_CheckBox )
   {
      m_instance.p_useGPU = checked;
   }
   else if ( sender == GUI->AdaptiveModels_CheckBox )
   {
      m_instance.p_adaptiveModels = checked;
   }
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_NumericValueUpdated( NumericEdit& sender, double value )
{
   if ( sender == GUI->OutlierSigma_NumericControl )
      m_instance.p_outlierSigmaThreshold = value;
   else if ( sender == GUI->FWHMWeight_NumericControl )
      m_instance.p_fwhmWeight = value;
   else if ( sender == GUI->EccentricityWeight_NumericControl )
      m_instance.p_eccentricityWeight = value;
   else if ( sender == GUI->SkyBackgroundWeight_NumericControl )
      m_instance.p_skyBackgroundWeight = value;
   else if ( sender == GUI->HFRWeight_NumericControl )
      m_instance.p_hfrWeight = value;
   else if ( sender == GUI->AltitudeWeight_NumericControl )
      m_instance.p_altitudeWeight = value;
}

// ----------------------------------------------------------------------------
// GUIData Implementation
// ----------------------------------------------------------------------------

NukeXStackInterface::GUIData::GUIData( NukeXStackInterface& w )
{
   int labelWidth1 = w.Font().Width( String( "Sky Background Weight:" ) + 'M' );
   int editWidth1 = w.Font().Width( String( '0', 10 ) );

   // =========================================================================
   // Input Files Section
   // =========================================================================

   InputFiles_SectionBar.SetTitle( "Input Frames" );
   InputFiles_SectionBar.SetSection( InputFiles_Control );

   InputFiles_TreeBox.SetMinHeight( 200 );
   InputFiles_TreeBox.SetNumberOfColumns( 4 );
   InputFiles_TreeBox.SetHeaderText( 0, "" );
   InputFiles_TreeBox.SetHeaderText( 1, "File" );
   InputFiles_TreeBox.SetHeaderText( 2, "Path" );
   InputFiles_TreeBox.SetHeaderText( 3, "Size" );
   InputFiles_TreeBox.SetColumnWidth( 0, 32 );
   InputFiles_TreeBox.SetColumnWidth( 1, 200 );
   InputFiles_TreeBox.SetColumnWidth( 2, 300 );
   InputFiles_TreeBox.SetColumnWidth( 3, 80 );
   InputFiles_TreeBox.EnableMultipleSelections();
   InputFiles_TreeBox.EnableHeaderSorting();
   InputFiles_TreeBox.SetToolTip( "<p>List of input frames for statistical stacking.</p>"
                                   "<p>Check the box next to each frame to include it.</p>" );
   InputFiles_TreeBox.OnNodeActivated( (TreeBox::node_event_handler)&NukeXStackInterface::e_TreeBoxNodeActivated, w );
   InputFiles_TreeBox.OnNodeSelectionUpdated( (TreeBox::tree_event_handler)&NukeXStackInterface::e_TreeBoxNodeSelectionUpdated, w );
   InputFiles_TreeBox.OnNodeUpdated( (TreeBox::node_event_handler)&NukeXStackInterface::e_TreeBoxNodeUpdated, w );

   AddFiles_PushButton.SetText( "Add Files" );
   AddFiles_PushButton.SetToolTip( "<p>Add individual image files.</p>" );
   AddFiles_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   AddFolder_PushButton.SetText( "Add Folder" );
   AddFolder_PushButton.SetToolTip( "<p>Add all compatible image files from a folder.</p>" );
   AddFolder_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   Remove_PushButton.SetText( "Remove" );
   Remove_PushButton.SetToolTip( "<p>Remove selected files from the list.</p>" );
   Remove_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   Clear_PushButton.SetText( "Clear" );
   Clear_PushButton.SetToolTip( "<p>Remove all files.</p>" );
   Clear_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   SelectAll_PushButton.SetText( "Select All" );
   SelectAll_PushButton.SetToolTip( "<p>Enable all files.</p>" );
   SelectAll_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   InvertSelection_PushButton.SetText( "Invert" );
   InvertSelection_PushButton.SetToolTip( "<p>Toggle enabled/disabled state.</p>" );
   InvertSelection_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   InputFiles_Buttons_HSizer.SetSpacing( 4 );
   InputFiles_Buttons_HSizer.Add( AddFiles_PushButton );
   InputFiles_Buttons_HSizer.Add( AddFolder_PushButton );
   InputFiles_Buttons_HSizer.Add( Remove_PushButton );
   InputFiles_Buttons_HSizer.Add( Clear_PushButton );
   InputFiles_Buttons_HSizer.AddSpacing( 16 );
   InputFiles_Buttons_HSizer.Add( SelectAll_PushButton );
   InputFiles_Buttons_HSizer.Add( InvertSelection_PushButton );
   InputFiles_Buttons_HSizer.AddStretch();

   FileCount_Label.SetText( "0 files, 0 enabled" );
   FileCount_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   InputFiles_Info_HSizer.AddStretch();
   InputFiles_Info_HSizer.Add( FileCount_Label );

   InputFiles_Sizer.SetSpacing( 4 );
   InputFiles_Sizer.Add( InputFiles_TreeBox, 100 );
   InputFiles_Sizer.Add( InputFiles_Buttons_HSizer );
   InputFiles_Sizer.Add( InputFiles_Info_HSizer );

   InputFiles_Control.SetSizer( InputFiles_Sizer );

   // =========================================================================
   // Outlier Rejection Section
   // =========================================================================

   Outliers_SectionBar.SetTitle( "Outlier Rejection" );
   Outliers_SectionBar.SetSection( Outliers_Control );

   OutlierSigma_NumericControl.label.SetText( "Sigma Threshold:" );
   OutlierSigma_NumericControl.label.SetMinWidth( labelWidth1 );
   OutlierSigma_NumericControl.slider.SetRange( 0, 100 );
   OutlierSigma_NumericControl.SetReal();
   OutlierSigma_NumericControl.SetRange( TheNXSOutlierSigmaThresholdParameter->MinimumValue(),
                                          TheNXSOutlierSigmaThresholdParameter->MaximumValue() );
   OutlierSigma_NumericControl.SetPrecision( TheNXSOutlierSigmaThresholdParameter->Precision() );
   OutlierSigma_NumericControl.edit.SetMinWidth( editWidth1 );
   OutlierSigma_NumericControl.SetToolTip( "<p>Reject pixel values beyond this many standard deviations "
                                            "from the fitted distribution. Lower values are more aggressive.</p>" );
   OutlierSigma_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   Outliers_Sizer.SetSpacing( 4 );
   Outliers_Sizer.Add( OutlierSigma_NumericControl );

   Outliers_Control.SetSizer( Outliers_Sizer );

   // =========================================================================
   // Quality Weighting Section
   // =========================================================================

   Quality_SectionBar.SetTitle( "Quality Weighting" );
   Quality_SectionBar.SetSection( Quality_Control );

   EnableQualityWeighting_CheckBox.SetText( "Enable Quality Weighting" );
   EnableQualityWeighting_CheckBox.SetToolTip( "<p>Compute per-frame quality weights from FITS metadata. "
                                                "Better frames contribute more to the final integration.</p>" );
   EnableQualityWeighting_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   QualityMode_Label.SetText( "Mode:" );
   QualityMode_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   QualityMode_Label.SetMinWidth( labelWidth1 );

   QualityMode_ComboBox.AddItem( "None" );
   QualityMode_ComboBox.AddItem( "FWHM Only" );
   QualityMode_ComboBox.AddItem( "Full (All Attributes)" );
   QualityMode_ComboBox.SetToolTip( "<p><b>None</b>: Equal weights for all frames.</p>"
                                     "<p><b>FWHM Only</b>: Weight by seeing (FWHM) only.</p>"
                                     "<p><b>Full</b>: Geometric mean of FWHM, eccentricity, sky background, "
                                     "HFR, and altitude weights.</p>" );
   QualityMode_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXStackInterface::e_ComboBoxItemSelected, w );

   QualityMode_HSizer.SetSpacing( 4 );
   QualityMode_HSizer.Add( QualityMode_Label );
   QualityMode_HSizer.Add( QualityMode_ComboBox, 100 );

   FWHMWeight_NumericControl.label.SetText( "FWHM Weight:" );
   FWHMWeight_NumericControl.label.SetMinWidth( labelWidth1 );
   FWHMWeight_NumericControl.slider.SetRange( 0, 100 );
   FWHMWeight_NumericControl.SetReal();
   FWHMWeight_NumericControl.SetRange( TheNXSFWHMWeightParameter->MinimumValue(),
                                        TheNXSFWHMWeightParameter->MaximumValue() );
   FWHMWeight_NumericControl.SetPrecision( TheNXSFWHMWeightParameter->Precision() );
   FWHMWeight_NumericControl.edit.SetMinWidth( editWidth1 );
   FWHMWeight_NumericControl.SetToolTip( "<p>Relative importance of FWHM (seeing) in quality weighting.</p>" );
   FWHMWeight_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   EccentricityWeight_NumericControl.label.SetText( "Eccentricity Weight:" );
   EccentricityWeight_NumericControl.label.SetMinWidth( labelWidth1 );
   EccentricityWeight_NumericControl.slider.SetRange( 0, 100 );
   EccentricityWeight_NumericControl.SetReal();
   EccentricityWeight_NumericControl.SetRange( TheNXSEccentricityWeightParameter->MinimumValue(),
                                                TheNXSEccentricityWeightParameter->MaximumValue() );
   EccentricityWeight_NumericControl.SetPrecision( TheNXSEccentricityWeightParameter->Precision() );
   EccentricityWeight_NumericControl.edit.SetMinWidth( editWidth1 );
   EccentricityWeight_NumericControl.SetToolTip( "<p>Relative importance of star eccentricity (roundness).</p>" );
   EccentricityWeight_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   SkyBackgroundWeight_NumericControl.label.SetText( "Sky Background Weight:" );
   SkyBackgroundWeight_NumericControl.label.SetMinWidth( labelWidth1 );
   SkyBackgroundWeight_NumericControl.slider.SetRange( 0, 100 );
   SkyBackgroundWeight_NumericControl.SetReal();
   SkyBackgroundWeight_NumericControl.SetRange( TheNXSSkyBackgroundWeightParameter->MinimumValue(),
                                                 TheNXSSkyBackgroundWeightParameter->MaximumValue() );
   SkyBackgroundWeight_NumericControl.SetPrecision( TheNXSSkyBackgroundWeightParameter->Precision() );
   SkyBackgroundWeight_NumericControl.edit.SetMinWidth( editWidth1 );
   SkyBackgroundWeight_NumericControl.SetToolTip( "<p>Relative importance of sky background level (lower is better).</p>" );
   SkyBackgroundWeight_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   HFRWeight_NumericControl.label.SetText( "HFR Weight:" );
   HFRWeight_NumericControl.label.SetMinWidth( labelWidth1 );
   HFRWeight_NumericControl.slider.SetRange( 0, 100 );
   HFRWeight_NumericControl.SetReal();
   HFRWeight_NumericControl.SetRange( TheNXSHFRWeightParameter->MinimumValue(),
                                       TheNXSHFRWeightParameter->MaximumValue() );
   HFRWeight_NumericControl.SetPrecision( TheNXSHFRWeightParameter->Precision() );
   HFRWeight_NumericControl.edit.SetMinWidth( editWidth1 );
   HFRWeight_NumericControl.SetToolTip( "<p>Relative importance of half-flux radius.</p>" );
   HFRWeight_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   AltitudeWeight_NumericControl.label.SetText( "Altitude Weight:" );
   AltitudeWeight_NumericControl.label.SetMinWidth( labelWidth1 );
   AltitudeWeight_NumericControl.slider.SetRange( 0, 100 );
   AltitudeWeight_NumericControl.SetReal();
   AltitudeWeight_NumericControl.SetRange( TheNXSAltitudeWeightParameter->MinimumValue(),
                                            TheNXSAltitudeWeightParameter->MaximumValue() );
   AltitudeWeight_NumericControl.SetPrecision( TheNXSAltitudeWeightParameter->Precision() );
   AltitudeWeight_NumericControl.edit.SetMinWidth( editWidth1 );
   AltitudeWeight_NumericControl.SetToolTip( "<p>Relative importance of object altitude (higher is better seeing).</p>" );
   AltitudeWeight_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   Quality_Sizer.SetSpacing( 4 );
   Quality_Sizer.Add( EnableQualityWeighting_CheckBox );
   Quality_Sizer.Add( QualityMode_HSizer );
   Quality_Sizer.Add( FWHMWeight_NumericControl );
   Quality_Sizer.Add( EccentricityWeight_NumericControl );
   Quality_Sizer.Add( SkyBackgroundWeight_NumericControl );
   Quality_Sizer.Add( HFRWeight_NumericControl );
   Quality_Sizer.Add( AltitudeWeight_NumericControl );

   Quality_Control.SetSizer( Quality_Sizer );

   // =========================================================================
   // Output Section
   // =========================================================================

   Output_SectionBar.SetTitle( "Output" );
   Output_SectionBar.SetSection( Output_Control );

   GenerateProvenance_CheckBox.SetText( "Generate Provenance Data" );
   GenerateProvenance_CheckBox.SetToolTip( "<p>Record which source frame contributed each pixel in the output. "
                                            "Stored as FITS keywords in the output image.</p>" );
   GenerateProvenance_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   GenerateDistMetadata_CheckBox.SetText( "Generate Distribution Metadata" );
   GenerateDistMetadata_CheckBox.SetToolTip( "<p>Record the best-fit distribution type and parameters for each pixel. "
                                              "Useful for diagnostic analysis. Increases output file size.</p>" );
   GenerateDistMetadata_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   EnableAutoStretch_CheckBox.SetText( "Auto-Stretch Output" );
   EnableAutoStretch_CheckBox.SetToolTip( "<p>Automatically select and apply the best stretch "
                                          "algorithm based on per-pixel distribution statistics. "
                                          "Creates a second output window (NukeX_stretched).</p>" );
   EnableAutoStretch_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   UseGPU_CheckBox.SetText( "Use GPU Acceleration" );
   UseGPU_CheckBox.SetToolTip( "<p>Use NVIDIA CUDA GPU for pixel selection when available. "
                                "Falls back to CPU if no compatible GPU is detected.</p>" );
   UseGPU_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );
#ifndef NUKEX_HAS_CUDA
   UseGPU_CheckBox.Disable();
   UseGPU_CheckBox.SetToolTip( "<p>GPU acceleration unavailable \xe2\x80\x94 module built without CUDA support.</p>" );
#endif

   AdaptiveModels_CheckBox.SetText( "Adaptive Model Selection" );
   AdaptiveModels_CheckBox.SetToolTip( "<p>Skip expensive distribution fits (Skew-Normal, Bimodal) when "
                                        "Gaussian provides an excellent fit. Significantly faster with "
                                        "negligible impact on quality for typical data.</p>" );
   AdaptiveModels_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   Output_Sizer.SetSpacing( 4 );
   Output_Sizer.Add( GenerateProvenance_CheckBox );
   Output_Sizer.Add( GenerateDistMetadata_CheckBox );
   Output_Sizer.Add( EnableAutoStretch_CheckBox );
   Output_Sizer.Add( UseGPU_CheckBox );
   Output_Sizer.Add( AdaptiveModels_CheckBox );

   Output_Control.SetSizer( Output_Sizer );

   // =========================================================================
   // Global Layout
   // =========================================================================

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( InputFiles_SectionBar );
   Global_Sizer.Add( InputFiles_Control, 100 );
   Global_Sizer.Add( Outliers_SectionBar );
   Global_Sizer.Add( Outliers_Control );
   Global_Sizer.Add( Quality_SectionBar );
   Global_Sizer.Add( Quality_Control );
   Global_Sizer.Add( Output_SectionBar );
   Global_Sizer.Add( Output_Control );

   w.SetSizer( Global_Sizer );

   w.EnsureLayoutUpdated();
   w.AdjustToContents();

   int minWidth = w.Font().Width( String( 'M', 80 ) );
   w.SetMinSize( minWidth, 600 );
}

// ----------------------------------------------------------------------------

} // namespace pcl
