// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PlanetaryVersion.h"
#include "SERViewerInterface.h"
#include "SERViewerParameters.h"
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
#include "VideoControls.h"

namespace ser = planetary::ser;

namespace pcl
{

SERViewerInterface* TheSERViewerInterface = nullptr;

namespace
{

const char* const kIdleText = "No video loaded.\nClick Open... to choose a .ser or .avi video.";

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
   // The interface converts only through Save as SER...: an imported saveSerPath
   // would turn Apply Global into a conversion.
   instance.p_saveSerPath.Clear();
   if ( GUI != nullptr )
      Guarded( [this]()
      {
         GUI->Debayer_CheckBox.SetChecked( instance.p_debayer );
         // An out-of-range value is reported by LoadFromInstance (BayerChoiceFor).
         if ( instance.p_bayerPattern >= 0 && instance.p_bayerPattern < SVBayerPattern::NumberOfItems )
            GUI->Bayer_ComboBox.SetCurrentItem( instance.p_bayerPattern );
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
   GuardedCall( f, [this, interactive]( const String& message ) { Fail( message, interactive ); } );
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
      m_file = ser::OpenVideo( ToUtf8( trimmed ), BayerChoiceFor( instance.p_bayerPattern, "SERViewer" ) );
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
      const DisplayFrame shown = LoadFrameForDisplay( *m_file, index, instance.p_debayer );
      GUI->Preview_Control.SetBitmap( MakePreviewBitmap( shown.image ) );
      m_frameInfo = planetary::preview::FrameInfoText( m_fileInfo, shown.rangeWarning );
      SetInfo( m_frameInfo );
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
   m_frameInfo = m_fileInfo + "\n\nError: frame " + std::to_string( index ) + ": " + ToUtf8( error );
   SetInfo( m_frameInfo );
}

void SERViewerInterface::CloseFile()
{
   m_file.reset();
   m_fileInfo.clear();
   m_frameInfo.clear();
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
   // A SER carries its own ColorID: the Bayer pattern and Save as SER are AVI-only.
   const bool avi = AviLoaded();
   GUI->Bayer_Label.Enable( avi );
   GUI->Bayer_ComboBox.Enable( avi );
   GUI->SaveSer_PushButton.Enable( avi );
   // Save pattern... only where there is no sidecar to read (never one to
   // overwrite), and only once a pattern is chosen (Auto and None have none to
   // save) with Debayer on: the colours that confirm it show only debayered.
   GUI->SavePattern_PushButton.SetVisible( AviWithoutSidecar() );
   GUI->SavePattern_PushButton.Enable( AviWithoutSidecar() &&
                                       instance.p_bayerPattern >= SVBayerPattern::RGGB &&
                                       instance.p_bayerPattern <= SVBayerPattern::BGGR && instance.p_debayer );
}

bool SERViewerInterface::AviLoaded() const
{
   return m_file && m_file->Info().container != ser::Container::Ser;
}

bool SERViewerInterface::AviWithoutSidecar() const
{
   return m_file && pcl::AviWithoutSidecar( m_file->Info() );
}

void SERViewerInterface::SaveBayerPattern()
{
   if ( !AviWithoutSidecar() || !ser::IsBayer( m_file->Info().colorId ) || !instance.p_debayer )
      return;
   String failure;
   switch ( pcl::SaveBayerPattern( instance.p_filePath, m_file->Info().colorId, "SERViewer", failure ) )
   {
   case SavePatternResult::Declined:
      return;
   case SavePatternResult::Failed:
      // The viewer stays on the AVI: the failure is about the sidecar, not the video.
      if ( m_file )
         SetInfo( m_frameInfo + "\n\nSave pattern failed: " + ToUtf8( failure ) );
      return;
   case SavePatternResult::Written:
      break;
   }

   // Reopened with Auto on the same frame: the new sidecar is read back, the
   // info block names it as the source, and the button goes (a sidecar exists).
   instance.p_bayerPattern = SVBayerPattern::Auto;
   GUI->Bayer_ComboBox.SetCurrentItem( SVBayerPattern::Auto );
   LoadFile( instance.p_filePath, true/*interactive*/ );
}

void SERViewerInterface::SaveAsSer()
{
   if ( !AviLoaded() )
      return;
   const String source = instance.p_filePath.Trimmed();
   SaveFileDialog d;
   d.SetCaption( "Save as SER" );
   d.SetFilter( FileFilter( "SER video", ".ser" ) );
   d.SetInitialPath( File::ChangeExtension( source, ".ser" ) );
   d.EnableOverwritePrompt();
   if ( !d.Execute() )
      return;
   String savePath = d.FileName();
   // The dialog adds .ser to a name typed without an extension (measured on PI
   // 1.9.5 by the GUI smoke); nothing here relies on that. A name the dialog
   // returns without one gets .ser, and since the dialog's overwrite prompt was
   // about the other name, an existing file under the new one is asked about here.
   if ( File::ExtractExtension( savePath ).IsEmpty() )
   {
      savePath += ".ser";
      if ( File::Exists( savePath ) )
         if ( MessageBox( savePath + " already exists. Replace it?", "SERViewer", StdIcon::Warning,
                          StdButton::No, StdButton::Yes ).Execute() != StdButton::Yes )
            return;
   }

   // The export reads through its own reader (same file, same Bayer choice): the
   // progress dialog processes events between frames, and nothing done to the
   // viewer meanwhile (a frame change, a closed interface releasing m_file) may
   // touch the reader being exported. The dialog has asked about an existing
   // file, so ExportSer may replace it (atomically: the old file stays until the
   // new one is complete). Progress goes in a modal dialog whose Cancel stops it:
   // the console's Abort works only inside process execution (SaveProgress).
   String failure;
   try
   {
      std::unique_ptr<ser::FrameSource> reader;
      try
      {
         reader = ser::OpenVideo( ToUtf8( source ), BayerChoiceFor( instance.p_bayerPattern, "SERViewer" ) );
      }
      catch ( const ser::SerError& e )
      {
         throw Error( "SERViewer: " + File::ExtractNameAndExtension( source ) + ": " + FromUtf8( e.what() ) );
      }
      const ser::ExportResult result = ConvertToSer( *reader, source, savePath, true/*allowOverwrite*/,
                                                     SaveProgress::Dialog );
      const std::string note = "Saved as SER: " + ToUtf8( savePath ) + " (" + std::to_string( result.framesWritten ) +
                               " frames, " + ser::ColorIdName( reader->Info().colorId ) + ")";
      if ( m_file )
         SetInfo( m_frameInfo + "\n\n" + note );
      return;
   }
   catch ( const ProcessAborted& )
   {
      failure = "Save as SER cancelled; " + savePath + " was not written";
   }
   catch ( const Exception& e )
   {
      failure = e.Message();
   }
   catch ( ... )
   {
      failure = "SERViewer: Save as SER: " +
                FromUtf8( planetary::preview::DescribeException( std::current_exception() ) );
   }
   // The viewer stays on the AVI: the failure is about the output, not the source.
   Console().CriticalLn( "<end><cbr>*** " + failure.EncodedHTMLSpecialChars() );
   if ( m_file )
      SetInfo( m_frameInfo + "\n\nSave as SER failed: " + ToUtf8( failure ) );
   MessageBox( failure, "SERViewer", StdIcon::Error, StdButton::Ok ).Execute();
}

void SERViewerInterface::e_ItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->Bayer_ComboBox && itemIndex != instance.p_bayerPattern )
      Guarded( [this, itemIndex]()
      {
         instance.p_bayerPattern = pcl_enum( itemIndex );
         // Reopened with the new pattern, on the same frame.
         if ( AviLoaded() )
            LoadFile( instance.p_filePath, true/*interactive*/ );
      }, true/*interactive*/ );
}

void SERViewerInterface::e_Click( Button& sender, bool checked )
{
   if ( sender == GUI->Open_PushButton )
      Guarded( [this]()
      {
         String path;
         if ( AskForVideo( instance.p_filePath, path ) )
         {
            // A new file starts on frame 0 with Bayer pattern Auto (its own sidecar
            // decides); an imported instance keeps its bayerPattern (ImportProcess).
            instance.p_filePath = path;
            instance.p_frameIndex = 0;
            instance.p_bayerPattern = SVBayerPattern::Auto;
            GUI->Bayer_ComboBox.SetCurrentItem( SVBayerPattern::Auto );
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
   else if ( sender == GUI->SaveSer_PushButton )
      Guarded( [this]() { SaveAsSer(); }, true/*interactive*/ );
   else if ( sender == GUI->SavePattern_PushButton )
      Guarded( [this]() { SaveBayerPattern(); }, true/*interactive*/ );
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
   // Never hold a video file open behind a closed interface.
   Guarded( [this]() { CloseFile(); }, false/*interactive*/ );
}

SERViewerInterface::GUIData::GUIData( SERViewerInterface& w )
{
   Open_PushButton.SetText( "Open..." );
   Open_PushButton.SetToolTip( "<p>Choose a planetary video: SER (.ser) or uncompressed 8-bit greyscale or raw Bayer AVI (.avi). "
                               "Its first frame is shown in the preview.</p>" );
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

   Bayer_Label.SetText( "Bayer pattern:" );
   Bayer_Label.SetTextAlignment( TextAlign::Left|TextAlign::VertCenter );

   // Item index == SVBayerPattern value.
   SetUpBayerCombo( Bayer_ComboBox );
   Bayer_ComboBox.SetCurrentItem( w.instance.p_bayerPattern );
   Bayer_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&SERViewerInterface::e_ItemSelected, w );

   SaveSer_PushButton.SetText( "Save as SER..." );
   SaveSer_PushButton.SetToolTip( "<p>AVI only: write the whole video as a SER file (8-bit, the Bayer pattern "
                                  "above as its colour layout). A progress dialog shows how far it is; its Cancel "
                                  "button stops it and removes the partial file. The viewer stays on the AVI.</p>" );
   SaveSer_PushButton.OnClick( (Button::click_event_handler)&SERViewerInterface::e_Click, w );

   SavePattern_PushButton.SetText( "Save pattern..." );
   SavePattern_PushButton.SetToolTip( "<p>AVI without a sidecar only: remember the Bayer pattern chosen above for this "
                                      "video. It writes a small text file next to the video (&lt;name&gt;.avi.txt, "
                                      "the sidecar Auto reads), so Auto uses this pattern whenever the file is opened. "
                                      "Enabled once a pattern is chosen and Debayer is on: the colours of the "
                                      "debayered preview are how you tell the pattern is right (the raw CFA shows "
                                      "none). Asks first; never replaces an existing sidecar.</p>" );
   SavePattern_PushButton.OnClick( (Button::click_event_handler)&SERViewerInterface::e_Click, w );

   Avi_Sizer.SetSpacing( 6 );
   Avi_Sizer.Add( Bayer_Label );
   Avi_Sizer.Add( Bayer_ComboBox );
   Avi_Sizer.Add( SavePattern_PushButton );
   Avi_Sizer.AddStretch();
   Avi_Sizer.Add( SaveSer_PushButton );

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

   // The GUI smoke (test/gui_drive.py controls()) clicks relative to the
   // preview: two rows above it (Open... / Debayer, then Bayer pattern / Save
   // as SER..., at its left / right ends; Save pattern..., when shown, right of
   // the combo), the frame slider directly below;
   // 6 px apart. A layout change here means re-measuring those offsets.
   Global_Sizer.SetMargin( 8 );
   Global_Sizer.SetSpacing( 6 );
   Global_Sizer.Add( Top_Sizer );
   Global_Sizer.Add( Avi_Sizer );
   Global_Sizer.Add( Preview_Control, 100 );
   Global_Sizer.Add( Frame_Sizer );
   Global_Sizer.Add( Info_TextBox );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
   // Laid out with Save pattern... shown, so the Bayer row keeps room for it:
   // shown later in a narrower window it would squeeze every button on the
   // row (measured: "ave pattern." / "ave as SER."). Hidden until an AVI with
   // no sidecar is open (UpdateFrameControls).
   w.SetMinWidth( w.Width() );
   SavePattern_PushButton.Hide();
}

} // namespace pcl
