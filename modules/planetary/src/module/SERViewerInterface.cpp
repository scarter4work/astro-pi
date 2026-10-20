// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SERViewerInterface.h"
#include "SERViewerProcess.h"

#include <pcl/Console.h>
#include <pcl/File.h>
#include <pcl/FileDialog.h>
#include <pcl/MessageBox.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <exception>

#include "SerInfoText.h"

namespace ser = planetary::ser;

namespace pcl
{

SERViewerInterface* TheSERViewerInterface = nullptr;

namespace
{

const char* const kIdleText = "No SER file loaded.\nClick Open SER... to choose a .ser video.";

String FromUtf8( const std::string& s )
{
   return String::UTF8ToUTF16( s.c_str() );
}

std::string ToUtf8( const String& s )
{
   return s.ToUTF8().c_str();
}

} // namespace

SERViewerInterface::SERViewerInterface()
   : instance( TheSERViewerProcess )
{
   TheSERViewerInterface = this;
}

SERViewerInterface::~SERViewerInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

IsoString SERViewerInterface::Id() const
{
   return "SERViewer";
}

MetaProcess* SERViewerInterface::Process() const
{
   return TheSERViewerProcess;
}

InterfaceFeatures SERViewerInterface::Features() const
{
   return InterfaceFeature::DefaultGlobal;
}

void SERViewerInterface::ApplyInstance() const
{
   instance.LaunchGlobal();
}

void SERViewerInterface::ResetInstance()
{
   SERViewerInstance defaultInstance( TheSERViewerProcess );
   ImportProcess( defaultInstance );
}

bool SERViewerInterface::Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "SERViewer" );
      OnShow( (Control::event_handler)&SERViewerInterface::e_Show, *this );
      OnHide( (Control::event_handler)&SERViewerInterface::e_Hide, *this );
      UpdateFrameControls();
   }

   dynamic = false;
   return true;
}

ProcessImplementation* SERViewerInterface::NewProcess() const
{
   return new SERViewerInstance( instance );
}

bool SERViewerInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const SERViewerInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Must be an instance of SERViewer.";
   return false;
}

bool SERViewerInterface::RequiresInstanceValidation() const
{
   return true;
}

bool SERViewerInterface::ImportProcess( const ProcessImplementation& p )
{
   instance.Assign( p );
   if ( GUI != nullptr )
      Guarded( [this]()
      {
         GUI->Debayer_CheckBox.SetChecked( instance.p_debayer );
         // A hidden interface holds no file: e_Show loads it when it appears.
         if ( IsVisible() )
            LoadFromInstance();
         else
            CloseFile();
      }, false/*interactive*/ );
   return true;
}

template <class F>
void SERViewerInterface::Guarded( F&& f, bool interactive )
{
   try
   {
      f();
   }
   catch ( const Exception& e )
   {
      Fail( e.Message(), interactive );
   }
   catch ( ... )
   {
      Fail( FromUtf8( planetary::preview::DescribeException( std::current_exception() ) ), interactive );
   }
}

void SERViewerInterface::Fail( const String& message, bool interactive )
{
   // Each step on its own, so a failure in one never skips the others; a step
   // that fails is noted on stderr (nothing may cross into the core).
   auto attempt = [&message]( const char* step, auto&& f )
   {
      try
      {
         f();
      }
      catch ( ... )
      {
         std::fprintf( stderr, "SERViewer: %s failed while reporting: %s\n", step, ToUtf8( message ).c_str() );
      }
   };
   attempt( "closing the file", [this]() { CloseFile(); } );
   attempt( "showing the error", [this, &message, interactive]() { ShowError( message, interactive ); } );
   attempt( "writing the console", [&message]()
   {
      Console().CriticalLn( "<end><cbr>*** SERViewer: " + message.EncodedHTMLSpecialChars() );
   } );
}

void SERViewerInterface::SetInfo( const std::string& textUtf8 )
{
   GUI->Info_TextBox.SetPlainText( FromUtf8( planetary::preview::RawSafe( textUtf8 ) ) );
}

void SERViewerInterface::LoadFromInstance()
{
   if ( instance.p_filePath.Trimmed().IsEmpty() )
   {
      CloseFile();
      SetInfo( kIdleText );
   }
   else
      LoadFile( instance.p_filePath, false/*interactive*/ );
}

void SERViewerInterface::LoadFile( const String& path, bool interactive )
{
   CloseFile();
   const String trimmed = path.Trimmed();
   const String fileName = File::ExtractNameAndExtension( trimmed );
   try
   {
      m_file = std::make_unique<ser::SerFile>( ser::SerFile::Open( ToUtf8( trimmed ) ) );
      const ser::FileInfo& info = m_file->Info();
      if ( info.frameCount > INT_MAX )
         throw ser::SerError( std::to_string( info.frameCount ) + " frames: more than the frame slider can address" );
      if ( instance.p_frameIndex < 0 || instance.p_frameIndex >= info.frameCount )
         throw ser::SerError( "frame index " + std::to_string( instance.p_frameIndex ) + " out of range: the file has " +
                              std::to_string( info.frameCount ) + " frames" );
      m_fileInfo = planetary::preview::InfoText( ToUtf8( fileName ), info, m_file->FrameTimestamp( 0 ) );
   }
   catch ( const ser::SerError& e )
   {
      CloseFile();
      ShowError( fileName + ": " + FromUtf8( e.what() ) + "\nPath: " + trimmed, interactive );
      return;
   }
   UpdateFrameControls();
   ShowFrame( instance.p_frameIndex );
}

void SERViewerInterface::ShowFrame( int index )
{
   if ( !m_file )
      return;
   instance.p_frameIndex = index;
   UpdateFrameControls();
   String error;
   try
   {
      GUI->Preview_Control.SetBitmap( MakePreviewBitmap( LoadFrameForDisplay( *m_file, index, instance.p_debayer ) ) );
      SetInfo( m_fileInfo );
      return;
   }
   catch ( const ser::SerError& e )
   {
      error = FromUtf8( e.what() );
   }
   catch ( const Error& e )
   {
      error = e.Message();
   }
   GUI->Preview_Control.Clear();
   SetInfo( m_fileInfo + "\n\nError: frame " + std::to_string( index ) + ": " + ToUtf8( error ) );
}

void SERViewerInterface::CloseFile()
{
   m_file.reset();
   m_fileInfo.clear();
   if ( GUI != nullptr )
   {
      GUI->Preview_Control.Clear();
      UpdateFrameControls();
   }
}

void SERViewerInterface::ShowError( const String& message, bool interactive )
{
   SetInfo( "Error: " + ToUtf8( message ) );
   if ( interactive )
      MessageBox( message, "SERViewer", StdIcon::Error, StdButton::Ok ).Execute();
}

void SERViewerInterface::UpdateFrameControls()
{
   if ( GUI == nullptr )
      return;
   const bool loaded = bool( m_file );
   const int last = loaded ? int( m_file->Info().frameCount - 1 ) : 0;
   const int value = loaded ? std::clamp( int( instance.p_frameIndex ), 0, last ) : 0;
   m_updatingFrameControls = true;
   GUI->Frame_Slider.SetRange( 0, last );
   GUI->Frame_Slider.SetPageSize( std::max( 1, (last + 1)/20 ) );
   GUI->Frame_Slider.SetValue( value );
   GUI->Frame_SpinBox.SetRange( 0, last );
   GUI->Frame_SpinBox.SetValue( value );
   m_updatingFrameControls = false;
   GUI->Frame_Label.Enable( loaded );
   GUI->Frame_Slider.Enable( loaded );
   GUI->Frame_SpinBox.Enable( loaded );
}

void SERViewerInterface::e_Click( Button& sender, bool checked )
{
   if ( sender == GUI->Open_PushButton )
      Guarded( [this]()
      {
         OpenFileDialog d;
         d.SetCaption( "Open SER video" );
         d.SetFilter( FileFilter( "SER video files", ".ser" ) );
         d.DisableMultipleSelections();
         const String current = instance.p_filePath.Trimmed();
         if ( !current.IsEmpty() )
            d.SetInitialPath( File::ExtractDrive( current ) + File::ExtractDirectory( current ) );
         if ( d.Execute() )
         {
            instance.p_filePath = d.FileName();
            instance.p_frameIndex = 0;
            LoadFile( instance.p_filePath, true/*interactive*/ );
         }
      }, true/*interactive*/ );
   else if ( sender == GUI->Debayer_CheckBox )
      Guarded( [this, checked]()
      {
         instance.p_debayer = checked;
         if ( m_file )
            ShowFrame( instance.p_frameIndex );
      }, true/*interactive*/ );
}

void SERViewerInterface::e_SliderValueUpdated( Slider& sender, int value )
{
   if ( !m_updatingFrameControls && m_file && value != instance.p_frameIndex )
      Guarded( [this, value]() { ShowFrame( value ); }, false/*interactive*/ );
}

void SERViewerInterface::e_SpinValueUpdated( SpinBox& sender, int value )
{
   if ( !m_updatingFrameControls && m_file && value != instance.p_frameIndex )
      Guarded( [this, value]() { ShowFrame( value ); }, false/*interactive*/ );
}

void SERViewerInterface::e_Show( Control& )
{
   if ( !m_file )
      Guarded( [this]() { LoadFromInstance(); }, false/*interactive*/ );
}

void SERViewerInterface::e_Hide( Control& )
{
   // Never hold a SER file open behind a closed interface.
   Guarded( [this]() { CloseFile(); }, false/*interactive*/ );
}

SERViewerInterface::GUIData::GUIData( SERViewerInterface& w )
{
   Open_PushButton.SetText( "Open SER..." );
   Open_PushButton.SetToolTip( "<p>Choose a SER video (.ser). Its first frame is shown in the preview.</p>" );
   Open_PushButton.OnClick( (Button::click_event_handler)&SERViewerInterface::e_Click, w );

   Debayer_CheckBox.SetText( "Debayer" );
   Debayer_CheckBox.SetToolTip( "<p>Bayer files: show and open frames as RGB (bilinear demosaic). "
                                "Unchecked: the raw CFA frame as one channel.</p>" );
   Debayer_CheckBox.SetChecked( w.instance.p_debayer );
   Debayer_CheckBox.OnClick( (Button::click_event_handler)&SERViewerInterface::e_Click, w );

   Top_Sizer.SetSpacing( 6 );
   Top_Sizer.Add( Open_PushButton );
   Top_Sizer.AddStretch();
   Top_Sizer.Add( Debayer_CheckBox );

   Preview_Control.SetScaledMinSize( 400, 300 );

   Frame_Label.SetText( "Frame:" );
   Frame_Label.SetTextAlignment( TextAlign::Right|TextAlign::VertCenter );

   Frame_Slider.SetScaledMinWidth( 200 );
   Frame_Slider.SetRange( 0, 0 );
   Frame_Slider.SetStepSize( 1 );
   Frame_Slider.EnableTracking();
   Frame_Slider.SetFocusStyle( FocusStyle::Click|FocusStyle::Tab );
   Frame_Slider.SetToolTip( "<p>Frame shown in the preview and opened by Apply Global (first frame = 0).</p>" );
   Frame_Slider.OnValueUpdated( (Slider::value_event_handler)&SERViewerInterface::e_SliderValueUpdated, w );

   Frame_SpinBox.SetRange( 0, 0 );
   Frame_SpinBox.SetScaledMinWidth( 80 );
   Frame_SpinBox.SetToolTip( "<p>Frame shown in the preview and opened by Apply Global (first frame = 0).</p>" );
   Frame_SpinBox.OnValueUpdated( (SpinBox::value_event_handler)&SERViewerInterface::e_SpinValueUpdated, w );

   Frame_Sizer.SetSpacing( 4 );
   Frame_Sizer.Add( Frame_Label );
   Frame_Sizer.Add( Frame_Slider, 100 );
   Frame_Sizer.Add( Frame_SpinBox );

   Info_TextBox.SetReadOnly();
   Info_TextBox.SetScaledMinSize( 400, 180 );
   Info_TextBox.SetPlainText( String( kIdleText ) );

   // The GUI smoke (test/gui_drive.py) clicks relative to the preview: Open
   // SER... directly above its left end, Debayer above its right end, the
   // frame slider directly below; 6 px apart.
   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Top_Sizer );
   Global_Sizer.Add( Preview_Control, 100 );
   Global_Sizer.Add( Frame_Sizer );
   Global_Sizer.Add( Info_TextBox );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
}

} // namespace pcl
