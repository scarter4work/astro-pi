//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
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

bool NukeXStackInterface::Launch( const MetaProcess& P, const ProcessImplementation*, bool& dynamic, unsigned& /*flags*/ )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "NukeXStack - Intelligent Pixel Selection" );
      UpdateControls();
   }

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

   // Update file list
   UpdateFileList();

   // Selection strategy
   GUI->Strategy_ComboBox.SetCurrentItem( m_instance.p_selectionStrategy );
   UpdateStrategyDescription();

   // ML Segmentation
   GUI->EnableMLSegmentation_CheckBox.SetChecked( m_instance.p_enableMLSegmentation );
   GUI->MinConfidence_NumericControl.SetValue( m_instance.p_minClassConfidence );
   GUI->UseSpatialContext_CheckBox.SetChecked( m_instance.p_useSpatialContext );
   GUI->UseTargetContext_CheckBox.SetChecked( m_instance.p_useTargetContext );

   // Enable/disable dependent controls
   bool mlEnabled = m_instance.p_enableMLSegmentation;
   GUI->MinConfidence_NumericControl.Enable( mlEnabled );
   GUI->UseSpatialContext_CheckBox.Enable( mlEnabled );
   GUI->UseTargetContext_CheckBox.Enable( mlEnabled );

   // Transition Smoothing
   GUI->EnableSmoothing_CheckBox.SetChecked( m_instance.p_enableTransitionSmoothing );
   GUI->SmoothingStrength_NumericControl.SetValue( m_instance.p_smoothingStrength );
   GUI->TransitionThreshold_NumericControl.SetValue( m_instance.p_transitionThreshold );
   GUI->TileSize_SpinBox.SetValue( m_instance.p_tileSize );
   GUI->SmoothingRadius_SpinBox.SetValue( m_instance.p_smoothingRadius );

   // Enable/disable smoothing controls
   bool smoothEnabled = m_instance.p_enableTransitionSmoothing;
   GUI->SmoothingStrength_NumericControl.Enable( smoothEnabled );
   GUI->TransitionThreshold_NumericControl.Enable( smoothEnabled );
   GUI->TileSize_SpinBox.Enable( smoothEnabled );
   GUI->SmoothingRadius_SpinBox.Enable( smoothEnabled );

   // Outlier controls
   GUI->OutlierSigma_NumericControl.SetValue( m_instance.p_outlierSigmaThreshold );
   GUI->GenerateMetadata_CheckBox.SetChecked( m_instance.p_generateMetadata );
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

      // Column 0: Enabled checkbox
      node->SetCheckable( true );
      node->Check( frame.enabled );

      // Column 1: File name
      node->SetText( 1, File::ExtractName( frame.path ) + File::ExtractExtension( frame.path ) );

      // Column 2: Full path (shown on hover or in details)
      node->SetText( 2, frame.path );

      // Column 3: File info (size)
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

void NukeXStackInterface::UpdateStrategyDescription()
{
   if ( GUI == nullptr )
      return;

   String desc;
   switch ( m_instance.p_selectionStrategy )
   {
   case NXSSelectionStrategy::Distribution:
      desc = "Uses statistical distribution fitting to select optimal pixel values.";
      break;
   case NXSSelectionStrategy::WeightedMedian:
      desc = "Selects values weighted by confidence, centered on median.";
      break;
   case NXSSelectionStrategy::MLGuided:
      desc = "Uses ML segmentation class to guide pixel selection strategy.";
      break;
   case NXSSelectionStrategy::Hybrid:
   default:
      desc = "Combines distribution fitting with ML-guided selection (recommended).";
      break;
   }

   GUI->StrategyDescription_Label.SetText( desc );
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::AddFiles( const StringList& files )
{
   for ( const String& file : files )
   {
      // Check if already in list
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
         m_instance.AddInputFrame( file, true );
   }

   UpdateFileList();
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_TreeBoxNodeActivated( TreeBox& /*sender*/, TreeBox::Node& /*node*/, int /*col*/ )
{
   // Double-click - could open file info
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_TreeBoxNodeSelectionUpdated( TreeBox& /*sender*/ )
{
   // Selection changed - update button states if needed
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_TreeBoxNodeUpdated( TreeBox& sender, TreeBox::Node& node, int col )
{
   if ( col == 0 ) // Checkbox column
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
         // Build list of indices to remove (in reverse order)
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
            m_instance.ClearInputFrames();
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
   if ( sender == GUI->Strategy_ComboBox )
   {
      m_instance.p_selectionStrategy = itemIndex;
      UpdateStrategyDescription();
   }
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_CheckBoxClick( Button& sender, bool checked )
{
   if ( sender == GUI->EnableMLSegmentation_CheckBox )
   {
      m_instance.p_enableMLSegmentation = checked;
      GUI->MinConfidence_NumericControl.Enable( checked );
      GUI->UseSpatialContext_CheckBox.Enable( checked );
      GUI->UseTargetContext_CheckBox.Enable( checked );
   }
   else if ( sender == GUI->UseSpatialContext_CheckBox )
   {
      m_instance.p_useSpatialContext = checked;
   }
   else if ( sender == GUI->UseTargetContext_CheckBox )
   {
      m_instance.p_useTargetContext = checked;
   }
   else if ( sender == GUI->EnableSmoothing_CheckBox )
   {
      m_instance.p_enableTransitionSmoothing = checked;
      GUI->SmoothingStrength_NumericControl.Enable( checked );
      GUI->TransitionThreshold_NumericControl.Enable( checked );
      GUI->TileSize_SpinBox.Enable( checked );
      GUI->SmoothingRadius_SpinBox.Enable( checked );
   }
   else if ( sender == GUI->GenerateMetadata_CheckBox )
   {
      m_instance.p_generateMetadata = checked;
   }
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_NumericValueUpdated( NumericEdit& sender, double value )
{
   if ( sender == GUI->MinConfidence_NumericControl )
      m_instance.p_minClassConfidence = value;
   else if ( sender == GUI->SmoothingStrength_NumericControl )
      m_instance.p_smoothingStrength = value;
   else if ( sender == GUI->TransitionThreshold_NumericControl )
      m_instance.p_transitionThreshold = value;
   else if ( sender == GUI->OutlierSigma_NumericControl )
      m_instance.p_outlierSigmaThreshold = value;
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_SpinBoxValueUpdated( SpinBox& sender, int value )
{
   if ( sender == GUI->TileSize_SpinBox )
      m_instance.p_tileSize = value;
   else if ( sender == GUI->SmoothingRadius_SpinBox )
      m_instance.p_smoothingRadius = value;
}

// ----------------------------------------------------------------------------
// GUIData Implementation
// ----------------------------------------------------------------------------

NukeXStackInterface::GUIData::GUIData( NukeXStackInterface& w )
{
   int labelWidth1 = w.Font().Width( String( "Transition Threshold:" ) + 'M' );
   int editWidth1 = w.Font().Width( String( '0', 10 ) );

   // =========================================================================
   // Input Files Section
   // =========================================================================

   InputFiles_SectionBar.SetTitle( "Input Frames" );
   InputFiles_SectionBar.SetSection( InputFiles_Control );

   InputFiles_TreeBox.SetMinHeight( 200 );
   InputFiles_TreeBox.SetNumberOfColumns( 4 );
   InputFiles_TreeBox.SetHeaderText( 0, "" );        // Checkbox
   InputFiles_TreeBox.SetHeaderText( 1, "File" );
   InputFiles_TreeBox.SetHeaderText( 2, "Path" );
   InputFiles_TreeBox.SetHeaderText( 3, "Size" );
   InputFiles_TreeBox.SetColumnWidth( 0, 32 );
   InputFiles_TreeBox.SetColumnWidth( 1, 200 );
   InputFiles_TreeBox.SetColumnWidth( 2, 300 );
   InputFiles_TreeBox.SetColumnWidth( 3, 80 );
   InputFiles_TreeBox.EnableMultipleSelections();
   InputFiles_TreeBox.EnableHeaderSorting();
   InputFiles_TreeBox.SetToolTip( "<p>List of prestretched input frames for integration.</p>"
                                   "<p>Check the box next to each frame to include it in processing. "
                                   "Drag and drop files here to add them.</p>" );
   InputFiles_TreeBox.OnNodeActivated( (TreeBox::node_event_handler)&NukeXStackInterface::e_TreeBoxNodeActivated, w );
   InputFiles_TreeBox.OnNodeSelectionUpdated( (TreeBox::tree_event_handler)&NukeXStackInterface::e_TreeBoxNodeSelectionUpdated, w );
   InputFiles_TreeBox.OnNodeUpdated( (TreeBox::node_event_handler)&NukeXStackInterface::e_TreeBoxNodeUpdated, w );

   AddFiles_PushButton.SetText( "Add Files" );
   AddFiles_PushButton.SetToolTip( "<p>Add individual image files to the input list.</p>" );
   AddFiles_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   AddFolder_PushButton.SetText( "Add Folder" );
   AddFolder_PushButton.SetToolTip( "<p>Add all compatible image files from a folder.</p>" );
   AddFolder_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   Remove_PushButton.SetText( "Remove" );
   Remove_PushButton.SetToolTip( "<p>Remove selected files from the input list.</p>" );
   Remove_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   Clear_PushButton.SetText( "Clear" );
   Clear_PushButton.SetToolTip( "<p>Remove all files from the input list.</p>" );
   Clear_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   SelectAll_PushButton.SetText( "Select All" );
   SelectAll_PushButton.SetToolTip( "<p>Enable all files for processing.</p>" );
   SelectAll_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   InvertSelection_PushButton.SetText( "Invert" );
   InvertSelection_PushButton.SetToolTip( "<p>Invert enabled/disabled state of all files.</p>" );
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
   // Selection Strategy Section
   // =========================================================================

   Strategy_SectionBar.SetTitle( "Selection Strategy" );
   Strategy_SectionBar.SetSection( Strategy_Control );

   Strategy_Label.SetText( "Strategy:" );
   Strategy_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   Strategy_Label.SetMinWidth( labelWidth1 );

   Strategy_ComboBox.AddItem( "Distribution Fitting" );
   Strategy_ComboBox.AddItem( "Weighted Median" );
   Strategy_ComboBox.AddItem( "ML Guided" );
   Strategy_ComboBox.AddItem( "Hybrid (Recommended)" );
   Strategy_ComboBox.SetToolTip( "<p>Select the pixel selection strategy:</p>"
                                  "<p><b>Distribution</b>: Uses statistical distribution fitting.</p>"
                                  "<p><b>Weighted Median</b>: Confidence-weighted median selection.</p>"
                                  "<p><b>ML Guided</b>: Uses segmentation class to guide selection.</p>"
                                  "<p><b>Hybrid</b>: Combines distribution fitting with ML guidance.</p>" );
   Strategy_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXStackInterface::e_ComboBoxItemSelected, w );

   Strategy_HSizer.SetSpacing( 4 );
   Strategy_HSizer.Add( Strategy_Label );
   Strategy_HSizer.Add( Strategy_ComboBox, 100 );

   StrategyDescription_Label.SetTextColor( 0xFF808080 );  // Gray
   StrategyDescription_Label.EnableWordWrapping();

   Strategy_Sizer.SetSpacing( 4 );
   Strategy_Sizer.Add( Strategy_HSizer );
   Strategy_Sizer.Add( StrategyDescription_Label );

   Strategy_Control.SetSizer( Strategy_Sizer );

   // =========================================================================
   // ML Segmentation Section
   // =========================================================================

   MLSegmentation_SectionBar.SetTitle( "ML Segmentation" );
   MLSegmentation_SectionBar.SetSection( MLSegmentation_Control );

   EnableMLSegmentation_CheckBox.SetText( "Enable ML Segmentation" );
   EnableMLSegmentation_CheckBox.SetToolTip( "<p>Use 23-class ML semantic segmentation to identify pixel types "
                                              "(stars, nebula, background, etc.) and apply class-specific "
                                              "selection strategies.</p>" );
   EnableMLSegmentation_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   MinConfidence_NumericControl.label.SetText( "Min Confidence:" );
   MinConfidence_NumericControl.label.SetMinWidth( labelWidth1 );
   MinConfidence_NumericControl.slider.SetRange( 0, 100 );
   MinConfidence_NumericControl.SetReal();
   MinConfidence_NumericControl.SetRange( TheNXSMinClassConfidenceParameter->MinimumValue(),
                                           TheNXSMinClassConfidenceParameter->MaximumValue() );
   MinConfidence_NumericControl.SetPrecision( TheNXSMinClassConfidenceParameter->Precision() );
   MinConfidence_NumericControl.edit.SetMinWidth( editWidth1 );
   MinConfidence_NumericControl.SetToolTip( "<p>Minimum confidence threshold for using ML class information. "
                                             "Below this threshold, fallback to median selection.</p>" );
   MinConfidence_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   UseSpatialContext_CheckBox.SetText( "Spatial Context" );
   UseSpatialContext_CheckBox.SetToolTip( "<p>Consider neighboring pixels' classes when selecting values. "
                                           "Helps smooth transitions between regions.</p>" );
   UseSpatialContext_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   UseTargetContext_CheckBox.SetText( "Use FITS Headers" );
   UseTargetContext_CheckBox.SetToolTip( "<p>Read OBJECT keyword from FITS headers to infer expected features. "
                                          "For example, M42 will expect emission nebula regions.</p>" );
   UseTargetContext_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   Context_HSizer.SetSpacing( 16 );
   Context_HSizer.Add( UseSpatialContext_CheckBox );
   Context_HSizer.Add( UseTargetContext_CheckBox );
   Context_HSizer.AddStretch();

   MLSegmentation_Sizer.SetSpacing( 4 );
   MLSegmentation_Sizer.Add( EnableMLSegmentation_CheckBox );
   MLSegmentation_Sizer.Add( MinConfidence_NumericControl );
   MLSegmentation_Sizer.Add( Context_HSizer );

   MLSegmentation_Control.SetSizer( MLSegmentation_Sizer );

   // =========================================================================
   // Transition Smoothing Section
   // =========================================================================

   Smoothing_SectionBar.SetTitle( "Transition Smoothing" );
   Smoothing_SectionBar.SetSection( Smoothing_Control );

   EnableSmoothing_CheckBox.SetText( "Enable Transition Smoothing" );
   EnableSmoothing_CheckBox.SetToolTip( "<p>Post-integration check for hard transitions between regions. "
                                         "Applies localized smoothing to artifacts while preserving real features.</p>" );
   EnableSmoothing_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   SmoothingStrength_NumericControl.label.SetText( "Smoothing Strength:" );
   SmoothingStrength_NumericControl.label.SetMinWidth( labelWidth1 );
   SmoothingStrength_NumericControl.slider.SetRange( 0, 100 );
   SmoothingStrength_NumericControl.SetReal();
   SmoothingStrength_NumericControl.SetRange( TheNXSSmoothingStrengthParameter->MinimumValue(),
                                               TheNXSSmoothingStrengthParameter->MaximumValue() );
   SmoothingStrength_NumericControl.SetPrecision( TheNXSSmoothingStrengthParameter->Precision() );
   SmoothingStrength_NumericControl.edit.SetMinWidth( editWidth1 );
   SmoothingStrength_NumericControl.SetToolTip( "<p>Maximum blending weight for transition smoothing.</p>" );
   SmoothingStrength_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   TransitionThreshold_NumericControl.label.SetText( "Transition Threshold:" );
   TransitionThreshold_NumericControl.label.SetMinWidth( labelWidth1 );
   TransitionThreshold_NumericControl.slider.SetRange( 0, 100 );
   TransitionThreshold_NumericControl.SetReal();
   TransitionThreshold_NumericControl.SetRange( TheNXSTransitionThresholdParameter->MinimumValue(),
                                                 TheNXSTransitionThresholdParameter->MaximumValue() );
   TransitionThreshold_NumericControl.SetPrecision( TheNXSTransitionThresholdParameter->Precision() );
   TransitionThreshold_NumericControl.edit.SetMinWidth( editWidth1 );
   TransitionThreshold_NumericControl.SetToolTip( "<p>Gradient threshold for detecting hard transitions. "
                                                   "Lower values detect more subtle transitions.</p>" );
   TransitionThreshold_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   TileSize_Label.SetText( "Tile Size:" );
   TileSize_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   TileSize_SpinBox.SetRange( int( TheNXSTileSizeParameter->MinimumValue() ),
                               int( TheNXSTileSizeParameter->MaximumValue() ) );
   TileSize_SpinBox.SetToolTip( "<p>Size of tiles for transition detection (pixels).</p>" );
   TileSize_SpinBox.OnValueUpdated( (SpinBox::value_event_handler)&NukeXStackInterface::e_SpinBoxValueUpdated, w );

   SmoothingRadius_Label.SetText( "Radius:" );
   SmoothingRadius_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   SmoothingRadius_SpinBox.SetRange( int( TheNXSSmoothingRadiusParameter->MinimumValue() ),
                                      int( TheNXSSmoothingRadiusParameter->MaximumValue() ) );
   SmoothingRadius_SpinBox.SetToolTip( "<p>Gaussian smoothing radius at boundaries (pixels).</p>" );
   SmoothingRadius_SpinBox.OnValueUpdated( (SpinBox::value_event_handler)&NukeXStackInterface::e_SpinBoxValueUpdated, w );

   SmoothingParams_HSizer.SetSpacing( 8 );
   SmoothingParams_HSizer.AddUnscaledSpacing( labelWidth1 + 4 );
   SmoothingParams_HSizer.Add( TileSize_Label );
   SmoothingParams_HSizer.Add( TileSize_SpinBox );
   SmoothingParams_HSizer.AddSpacing( 16 );
   SmoothingParams_HSizer.Add( SmoothingRadius_Label );
   SmoothingParams_HSizer.Add( SmoothingRadius_SpinBox );
   SmoothingParams_HSizer.AddStretch();

   Smoothing_Sizer.SetSpacing( 4 );
   Smoothing_Sizer.Add( EnableSmoothing_CheckBox );
   Smoothing_Sizer.Add( SmoothingStrength_NumericControl );
   Smoothing_Sizer.Add( TransitionThreshold_NumericControl );
   Smoothing_Sizer.Add( SmoothingParams_HSizer );

   Smoothing_Control.SetSizer( Smoothing_Sizer );

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
   OutlierSigma_NumericControl.SetToolTip( "<p>Reject pixel values beyond this many standard deviations from the mean. "
                                            "Lower values reject more aggressively.</p>" );
   OutlierSigma_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   GenerateMetadata_CheckBox.SetText( "Generate Per-Pixel Metadata" );
   GenerateMetadata_CheckBox.SetToolTip( "<p>Store detailed metadata for each pixel including source frame, "
                                          "confidence, and distribution info. Increases memory usage but useful "
                                          "for analysis.</p>" );
   GenerateMetadata_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   Outliers_Sizer.SetSpacing( 4 );
   Outliers_Sizer.Add( OutlierSigma_NumericControl );
   Outliers_Sizer.Add( GenerateMetadata_CheckBox );

   Outliers_Control.SetSizer( Outliers_Sizer );

   // =========================================================================
   // Global Layout
   // =========================================================================

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( InputFiles_SectionBar );
   Global_Sizer.Add( InputFiles_Control, 100 );
   Global_Sizer.Add( Strategy_SectionBar );
   Global_Sizer.Add( Strategy_Control );
   Global_Sizer.Add( MLSegmentation_SectionBar );
   Global_Sizer.Add( MLSegmentation_Control );
   Global_Sizer.Add( Smoothing_SectionBar );
   Global_Sizer.Add( Smoothing_Control );
   Global_Sizer.Add( Outliers_SectionBar );
   Global_Sizer.Add( Outliers_Control );

   w.SetSizer( Global_Sizer );

   w.EnsureLayoutUpdated();
   w.AdjustToContents();

   // Set a reasonable minimum size
   int minWidth = w.Font().Width( String( 'M', 80 ) );
   int minHeight = 600;
   w.SetMinSize( minWidth, minHeight );
}

// ----------------------------------------------------------------------------

} // namespace pcl
