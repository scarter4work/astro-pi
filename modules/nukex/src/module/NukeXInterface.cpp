// NukeX v4 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "NukeXInterface.h"
#include "NukeXProcess.h"
#include "NukeXParameters.h"
#include "NukeXVersion.h"
#include "RatingDialog.h"

#include "QEFetcher.h"

#include "nukex/calibration/qe_update.hpp"
#include "nukex/calibration/qe_update_state.hpp"
#include "nukex/calibration/qe_database.hpp"

#include <pcl/FileDialog.h>
#include <pcl/MessageBox.h>
#include <pcl/Console.h>

#include <ctime>
#include <filesystem>
#include <cstdlib>
#include <string>
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
      SetWindowTitle( "NukeX" );
      UpdateControls();
      UpdateDatabaseStatusLabel();
      // Interval-gated, silent when offline or up to date. This is what
      // keeps the camera roster from going stale without anyone having to
      // remember to look -- the failure mode this feature exists to avoid.
      CheckForDatabaseUpdate( false );
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
      "have no flats -- NukeX will skip calibration." );
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
      "Auto -- picks a Phase-5 champion curve based on the first light "
      "frame's FITS metadata (FILTER / BAYERPAT / NAXIS3). Recommended.\n\n"
      "VeraLux / GHS / MTF / ArcSinh / Log / Lupton / CLAHE -- force a "
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
      "None -- only curve enrolled today. SAS / OTS / Photometric "
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

   BackgroundTarget_NumericControl.label.SetText( "Background level:" );
   BackgroundTarget_NumericControl.slider.SetRange( 0, 90 );
   BackgroundTarget_NumericControl.SetReal();
   BackgroundTarget_NumericControl.SetRange(
      TheNXBackgroundTargetParameter->MinimumValue(),
      TheNXBackgroundTargetParameter->MaximumValue() );
   BackgroundTarget_NumericControl.SetPrecision(
      TheNXBackgroundTargetParameter->Precision() );
   BackgroundTarget_NumericControl.SetToolTip(
      "Where the auto-stretch puts the sky, from 0.05 (dark) to 0.50 (bright).\n\n"
      "0.25 is the screen-autostretch convention and the default. It is also "
      "what lifts the noise floor into plain view: lower the target and the "
      "sky darkens without losing a pixel of detail, so faint grain stops "
      "competing with the subject. Raise it to judge the faintest structure.\n\n"
      "Affects the stretched image only. The stacked and composed images are "
      "linear and are not touched." );
   BackgroundTarget_NumericControl.edit.SetFixedWidth( 80 );
   BackgroundTarget_NumericControl.OnValueUpdated(
      (NumericEdit::value_event_handler)&NukeXInterface::e_ValueUpdated, w );

   EnableGPU_CheckBox.SetText( "Enable GPU acceleration (OpenCL)" );
   EnableGPU_CheckBox.SetToolTip(
      "Runs Phase B's per-voxel weight classification, robust statistics, "
      "and pixel-selection kernels on an OpenCL device "
      "(NVIDIA / AMD / Intel).  Distribution fitting (Ceres) stays on "
      "CPU regardless.  Disable to run the whole stack on CPU -- useful "
      "for debugging or on machines without OpenCL.  Default: on." );
   EnableGPU_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_OptionToggled, w );

   Options_Sizer.Add( BackgroundTarget_NumericControl );

   const char* kEstimatorTip =
      "Which estimator produces each pixel of the stack.  Distribution race: "
      "NukeX's original method -- fits Student-t, Gaussian-mixture, "
      "Contamination and KDE models to every pixel's samples and picks by "
      "AICc; also produces the per-pixel model diagnostics.  Huber "
      "M-estimator: a robust weighted location (median seed, MAD scale, "
      "tuning 1.345).  Measured on four real sessions the race's result is "
      "1.05x to 1.47x noisier than Huber's at the pixel scale, and Huber "
      "costs a small fraction of the time.  Huber is the default since "
      "5.0.6.0.";
   Estimator_Label.SetText( "Estimator:" );
   Estimator_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   Estimator_Label.SetToolTip( kEstimatorTip );
   Estimator_ComboBox.AddItem( "Distribution race" );
   Estimator_ComboBox.AddItem( "Huber M-estimator" );
   Estimator_ComboBox.SetToolTip( kEstimatorTip );
   Estimator_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::e_ItemSelected, w );
   Estimator_Sizer.SetSpacing( 4 );
   Estimator_Sizer.Add( Estimator_Label );
   Estimator_Sizer.Add( Estimator_ComboBox );
   Estimator_Sizer.AddStretch();
   Options_Sizer.Add( Estimator_Sizer );

   RemoveSkyGradient_CheckBox.SetText( "Remove sky gradient (tilt only)" );
   RemoveSkyGradient_CheckBox.SetToolTip(
      "Removes the fixed-pattern sky tilt from the stacked image: a flat "
      "plane fitted to the background, with anything brighter than the sky "
      "ignored so stars and nebulosity do not pull it.  Only the tilt comes "
      "off -- the sky level is left where it was, and a curved gradient "
      "(vignetting, a dome) is deliberately not touched, because a curved "
      "fit can follow the faint outskirts of a large object and subtract "
      "them.  Turn off to keep the stack exactly as accumulated, e.g. to "
      "remove gradients yourself in PixInsight.  Default: on." );
   RemoveSkyGradient_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_OptionToggled, w );

   SkyGradient_Sizer.SetSpacing( 16 );
   SkyGradient_Sizer.Add( RemoveSkyGradient_CheckBox );
   SkyGradient_Sizer.AddStretch();
   Options_Sizer.Add( SkyGradient_Sizer );

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

   // ── QE override file picker ──
   // Edit is read-only; canonical input is the OpenFileDialog so the user
   // can't paste arbitrary path strings.  All other state lives in the
   // tooltip.
   const char* kQEOverrideTip =
      "Optional JSON file with custom camera/filter QE values to augment "
      "or replace the shipped quantum-efficiency database.  Useful when "
      "you have measured response curves for gear that isn't in the "
      "default DB.  Leave empty to use the shipped database only.";
   QEOverride_Label.SetText( "QE override file:" );
   QEOverride_Label.SetTextAlignment( TextAlign::Right | TextAlign::VertCenter );
   QEOverride_Label.SetToolTip( kQEOverrideTip );

   QEOverride_Edit.SetReadOnly();
   QEOverride_Edit.SetMinWidth( 360 );
   QEOverride_Edit.SetText( w.instance.qeOverridePath );
   QEOverride_Edit.SetToolTip( kQEOverrideTip );

   QEOverride_Browse_Button.SetText( "Browse..." );  // UTF-8 ellipsis
   QEOverride_Browse_Button.SetToolTip( "Select a QE override JSON file." );
   QEOverride_Browse_Button.OnClick(
      (Button::click_event_handler)&NukeXInterface::e_QEOverrideBrowse, w );

   QEOverride_Clear_Button.SetText( "Clear" );
   QEOverride_Clear_Button.SetToolTip( "Remove the QE override and use the shipped database only." );
   QEOverride_Clear_Button.OnClick(
      (Button::click_event_handler)&NukeXInterface::e_QEOverrideClear, w );

   QEOverride_Sizer.SetSpacing( 4 );
   QEOverride_Sizer.Add( QEOverride_Label );
   QEOverride_Sizer.Add( QEOverride_Edit, 100 );
   QEOverride_Sizer.Add( QEOverride_Browse_Button );
   QEOverride_Sizer.Add( QEOverride_Clear_Button );

   Options_Sizer.SetSpacing( 4 );
   Options_Sizer.Add( PrimaryStretch_Sizer );
   Options_Sizer.Add( FinishingStretch_Sizer );
   Options_Sizer.Add( GPU_Sizer );
   Options_Sizer.Add( Rating_Sizer );
   QEUpdate_CheckBox.SetText( "Check for camera database updates" );
   QEUpdate_CheckBox.SetToolTip(
      "<p>Periodically check whether a newer QE camera database has been "
      "published, so newly released cameras are recognised without waiting "
      "for a NukeX release.</p>"
      "<p>Nothing is downloaded or installed without your consent, the check "
      "never runs while a stack is in progress, and every published database "
      "is cryptographically signed and verified before use. Being offline is "
      "not an error.</p>" );
   QEUpdate_CheckBox.OnClick( (Button::click_event_handler)&NukeXInterface::e_QEUpdateToggled, w );

   QEUpdate_Check_Button.SetText( "Check now" );
   QEUpdate_Check_Button.SetToolTip( "<p>Check for a newer camera database immediately.</p>" );
   QEUpdate_Check_Button.OnClick( (Button::click_event_handler)&NukeXInterface::e_QEUpdateCheck, w );

   QEUpdate_Status_Label.SetTextAlignment( TextAlign::Left | TextAlign::VertCenter );

   QEUpdate_Sizer.SetSpacing( 4 );
   QEUpdate_Sizer.Add( QEUpdate_CheckBox );
   QEUpdate_Sizer.Add( QEUpdate_Check_Button );
   QEUpdate_Sizer.Add( QEUpdate_Status_Label, 100 );

   Options_Sizer.Add( QEOverride_Sizer );
   Options_Sizer.Add( QEUpdate_Sizer );

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
   GUI->BackgroundTarget_NumericControl.SetValue( instance.backgroundTarget );
   GUI->RemoveSkyGradient_CheckBox.SetChecked( instance.removeSkyGradient );
   GUI->Estimator_ComboBox.SetCurrentItem( instance.estimator );
   GUI->EnableGPU_CheckBox.SetChecked( instance.enableGPU );
   GUI->QEOverride_Edit.SetText( instance.qeOverridePath );

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
   else if ( sender == GUI->Estimator_ComboBox )
      instance.estimator = itemIndex;
}

void NukeXInterface::e_OptionToggled( Button& sender, bool checked )
{
   if ( sender == GUI->EnableGPU_CheckBox )
      instance.enableGPU = checked;
   else if ( sender == GUI->RemoveSkyGradient_CheckBox )
      instance.removeSkyGradient = checked;
}

void NukeXInterface::e_ValueUpdated( NumericEdit& sender, double value )
{
   if ( sender == GUI->BackgroundTarget_NumericControl )
      instance.backgroundTarget = static_cast<float>( value );
}

// ── QE override file picker handlers ─────────────────────────────

void NukeXInterface::e_QEOverrideBrowse( Button&, bool )
{
   OpenFileDialog d;
   d.SetCaption( "NukeX: Select QE Override JSON File" );
   d.SetFilter( FileFilter( "JSON Files", StringList() << ".json" ) );
   if ( d.Execute() && !d.FileNames().IsEmpty() )
   {
      instance.qeOverridePath = d.FileName();
      GUI->QEOverride_Edit.SetText( instance.qeOverridePath );
   }
}

void NukeXInterface::e_QEOverrideClear( Button&, bool )
{
   instance.qeOverridePath.Clear();
   GUI->QEOverride_Edit.Clear();
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

// ----------------------------------------------------------------------------
// Camera-database updater
// ----------------------------------------------------------------------------

namespace
{

// Published alongside the PixInsight repository. HTTPS only; QEFetcher
// forces TLS with peer and host verification, and the payload carries its
// own Ed25519 signature besides.
const char* kQEUpdateBaseURL =
   "https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository";

std::string QEUserDataDir()
{
   const char* home = std::getenv( "HOME" );
   return ( home ? std::string( home ) + "/.config" : std::string( "/tmp" ) )
          + "/nukex4";
}

std::string QEStatePath()    { return QEUserDataDir() + "/qe_update_state.json"; }
std::string QEDatabasePath() { return QEUserDataDir() + "/qe_database.json"; }

// The database in effect and its version: a download only while it is newer
// than the one built into this module (see nukex::active_qe_database).
nukex::ActiveQEDatabase CurrentQEDatabase( const nukex::QEUpdateState& st )
{
   std::error_code ec;
   const bool present = std::filesystem::exists( QEDatabasePath(), ec ) && !ec;
   return nukex::active_qe_database( st, present, nukex::embedded_qe_database_version() );
}

} // anonymous namespace

void NukeXInterface::UpdateDatabaseStatusLabel()
{
   if ( GUI == nullptr )
      return;

   const nukex::QEUpdateState st = nukex::load_update_state( QEStatePath() );
   GUI->QEUpdate_CheckBox.SetChecked( st.enabled );

   const nukex::ActiveQEDatabase active = CurrentQEDatabase( st );
   String text;
   if ( active.use_downloaded )
      text = String().Format( "database v%d (downloaded)", active.version );
   else if ( active.version > 0 )
      text = String().Format( "built-in database v%d", active.version );
   else
      text = "built-in database";
   GUI->QEUpdate_Status_Label.SetText( text );
}

void NukeXInterface::e_QEUpdateToggled( Button& sender, bool checked )
{
   if ( sender == GUI->QEUpdate_CheckBox )
   {
      nukex::QEUpdateState st = nukex::load_update_state( QEStatePath() );
      st.enabled = checked;
      nukex::save_update_state( QEStatePath(), st );
   }
}

void NukeXInterface::e_QEUpdateCheck( Button& sender, bool /*checked*/ )
{
   if ( sender == GUI->QEUpdate_Check_Button )
      CheckForDatabaseUpdate( true );
}

void NukeXInterface::CheckForDatabaseUpdate( bool user_initiated )
{
   nukex::QEUpdateState st = nukex::load_update_state( QEStatePath() );

   const long long now = static_cast<long long>( std::time( nullptr ) );
   if ( !user_initiated && !nukex::should_check_now( st, now ) )
      return;

   QEFetcher fetcher;
   nukex::QEUpdater updater( fetcher, kQEUpdateBaseURL,
                             nukex::qe_signing_public_key() );

   // Compare against the database actually in effect -- the built-in one
   // counts at the version it was published as, so a fresh install is never
   // offered the database it already carries.
   const nukex::CheckResult r = updater.check( CurrentQEDatabase( st ).version );

   // Record the attempt whatever happened, so a broken endpoint cannot turn
   // every interface open into a network round trip.
   st.last_check_unix = now;
   st.last_result     = nukex::to_string( r.outcome );
   nukex::save_update_state( QEStatePath(), st );

   switch ( r.outcome )
   {
   case nukex::UpdateOutcome::AVAILABLE:
      break;   // handled below

   case nukex::UpdateOutcome::UP_TO_DATE:
      if ( user_initiated )
         MessageBox( "The camera database is up to date.", "NukeX",
                     StdIcon::Information, StdButton::Ok ).Execute();
      return;

   case nukex::UpdateOutcome::OFFLINE:
      // Silent unless the user asked: a telescope laptop with no network is
      // the normal case, not a condition worth interrupting anyone over.
      if ( user_initiated )
         MessageBox( "Could not reach the camera database server.\n"
                     "This is not a problem -- the installed database is still in use.",
                     "NukeX", StdIcon::Information, StdButton::Ok ).Execute();
      return;

   case nukex::UpdateOutcome::BAD_SIGNATURE:
   case nukex::UpdateOutcome::DIGEST_MISMATCH:
      // Loud even when nobody asked. This is possible tampering, not a
      // network hiccup, and it must never be retried silently.
      Console().WarningLn(
         "<end><cbr>** NukeX: camera database update REJECTED -- " +
         String( nukex::to_string( r.outcome ) ) +
         ". The installed database is unchanged. If this repeats, do not "
         "install the update and report it." );
      return;

   case nukex::UpdateOutcome::SCHEMA_TOO_NEW:
      Console().WarningLn(
         "<end><cbr>** NukeX: the published camera database needs a newer "
         "version of NukeX. Update the module to receive it." );
      return;

   default:
      if ( user_initiated )
         MessageBox( String( "Camera database check: " ) +
                     nukex::to_string( r.outcome ), "NukeX",
                     StdIcon::Information, StdButton::Ok ).Execute();
      return;
   }

   // AVAILABLE. Respect a version the user already declined, unless they
   // asked for this check themselves.
   if ( !user_initiated && r.manifest.db_version == st.declined_version )
      return;

   String prompt = String().Format(
         "A newer camera database is available (v%d).\n\n", r.manifest.db_version );
   prompt += String().Format( "%d cameras across %d sensors.\n",
                              r.manifest.n_cameras, r.manifest.n_sensors );
   if ( !r.manifest.summary.empty() )
      prompt += String( r.manifest.summary.c_str() ) + "\n";
   prompt += "\nInstall it now?";

   if ( MessageBox( prompt, "NukeX", StdIcon::Question,
                    StdButton::Yes, StdButton::No ).Execute() != StdButton::Yes )
   {
      st.declined_version = r.manifest.db_version;
      nukex::save_update_state( QEStatePath(), st );
      return;
   }

   const nukex::UpdateOutcome out = updater.install( r.manifest, QEDatabasePath() );
   if ( out == nukex::UpdateOutcome::INSTALLED )
   {
      st.installed_db_version = r.manifest.db_version;
      st.declined_version     = 0;
      st.last_result          = nukex::to_string( out );
      nukex::save_update_state( QEStatePath(), st );
      UpdateDatabaseStatusLabel();
      Console().NoteLn( String().Format(
         "<end><cbr>* NukeX: camera database updated to v%d.", r.manifest.db_version ) );
   }
   else
   {
      Console().WarningLn(
         "<end><cbr>** NukeX: camera database update failed -- " +
         String( nukex::to_string( out ) ) +
         ". The installed database is unchanged." );
   }
}

} // namespace pcl
