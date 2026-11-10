// EZTools -- EZHazeKill interface with PixInsight's native Real-Time Preview.
// Copyright and licence: see src/EZToolsLicense.h.

#include "EZHazeKillInterface.h"
#include "EZHazeKillParameters.h"
#include "EZHazeKillProcess.h"

#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>
#include <pcl/ImageWindow.h>
#include <pcl/RealTimePreview.h>
#include <pcl/View.h>

namespace pcl
{

using namespace eztools;

EZHazeKillInterface* TheEZHazeKillInterface = nullptr;

EZHazeKillInterface::EZHazeKillInterface()
   : m_instance( TheEZHazeKillProcess )
{
   TheEZHazeKillInterface = this;
}

EZHazeKillInterface::~EZHazeKillInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

IsoString EZHazeKillInterface::Id() const
{
   return "EZHazeKill";
}

MetaProcess* EZHazeKillInterface::Process() const
{
   return TheEZHazeKillProcess;
}

InterfaceFeatures EZHazeKillInterface::Features() const
{
   return InterfaceFeature::Default | InterfaceFeature::RealTimeButton | InterfaceFeature::RealTimeF32Images;
}

void EZHazeKillInterface::ResetInstance()
{
   EZHazeKillInstance defaultInstance( TheEZHazeKillProcess );
   ImportProcess( defaultInstance );
}

void EZHazeKillInterface::RealTimePreviewUpdated( bool active )
{
   if ( GUI != nullptr )
   {
      if ( active )
         RealTimePreview::SetOwner( *this );
      else
         RealTimePreview::SetOwner( ProcessInterface::Null() );
   }
}

bool EZHazeKillInterface::Launch( const MetaProcess& P, const ProcessImplementation*, bool& dynamic, unsigned& /*flags*/ )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "EZHazeKill" );
      UpdateControls();
   }
   dynamic = false;
   return &P == TheEZHazeKillProcess;
}

ProcessImplementation* EZHazeKillInterface::NewProcess() const
{
   return new EZHazeKillInstance( m_instance );
}

bool EZHazeKillInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const EZHazeKillInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not an EZHazeKill instance.";
   return false;
}

bool EZHazeKillInterface::RequiresInstanceValidation() const
{
   return true;
}

bool EZHazeKillInterface::ImportProcess( const ProcessImplementation& p )
{
   {
      std::lock_guard<std::mutex> lock( m_settingsMutex );
      m_instance.Assign( p );
   }
   UpdateControls();
   UpdateRealTimePreview();
   return true;
}

// ----------------------------------------------------------------------------
// Real-Time Preview

bool EZHazeKillInterface::RequiresRealTimePreviewUpdate( const Image&, const View&, const Rect&, int ) const
{
   return true;
}

bool EZHazeKillInterface::GenerateRealTimePreview( Image& image, const View& view, const Rect&, int, String& info ) const
{
   // The sky of the view's full image, never of this reduced rendition: what Execute measures.
   ImageVariant full = view.Image();
   hazekill::SkyKey key;
   key.viewFullId = IsoString( view.FullId() ).c_str();
   key.width = full.Width();
   key.height = full.Height();
   key.channels = full.NumberOfNominalChannels();
   hazekill::SkyEstimate sky = m_cache.Get( key, [&full]() { return EZHazeKillInstance::MeasureImage( full ); } );
   if ( !sky.ok )
   {
      info = "EZHazeKill: " + String( sky.error.c_str() ) + " -- nothing to preview";
      return true;   // the rendition is the unchanged image
   }

   hazekill::Settings settings;
   {
      // The sliders write these on the GUI thread while renditions run on the RTP thread.
      std::lock_guard<std::mutex> lock( m_settingsMutex );
      settings = m_instance.Settings();
   }
   hazekill::Model m = hazekill::BuildModel( sky, settings );
   std::vector<float*> ch;
   for ( int c = 0, n = Min( image.NumberOfNominalChannels(), int( m.offset.size() ) ); c < n; ++c )
      ch.push_back( image.PixelData( c ) );
   hazekill::Apply( ch, image.NumberOfPixels(), m );

   info = String().Format( "EZHazeKill: sky measured on %s (%dx%d), Amount %.2f, Neutralize %.2f",
                           IsoString( view.FullId() ).c_str(), key.width, key.height, m.settings.amount,
                           m.settings.neutralize );
   return true;
}

void EZHazeKillInterface::UpdateRealTimePreview()
{
   if ( IsRealTimePreviewActive() )
      RealTimePreview::Update();
}

// ----------------------------------------------------------------------------
// Image notifications: any change to a view's image (another process, undo/redo, a crop, a
// preview redefined) drops the cached sky of its window, previews included.

bool EZHazeKillInterface::WantsImageNotifications() const
{
   return true;
}

void EZHazeKillInterface::ImageUpdated( const View& view )
{
   if ( view.IsNull() )
      return;
   m_cache.Invalidate( IsoString( view.Window().MainView().FullId() ).c_str() );
   UpdateRealTimePreview();
}

// A new view may take the id of one already measured (a window renamed, then another given its old
// id, as ImageIntegration's "integration" is): a created view drops any sky cached under its id, and a
// rename drops everything (the old id is not reported).
void EZHazeKillInterface::ImageCreated( const View& view )
{
   if ( view.IsNull() )
      return;
   m_cache.Invalidate( IsoString( view.FullId() ).c_str() );
}

void EZHazeKillInterface::ImageRenamed( const View& )
{
   m_cache.Clear();
   UpdateRealTimePreview();
}

void EZHazeKillInterface::ImageDeleted( const View& view )
{
   if ( view.IsNull() )
      return;
   // Only that view: the window's other views keep their (still valid) measurements.
   m_cache.Invalidate( IsoString( view.FullId() ).c_str() );
}

// A new screen stretch changes nothing EZHazeKill measures, but PixInsight 1.9.6 drops the current
// rendition when a view's STF changes and shows an empty Real-Time Preview until the owner asks for a
// new one (measured 2026-11-10 in the GUI smoke: AutoStretch left it empty until a slider moved).
void EZHazeKillInterface::ImageSTFEnabled( const View& )
{
   UpdateRealTimePreview();
}

void EZHazeKillInterface::ImageSTFDisabled( const View& )
{
   UpdateRealTimePreview();
}

void EZHazeKillInterface::ImageSTFUpdated( const View& )
{
   UpdateRealTimePreview();
}

// ----------------------------------------------------------------------------

void EZHazeKillInterface::UpdateControls()
{
   if ( GUI == nullptr )
      return;
   GUI->Amount_NumericControl.SetValue( m_instance.p_amount );
   GUI->Neutralize_NumericControl.SetValue( m_instance.p_neutralize );
   GUI->Target_NumericControl.SetValue( m_instance.p_targetLevel );
}

void EZHazeKillInterface::Measure()
{
   Console console;
   ImageWindow w = ImageWindow::ActiveWindow();
   if ( w.IsNull() )
   {
      console.CriticalLn( "<end><cbr>EZHazeKill: no image is open." );
      return;
   }
   View view = w.CurrentView();
   console.Show();
   console.NoteLn( "<end><cbr>== EZHazeKill : measure " + view.FullId() + " ==" );
   hazekill::Model m;
   String error;
   if ( !EZHazeKillInstance::ComputeModel( view.Image(), m_instance.Settings(), m, error ) )
   {
      console.CriticalLn( "EZHazeKill: " + error );
      return;
   }
   EZHazeKillInstance::WriteReport( m );
}

void EZHazeKillInterface::e_ValueUpdated( NumericEdit& sender, double value )
{
   {
      std::lock_guard<std::mutex> lock( m_settingsMutex );
      if ( sender == GUI->Amount_NumericControl )
         m_instance.p_amount = value;
      else if ( sender == GUI->Neutralize_NumericControl )
         m_instance.p_neutralize = value;
      else if ( sender == GUI->Target_NumericControl )
         m_instance.p_targetLevel = value;
   }
   UpdateRealTimePreview();
}

void EZHazeKillInterface::e_Click( Button& sender, bool /*checked*/ )
{
   if ( sender == GUI->Measure_PushButton )
      Measure();
}

// ----------------------------------------------------------------------------

EZHazeKillInterface::GUIData::GUIData( EZHazeKillInterface& w )
{
   const int labelWidth = w.Font().Width( String( "Neutralize:" ) + 'M' );
   const int editWidth = w.Font().Width( String( '0', 7 ) );

   Help_Label.EnableWordWrapping();
   Help_Label.EnableRichText();
   Help_Label.SetText( "Run after gradient extraction (GraXpert / DBE). Linear or stretched images. "
                       "The sky is measured on the darkest 10% of 64 px tiles of the whole image. It is never "
                       "raised, never pushed below 4 sigma of its own noise, and dark pixels roll off smoothly "
                       "instead of clipping to zero." );
   Help_Label.SetScaledMinWidth( 420 );

   const auto slider = [&]( NumericControl& c, const char* label, const MetaDouble* p, int steps, const String& tip )
   {
      c.label.SetText( label );
      c.label.SetMinWidth( labelWidth );
      c.slider.SetRange( 0, steps );
      c.slider.SetScaledMinWidth( 250 );
      c.SetReal();
      c.SetRange( p->MinimumValue(), p->MaximumValue() );
      c.SetPrecision( p->Precision() );
      c.edit.SetFixedWidth( editWidth );
      c.SetToolTip( tip );
      c.OnValueUpdated( (NumericEdit::value_event_handler)&EZHazeKillInterface::e_ValueUpdated, w );
   };
   // Labels, ranges and tooltips as EZHazeKill.js 1.1.0's dialog.
   slider( Amount_NumericControl, "Amount:", TheEZHKAmountParameter, 100,
           "<p>Fraction of the sky pedestal above the target that is removed (0 = keep it, 1 = bring the sky "
           "down to the target; default 0.5).</p>" );
   slider( Neutralize_NumericControl, "Neutralize:", TheEZHKNeutralizeParameter, 100,
           "<p>Fraction of the sky color cast that is removed (0 = keep it, 1 = neutral gray sky; "
           "default 1.0).</p>" );
   slider( Target_NumericControl, "Target:", TheEZHKTargetLevelParameter, 500,
           "<p>Lowest level the sky is brought down to (default 0.05). A sky already below it is left where "
           "it is; 4 sigma of the sky noise is used when that is higher.</p>" );

   Measure_PushButton.SetText( "Measure" );
   Measure_PushButton.SetToolTip( "<p>Report the sky, noise and cast of the active view and what Execute would "
                                  "subtract, without changing the image.</p>" );
   Measure_PushButton.OnClick( (Button::click_event_handler)&EZHazeKillInterface::e_Click, w );

   Measure_Sizer.AddStretch();
   Measure_Sizer.Add( Measure_PushButton );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Help_Label );
   Global_Sizer.Add( Amount_NumericControl );
   Global_Sizer.Add( Neutralize_NumericControl );
   Global_Sizer.Add( Target_NumericControl );
   Global_Sizer.Add( Measure_Sizer );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   w.SetFixedHeight();
}

} // namespace pcl
