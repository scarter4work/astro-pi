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
   UpdateFlatList();

   // Outlier rejection
   GUI->OutlierSigma_NumericControl.SetValue( m_instance.p_outlierSigmaThreshold );

   // Metadata tiebreaker
   GUI->EnableMetadataTiebreaker_CheckBox.SetChecked( m_instance.p_enableMetadataTiebreaker );

   // Output
   GUI->GenerateProvenance_CheckBox.SetChecked( m_instance.p_generateProvenance );
   GUI->GenerateDistMetadata_CheckBox.SetChecked( m_instance.p_generateDistMetadata );
   GUI->EnableAutoStretch_CheckBox.SetChecked( m_instance.p_enableAutoStretch );
   GUI->UseGPU_CheckBox.SetChecked( m_instance.p_useGPU );
   GUI->AdaptiveModels_CheckBox.SetChecked( m_instance.p_adaptiveModels );

   // Output — Bortle
   GUI->BortleNumber_NumericControl.SetValue( m_instance.p_bortleNumber );

   // Remediation
   GUI->EnableRemediation_CheckBox.SetChecked( m_instance.p_enableRemediation );
   GUI->EnableDustRemediation_CheckBox.SetChecked( m_instance.p_enableDustRemediation );
   GUI->EnableVignettingRemediation_CheckBox.SetChecked( m_instance.p_enableVignettingRemediation );

   bool remEnabled = m_instance.p_enableRemediation;
   GUI->EnableDustRemediation_CheckBox.Enable( remEnabled );
   GUI->EnableVignettingRemediation_CheckBox.Enable( remEnabled );
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

void NukeXStackInterface::UpdateFlatList()
{
   if ( GUI == nullptr )
      return;

   GUI->FlatFiles_TreeBox.DisableUpdates();
   GUI->FlatFiles_TreeBox.Clear();

   for ( size_t i = 0; i < m_instance.p_flatFrames.size(); ++i )
   {
      const String& path = m_instance.p_flatFrames[i];

      TreeBox::Node* node = new TreeBox::Node( GUI->FlatFiles_TreeBox );
      node->SetText( 0, File::ExtractName( path ) + File::ExtractExtension( path ) );
      node->SetText( 1, path );

      FileInfo info( path );
      if ( info.Exists() )
      {
         double sizeMB = info.Size() / (1024.0 * 1024.0);
         node->SetText( 2, String().Format( "%.1f MB", sizeMB ) );
      }
      else
      {
         node->SetText( 2, "Not found" );
      }
   }

   GUI->FlatFiles_TreeBox.EnableUpdates();
   UpdateFlatCountLabel();
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::UpdateFlatCountLabel()
{
   if ( GUI == nullptr )
      return;

   int total = static_cast<int>( m_instance.p_flatFrames.size() );
   GUI->FlatCount_Label.SetText( String().Format( "%d flat frames", total ) );
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::AddFlatFiles( const StringList& files )
{
   for ( const String& file : files )
   {
      bool exists = false;
      for ( const auto& path : m_instance.p_flatFrames )
      {
         if ( path == file )
         {
            exists = true;
            break;
         }
      }

      if ( !exists )
         m_instance.p_flatFrames.push_back( file );
   }

   UpdateFlatList();
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
   else if ( sender == GUI->AddFlats_PushButton )
   {
      OpenFileDialog dlg;
      dlg.SetCaption( "Select Flat Frames" );
      dlg.SetFilter( FileFilter( "FITS files", ".fit .fits .fts .xisf" ) );
      dlg.EnableMultipleSelections();

      if ( dlg.Execute() )
         AddFlatFiles( dlg.FileNames() );
   }
   else if ( sender == GUI->RemoveFlats_PushButton )
   {
      IndirectArray<TreeBox::Node> selected = GUI->FlatFiles_TreeBox.SelectedNodes();
      if ( !selected.IsEmpty() )
      {
         Array<int> indices;
         for ( const TreeBox::Node* node : selected )
            indices.Add( GUI->FlatFiles_TreeBox.ChildIndex( node ) );

         indices.Sort();
         for ( int i = static_cast<int>( indices.Length() ) - 1; i >= 0; --i )
         {
            int idx = indices[i];
            if ( idx >= 0 && static_cast<size_t>( idx ) < m_instance.p_flatFrames.size() )
               m_instance.p_flatFrames.erase( m_instance.p_flatFrames.begin() + idx );
         }

         UpdateFlatList();
      }
   }
   else if ( sender == GUI->ClearFlats_PushButton )
   {
      if ( !m_instance.p_flatFrames.empty() )
      {
         if ( MessageBox( "Remove all flat frames?", "NukeXStack",
                          StdIcon::Question, StdButton::Yes, StdButton::No ).Execute() == StdButton::Yes )
         {
            m_instance.p_flatFrames.clear();
            UpdateFlatList();
         }
      }
   }
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_ComboBoxItemSelected( ComboBox& sender, int itemIndex )
{
   (void)sender;
   (void)itemIndex;
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_CheckBoxClick( Button& sender, bool checked )
{
   if ( sender == GUI->EnableMetadataTiebreaker_CheckBox )
   {
      m_instance.p_enableMetadataTiebreaker = checked;
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
   else if ( sender == GUI->EnableRemediation_CheckBox )
   {
      m_instance.p_enableRemediation = checked;
      GUI->EnableDustRemediation_CheckBox.Enable( checked );
      GUI->EnableVignettingRemediation_CheckBox.Enable( checked );
   }
   else if ( sender == GUI->EnableDustRemediation_CheckBox )
   {
      m_instance.p_enableDustRemediation = checked;
   }
   else if ( sender == GUI->EnableVignettingRemediation_CheckBox )
   {
      m_instance.p_enableVignettingRemediation = checked;
   }
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::e_NumericValueUpdated( NumericEdit& sender, double value )
{
   if ( sender == GUI->OutlierSigma_NumericControl )
      m_instance.p_outlierSigmaThreshold = value;
   else if ( sender == GUI->BortleNumber_NumericControl )
      m_instance.p_bortleNumber = static_cast<int32>( value );
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
   // Flat Files Section (Optional Calibration)
   // =========================================================================

   FlatFiles_SectionBar.SetTitle( "Flat Frames (Optional)" );
   FlatFiles_SectionBar.SetSection( FlatFiles_Control );

   FlatFiles_TreeBox.SetMinHeight( 80 );
   FlatFiles_TreeBox.SetMaxHeight( 150 );
   FlatFiles_TreeBox.SetNumberOfColumns( 3 );
   FlatFiles_TreeBox.SetHeaderText( 0, "File" );
   FlatFiles_TreeBox.SetHeaderText( 1, "Path" );
   FlatFiles_TreeBox.SetHeaderText( 2, "Size" );
   FlatFiles_TreeBox.SetColumnWidth( 0, 200 );
   FlatFiles_TreeBox.SetColumnWidth( 1, 300 );
   FlatFiles_TreeBox.SetColumnWidth( 2, 80 );
   FlatFiles_TreeBox.EnableMultipleSelections();
   FlatFiles_TreeBox.EnableHeaderSorting();
   FlatFiles_TreeBox.SetToolTip( "<p>Flat calibration frames. Median-stacked and applied to lights before alignment.</p>" );

   AddFlats_PushButton.SetText( "Add Files" );
   AddFlats_PushButton.SetToolTip( "<p>Add flat calibration frames.</p>" );
   AddFlats_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   RemoveFlats_PushButton.SetText( "Remove" );
   RemoveFlats_PushButton.SetToolTip( "<p>Remove selected flat frames from the list.</p>" );
   RemoveFlats_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   ClearFlats_PushButton.SetText( "Clear" );
   ClearFlats_PushButton.SetToolTip( "<p>Remove all flat frames.</p>" );
   ClearFlats_PushButton.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_ButtonClick, w );

   FlatFiles_Buttons_HSizer.SetSpacing( 4 );
   FlatFiles_Buttons_HSizer.Add( AddFlats_PushButton );
   FlatFiles_Buttons_HSizer.Add( RemoveFlats_PushButton );
   FlatFiles_Buttons_HSizer.Add( ClearFlats_PushButton );
   FlatFiles_Buttons_HSizer.AddStretch();

   FlatCount_Label.SetText( "0 flat frames" );
   FlatCount_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   FlatFiles_Info_HSizer.AddStretch();
   FlatFiles_Info_HSizer.Add( FlatCount_Label );

   FlatFiles_Sizer.SetSpacing( 4 );
   FlatFiles_Sizer.Add( FlatFiles_TreeBox );
   FlatFiles_Sizer.Add( FlatFiles_Buttons_HSizer );
   FlatFiles_Sizer.Add( FlatFiles_Info_HSizer );

   FlatFiles_Control.SetSizer( FlatFiles_Sizer );
   FlatFiles_Control.Hide();

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

   Quality_SectionBar.SetTitle( "Metadata Tiebreaker" );
   Quality_SectionBar.SetSection( Quality_Control );

   EnableMetadataTiebreaker_CheckBox.SetText( "Enable Metadata Tiebreaker" );
   EnableMetadataTiebreaker_CheckBox.SetToolTip( "<p>When multiple frames are statistically indistinguishable, "
                                                  "prefer the one with better seeing and tracking (from FITS metadata).</p>" );
   EnableMetadataTiebreaker_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   Quality_Sizer.SetSpacing( 4 );
   Quality_Sizer.Add( EnableMetadataTiebreaker_CheckBox );

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

   BortleNumber_NumericControl.label.SetText( "Bortle Number:" );
   BortleNumber_NumericControl.label.SetMinWidth( labelWidth1 );
   BortleNumber_NumericControl.slider.SetRange( 0, 8 );
   BortleNumber_NumericControl.SetInteger();
   BortleNumber_NumericControl.SetRange( TheNXSBortleNumberParameter->MinimumValue(),
                                          TheNXSBortleNumberParameter->MaximumValue() );
   BortleNumber_NumericControl.edit.SetMinWidth( editWidth1 );
   BortleNumber_NumericControl.SetToolTip( "<p>Bortle Dark-Sky Scale (1" "\xe2\x80\x93" "9). Controls how bright the "
                                            "background sky is allowed to be in the auto-stretched output.</p>"
                                            "<p><b>1" "\xe2\x80\x93" "3</b> " "\xe2\x80\x94" " Excellent to good dark site (target median " "\xe2\x89\xa4" " 0.25)<br/>"
                                            "<b>4" "\xe2\x80\x93" "5</b> " "\xe2\x80\x94" " Rural/suburban transition (target median " "\xe2\x89\xa4" " 0.20)<br/>"
                                            "<b>6" "\xe2\x80\x93" "7</b> " "\xe2\x80\x94" " Suburban sky (target median " "\xe2\x89\xa4" " 0.16)<br/>"
                                            "<b>8" "\xe2\x80\x93" "9</b> " "\xe2\x80\x94" " City/inner-city sky (target median " "\xe2\x89\xa4" " 0.12)</p>" );
   BortleNumber_NumericControl.OnValueUpdated( (NumericEdit::value_event_handler)&NukeXStackInterface::e_NumericValueUpdated, w );

   Output_Sizer.SetSpacing( 4 );
   Output_Sizer.Add( GenerateProvenance_CheckBox );
   Output_Sizer.Add( GenerateDistMetadata_CheckBox );
   Output_Sizer.Add( EnableAutoStretch_CheckBox );
   Output_Sizer.Add( UseGPU_CheckBox );
   Output_Sizer.Add( AdaptiveModels_CheckBox );
   Output_Sizer.Add( BortleNumber_NumericControl );

   Output_Control.SetSizer( Output_Sizer );

   // =========================================================================
   // Remediation Section
   // =========================================================================

   Remediation_SectionBar.SetTitle( "Remediation" );
   Remediation_SectionBar.SetSection( Remediation_Control );

   EnableRemediation_CheckBox.SetText( "Enable Remediation" );
   EnableRemediation_CheckBox.SetToolTip( "<p>Master switch for post-stack remediation. When enabled, "
                                           "dust mote and vignetting corrections are applied to the "
                                           "stretched output.</p>" );
   EnableRemediation_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   EnableDustRemediation_CheckBox.SetText( "Auto-Correct Dust Motes" );
   EnableDustRemediation_CheckBox.SetToolTip( "<p>Automatically detect and correct circular dust mote "
                                               "artifacts using sensor-space analysis and self-flat "
                                               "correction.</p>" );
   EnableDustRemediation_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   EnableVignettingRemediation_CheckBox.SetText( "Auto-Correct Vignetting" );
   EnableVignettingRemediation_CheckBox.SetToolTip( "<p>Automatically detect and correct radial vignetting "
                                                     "gradient using polynomial surface fitting.</p>" );
   EnableVignettingRemediation_CheckBox.OnClick( (Button::click_event_handler)&NukeXStackInterface::e_CheckBoxClick, w );

   Remediation_Sizer.SetSpacing( 4 );
   Remediation_Sizer.Add( EnableRemediation_CheckBox );
   Remediation_Sizer.Add( EnableDustRemediation_CheckBox );
   Remediation_Sizer.Add( EnableVignettingRemediation_CheckBox );

   Remediation_Control.SetSizer( Remediation_Sizer );

   // =========================================================================
   // Global Layout
   // =========================================================================

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( InputFiles_SectionBar );
   Global_Sizer.Add( InputFiles_Control, 100 );
   Global_Sizer.Add( FlatFiles_SectionBar );
   Global_Sizer.Add( FlatFiles_Control );
   Global_Sizer.Add( Outliers_SectionBar );
   Global_Sizer.Add( Outliers_Control );
   Global_Sizer.Add( Quality_SectionBar );
   Global_Sizer.Add( Quality_Control );
   Global_Sizer.Add( Output_SectionBar );
   Global_Sizer.Add( Output_Control );
   Global_Sizer.Add( Remediation_SectionBar );
   Global_Sizer.Add( Remediation_Control );

   w.SetSizer( Global_Sizer );

   w.EnsureLayoutUpdated();
   w.AdjustToContents();

   int minWidth = w.Font().Width( String( 'M', 80 ) );
   w.SetMinSize( minWidth, 600 );
}

// ----------------------------------------------------------------------------

} // namespace pcl
