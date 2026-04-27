// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NukeXInterface.h"
#include "NukeXProcess.h"
#include "NukeXParameters.h"
#include "RatingDialog.h"

#include <pcl/FileDialog.h>
#include <pcl/ErrorHandler.h>

namespace pcl
{

NukeXInterface* TheNukeXInterface = nullptr;

NukeXInterface::NukeXInterface()
   : instance( TheNukeXProcess )
{
   TheNukeXInterface = this;
}

NukeXInterface::~NukeXInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

IsoString NukeXInterface::Id() const
{
   return "NukeX";
}

MetaProcess* NukeXInterface::Process() const
{
   return TheNukeXProcess;
}

String NukeXInterface::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

InterfaceFeatures NukeXInterface::Features() const
{
   return InterfaceFeature::DefaultGlobal;
}

void NukeXInterface::ApplyInstance() const
{
   instance.LaunchGlobal();
}

void NukeXInterface::ResetInstance()
{
   NukeXInstance defaultInstance( TheNukeXProcess );
   ImportProcess( defaultInstance );
}

bool NukeXInterface::Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "NukeX v4" );
      UpdateControls();
   }

   dynamic = false;
   return true;
}

ProcessImplementation* NukeXInterface::NewProcess() const
{
   return new NukeXInstance( instance );
}

bool NukeXInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   const NukeXInstance* inst = dynamic_cast<const NukeXInstance*>( &p );
   if ( inst == nullptr )
   {
      whyNot = "Not a NukeX instance.";
      return false;
   }
   if ( inst->lightFrames.IsEmpty() )
   {
      whyNot = "No light frames specified.";
      return false;
   }
   return true;
}

bool NukeXInterface::RequiresInstanceValidation() const
{
   return true;
}

bool NukeXInterface::ImportProcess( const ProcessImplementation& p )
{
   instance.Assign( p );
   UpdateControls();
   return true;
}

// ── GUI Construction ─────────────────────────────────────────────

NukeXInterface::GUIData::GUIData( NukeXInterface& w )
{
   // ── Light Frames Section ──
   LightFrames_TreeBox.SetMinHeight( 200 );
   LightFrames_TreeBox.SetNumberOfColumns( 2 );
   LightFrames_TreeBox.SetHeaderText( 0, "File" );
   LightFrames_TreeBox.SetHeaderText( 1, "Enabled" );
   LightFrames_TreeBox.EnableAlternateRowColor();

   LightFrames_TreeBox.SetToolTip(
      "Light frames to stack.  The first enabled frame becomes the alignment "
      "reference (H = identity); subsequent frames are aligned to it via "
      "triangle-similarity star matching.  Double-click a row to toggle its "
      "Enabled flag." );
   LightFrames_Add_Button.SetText( "Add" );
   LightFrames_Add_Button.SetToolTip( "Add FITS files (.fit/.fits) to the light frames list." );
   LightFrames_Add_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_LightAdd, w );
   LightFrames_Remove_Button.SetText( "Remove" );
   LightFrames_Remove_Button.SetToolTip( "Remove the currently selected row(s) from the list." );
   LightFrames_Remove_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_LightRemove, w );
   LightFrames_Clear_Button.SetText( "Clear" );
   LightFrames_Clear_Button.SetToolTip( "Clear all light frames." );
   LightFrames_Clear_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_LightClear, w );
   LightFrames_SelectAll_Button.SetText( "Toggle All" );
   LightFrames_SelectAll_Button.SetToolTip( "Enable/disable all light frames in one click." );
   LightFrames_SelectAll_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_LightSelectAll, w );

   LightFrames_Count_Label.SetText( "0 frames" );
   LightFrames_Count_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   LightFrames_TreeBox.OnNodeDoubleClicked( (TreeBox::node_event_handler)&NukeXInterface::e_LightNodeDoubleClicked, w );

   LightFrames_Buttons_Sizer.SetSpacing( 4 );
   LightFrames_Buttons_Sizer.Add( LightFrames_Add_Button );
   LightFrames_Buttons_Sizer.Add( LightFrames_Remove_Button );
   LightFrames_Buttons_Sizer.Add( LightFrames_Clear_Button );
   LightFrames_Buttons_Sizer.Add( LightFrames_SelectAll_Button );
   LightFrames_Buttons_Sizer.AddStretch();
   LightFrames_Buttons_Sizer.Add( LightFrames_Count_Label );

   LightFrames_Sizer.SetSpacing( 4 );
   LightFrames_Sizer.Add( LightFrames_TreeBox, 100 );
   LightFrames_Sizer.Add( LightFrames_Buttons_Sizer );

   LightFrames_Control.SetSizer( LightFrames_Sizer );

   LightFrames_SectionBar.SetTitle( "Light Frames" );
   LightFrames_SectionBar.SetSection( LightFrames_Control );

   // ── Flat Frames Section ──
   FlatFrames_TreeBox.SetMinHeight( 120 );
   FlatFrames_TreeBox.SetNumberOfColumns( 2 );
   FlatFrames_TreeBox.SetHeaderText( 0, "File" );
   FlatFrames_TreeBox.SetHeaderText( 1, "Enabled" );
   FlatFrames_TreeBox.EnableAlternateRowColor();

   FlatFrames_TreeBox.SetToolTip(
      "Optional flat-field frames.  Enabled flats are combined into a master "
      "flat and applied per-channel before alignment.  Leave empty if you "
      "have no flats — NukeX will skip calibration." );
   FlatFrames_Add_Button.SetText( "Add" );
   FlatFrames_Add_Button.SetToolTip( "Add FITS files (.fit/.fits) to the flat frames list." );
   FlatFrames_Add_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_FlatAdd, w );
   FlatFrames_Remove_Button.SetText( "Remove" );
   FlatFrames_Remove_Button.SetToolTip( "Remove the currently selected row(s) from the list." );
   FlatFrames_Remove_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_FlatRemove, w );
   FlatFrames_Clear_Button.SetText( "Clear" );
   FlatFrames_Clear_Button.SetToolTip( "Clear all flat frames." );
   FlatFrames_Clear_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_FlatClear, w );

   FlatFrames_Count_Label.SetText( "0 frames" );
   FlatFrames_Count_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );

   FlatFrames_Buttons_Sizer.SetSpacing( 4 );
   FlatFrames_Buttons_Sizer.Add( FlatFrames_Add_Button );
   FlatFrames_Buttons_Sizer.Add( FlatFrames_Remove_Button );
   FlatFrames_Buttons_Sizer.Add( FlatFrames_Clear_Button );
   FlatFrames_Buttons_Sizer.AddStretch();
   FlatFrames_Buttons_Sizer.Add( FlatFrames_Count_Label );

   FlatFrames_Sizer.SetSpacing( 4 );
   FlatFrames_Sizer.Add( FlatFrames_TreeBox, 100 );
   FlatFrames_Sizer.Add( FlatFrames_Buttons_Sizer );

   FlatFrames_Control.SetSizer( FlatFrames_Sizer );

   FlatFrames_SectionBar.SetTitle( "Flat Frames (Optional)" );
   FlatFrames_SectionBar.SetSection( FlatFrames_Control );
   FlatFrames_Control.Hide();  // Start collapsed — flats are optional

   // ── Options Section ──
   const char* kPrimaryStretchTip =
      "Curve applied to the stacked image to produce NukeX_stretched.\n\n"
      "Auto — picks a Phase-5 champion curve based on the first light "
      "frame's FITS metadata (FILTER / BAYERPAT / NAXIS3). Recommended.\n\n"
      "VeraLux / GHS / MTF / ArcSinh / Log / Lupton / CLAHE — force a "
      "specific curve regardless of filter class.\n\n"
      "The Process Console logs the Auto classification and choice so "
      "you can see exactly why a given curve was picked.";
   PrimaryStretch_Label.SetText( "Primary Stretch:" );
   PrimaryStretch_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   PrimaryStretch_Label.SetToolTip( kPrimaryStretchTip );
   PrimaryStretch_ComboBox.AddItem( "Auto" );
   PrimaryStretch_ComboBox.AddItem( "VeraLux" );
   PrimaryStretch_ComboBox.AddItem( "GHS" );
   PrimaryStretch_ComboBox.AddItem( "MTF" );
   PrimaryStretch_ComboBox.AddItem( "ArcSinh" );
   PrimaryStretch_ComboBox.AddItem( "Log" );
   PrimaryStretch_ComboBox.AddItem( "Lupton" );
   PrimaryStretch_ComboBox.AddItem( "CLAHE" );
   PrimaryStretch_ComboBox.SetToolTip( kPrimaryStretchTip );
   PrimaryStretch_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::e_ItemSelected, w );

   PrimaryStretch_Sizer.SetSpacing( 4 );
   PrimaryStretch_Sizer.Add( PrimaryStretch_Label );
   PrimaryStretch_Sizer.Add( PrimaryStretch_ComboBox, 100 );

   const char* kFinishingStretchTip =
      "Optional second-stage stretch applied after the Primary curve.\n\n"
      "None — only curve enrolled today. SAS / OTS / Photometric "
      "finishers are slated for future phases.";
   FinishingStretch_Label.SetText( "Finishing Stretch:" );
   FinishingStretch_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   FinishingStretch_Label.SetToolTip( kFinishingStretchTip );
   FinishingStretch_ComboBox.AddItem( "None" );
   FinishingStretch_ComboBox.SetToolTip( kFinishingStretchTip );
   FinishingStretch_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::e_ItemSelected, w );

   FinishingStretch_Sizer.SetSpacing( 4 );
   FinishingStretch_Sizer.Add( FinishingStretch_Label );
   FinishingStretch_Sizer.Add( FinishingStretch_ComboBox, 100 );

   EnableGPU_CheckBox.SetText( "Enable GPU acceleration (OpenCL)" );
   EnableGPU_CheckBox.SetToolTip(
      "Runs Phase B's per-voxel weight classification, robust statistics, "
      "and pixel-selection kernels on an OpenCL device "
      "(NVIDIA / AMD / Intel).  Distribution fitting (Ceres) stays on "
      "CPU regardless.  Disable to run the whole stack on CPU — useful "
      "for debugging or on machines without OpenCL.  Default: on." );
   EnableGPU_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_OptionToggled, w );

   GPU_Sizer.SetSpacing( 16 );
   GPU_Sizer.Add( EnableGPU_CheckBox );
   GPU_Sizer.AddStretch();

   // ── Phase 8 rating controls ──
   // The button is only useful after an Execute has populated
   // instance.lastRun; UpdateControls() keeps its enabled state in sync.
   RateLastRun_Button.SetText( "Rate last run" );
   RateLastRun_Button.SetToolTip(
      "Re-opens the rating dialog against the most recent Execute so you "
      "can score the stack after the fact.  Disabled until the first run "
      "in this session produces a stretched image.  Saving a rating here "
      "writes to the same per-user database the automatic popup uses." );
   RateLastRun_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_RateLastRun, w );

   SuppressRating_CheckBox.SetText( "Don't show rating popup after Execute" );
   SuppressRating_CheckBox.SetToolTip(
      "When checked, NukeX will not open the rating dialog automatically "
      "after an Execute.  You can still rate runs via the \"Rate last run\" "
      "button.  Persists across PixInsight restarts under setting key "
      "NukeX/Phase8/RatingPopupSuppressed." );
   SuppressRating_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_SuppressRating, w );

   Rating_Sizer.SetSpacing( 16 );
   Rating_Sizer.Add( RateLastRun_Button );
   Rating_Sizer.Add( SuppressRating_CheckBox );
   Rating_Sizer.AddStretch();

   Options_Sizer.SetSpacing( 4 );
   Options_Sizer.Add( PrimaryStretch_Sizer );
   Options_Sizer.Add( FinishingStretch_Sizer );
   Options_Sizer.Add( GPU_Sizer );
   Options_Sizer.Add( Rating_Sizer );

   Options_Control.SetSizer( Options_Sizer );

   Options_SectionBar.SetTitle( "Options" );
   Options_SectionBar.SetSection( Options_Control );

   // ── Global Layout ──
   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( LightFrames_SectionBar );
   Global_Sizer.Add( LightFrames_Control, 100 );
   Global_Sizer.Add( FlatFrames_SectionBar );
   Global_Sizer.Add( FlatFrames_Control );
   Global_Sizer.Add( Options_SectionBar );
   Global_Sizer.Add( Options_Control );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
}

// ── Update helpers ───────────────────────────────────────────────

void NukeXInterface::UpdateControls()
{
   if ( GUI == nullptr ) return;
   UpdateLightFramesList();
   UpdateFlatFramesList();
   GUI->PrimaryStretch_ComboBox.SetCurrentItem( instance.primaryStretch );
   GUI->FinishingStretch_ComboBox.SetCurrentItem( instance.finishingStretch );
   GUI->EnableGPU_CheckBox.SetChecked( instance.enableGPU );

   // Phase 8: "Rate last run" is meaningful only when ExecuteGlobal has
   // populated lastRun; SuppressRating reflects the PCL-Settings-backed
   // opt-out.
   GUI->RateLastRun_Button.Enable( instance.lastRun.valid );
   GUI->SuppressRating_CheckBox.SetChecked(
      TheNukeXProcess != nullptr && TheNukeXProcess->rating_popup_suppressed() );
}

void NukeXInterface::UpdateLightFramesList()
{
   GUI->LightFrames_TreeBox.Clear();
   GUI->LightFrames_TreeBox.DisableUpdates();
   int enabled_count = 0;
   for ( size_type i = 0; i < instance.lightFrames.Length(); ++i )
   {
      TreeBox::Node* node = new TreeBox::Node( GUI->LightFrames_TreeBox );
      // Show just the filename, not the full path
      String path = instance.lightFrames[i].path;
      size_type sep = path.FindLast( '/' );
      String filename = ( sep != String::notFound ) ? path.Substring( sep + 1 ) : path;
      node->SetText( 0, filename );
      node->SetToolTip( 0, path );  // Full path on hover
      node->SetText( 1, instance.lightFrames[i].enabled ? "Yes" : "No" );
      if ( instance.lightFrames[i].enabled ) enabled_count++;
   }
   GUI->LightFrames_TreeBox.EnableUpdates();
   GUI->LightFrames_Count_Label.SetText(
      String().Format( "%d/%d frames", enabled_count, instance.lightFrames.Length() ) );
}

void NukeXInterface::UpdateFlatFramesList()
{
   GUI->FlatFrames_TreeBox.Clear();
   GUI->FlatFrames_TreeBox.DisableUpdates();
   int enabled_count = 0;
   for ( size_type i = 0; i < instance.flatFrames.Length(); ++i )
   {
      TreeBox::Node* node = new TreeBox::Node( GUI->FlatFrames_TreeBox );
      String path = instance.flatFrames[i].path;
      size_type sep = path.FindLast( '/' );
      String filename = ( sep != String::notFound ) ? path.Substring( sep + 1 ) : path;
      node->SetText( 0, filename );
      node->SetToolTip( 0, path );
      node->SetText( 1, instance.flatFrames[i].enabled ? "Yes" : "No" );
      if ( instance.flatFrames[i].enabled ) enabled_count++;
   }
   GUI->FlatFrames_TreeBox.EnableUpdates();
   GUI->FlatFrames_Count_Label.SetText(
      String().Format( "%d/%d frames", enabled_count, instance.flatFrames.Length() ) );
}

// ── Event handlers ───────────────────────────────────────────────

void NukeXInterface::e_LightAdd( Button&, bool )
{
   OpenFileDialog d;
   d.SetCaption( "NukeX: Add Light Frames" );
   d.SetFilter( FileFilter( "FITS Files", StringList() << ".fit" << ".fits" << ".fts" ) );
   d.EnableMultipleSelections();
   if ( d.Execute() )
   {
      for ( const auto& f : d.FileNames() )
      {
         NukeXInstance::FrameItem item;
         item.path = f;
         item.enabled = true;
         instance.lightFrames.Add( item );
      }
      UpdateLightFramesList();
   }
}

void NukeXInterface::e_LightRemove( Button&, bool )
{
   int idx = GUI->LightFrames_TreeBox.CurrentNode() ?
             GUI->LightFrames_TreeBox.ChildIndex( GUI->LightFrames_TreeBox.CurrentNode() ) : -1;
   if ( idx >= 0 && idx < int( instance.lightFrames.Length() ) )
   {
      instance.lightFrames.Remove( instance.lightFrames.At( idx ) );
      UpdateLightFramesList();
   }
}

void NukeXInterface::e_LightClear( Button&, bool )
{
   instance.lightFrames.Clear();
   UpdateLightFramesList();
}

void NukeXInterface::e_LightSelectAll( Button&, bool )
{
   // Toggle: if any are disabled, enable all; otherwise disable all
   bool any_disabled = false;
   for ( const auto& f : instance.lightFrames )
      if ( !f.enabled ) { any_disabled = true; break; }

   for ( auto& f : instance.lightFrames )
      f.enabled = any_disabled;  // Enable all if any were disabled, else disable all

   UpdateLightFramesList();
}

void NukeXInterface::e_LightNodeDoubleClicked( TreeBox&, TreeBox::Node& node, int )
{
   // Double-click toggles enabled/disabled for that frame
   int idx = GUI->LightFrames_TreeBox.ChildIndex( &node );
   if ( idx >= 0 && idx < int( instance.lightFrames.Length() ) )
   {
      instance.lightFrames[idx].enabled = !instance.lightFrames[idx].enabled;
      UpdateLightFramesList();
   }
}

void NukeXInterface::e_FlatAdd( Button&, bool )
{
   OpenFileDialog d;
   d.SetCaption( "NukeX: Add Flat Frames" );
   d.SetFilter( FileFilter( "FITS Files", StringList() << ".fit" << ".fits" << ".fts" ) );
   d.EnableMultipleSelections();
   if ( d.Execute() )
   {
      for ( const auto& f : d.FileNames() )
      {
         NukeXInstance::FrameItem item;
         item.path = f;
         item.enabled = true;
         instance.flatFrames.Add( item );
      }
      UpdateFlatFramesList();
   }
}

void NukeXInterface::e_FlatRemove( Button&, bool )
{
   int idx = GUI->FlatFrames_TreeBox.CurrentNode() ?
             GUI->FlatFrames_TreeBox.ChildIndex( GUI->FlatFrames_TreeBox.CurrentNode() ) : -1;
   if ( idx >= 0 && idx < int( instance.flatFrames.Length() ) )
   {
      instance.flatFrames.Remove( instance.flatFrames.At( idx ) );
      UpdateFlatFramesList();
   }
}

void NukeXInterface::e_FlatClear( Button&, bool )
{
   instance.flatFrames.Clear();
   UpdateFlatFramesList();
}

void NukeXInterface::e_ItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->PrimaryStretch_ComboBox )
      instance.primaryStretch = itemIndex;
   else if ( sender == GUI->FinishingStretch_ComboBox )
      instance.finishingStretch = itemIndex;
}

void NukeXInterface::e_OptionToggled( Button& sender, bool checked )
{
   if ( sender == GUI->EnableGPU_CheckBox )
      instance.enableGPU = checked;
}

// ── Phase 8 rating handlers ──────────────────────────────────────

void NukeXInterface::e_RateLastRun( Button& /*sender*/, bool /*checked*/ )
{
   if ( !instance.lastRun.valid )
      return;

   RatingDialog dlg( instance.lastRun.filter_class );
   RatingResult res = dlg.Run();

   if ( res.saved )
      instance.SaveRatingFromLastRun( res );

   if ( res.dont_show_again )
   {
      if ( TheNukeXProcess != nullptr )
         TheNukeXProcess->set_rating_popup_suppressed( true );
      GUI->SuppressRating_CheckBox.SetChecked( true );
   }
}

void NukeXInterface::e_SuppressRating( Button& /*sender*/, bool checked )
{
   if ( TheNukeXProcess != nullptr )
      TheNukeXProcess->set_rating_popup_suppressed( checked );
}

} // namespace pcl
