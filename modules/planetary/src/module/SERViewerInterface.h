// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __SERViewerInterface_h
#define __SERViewerInterface_h

#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Label.h>
#include <pcl/ProcessInterface.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/Slider.h>
#include <pcl/SpinBox.h>
#include <pcl/TextBox.h>

#include <memory>
#include <string>

#include "FramePreview.h"
#include "FrameSource.h"
#include "SERViewerInstance.h"

namespace pcl
{

// Open... picks a SER or AVI video; its frame is decoded and drawn in the
// embedded preview, the file facts go in the info block, and the frame slider /
// spin box step through the video. For an AVI, the Bayer pattern combo chooses
// how its raw frames are read, and Save as SER... converts it (the viewer stays
// on the AVI). Apply Global opens the selected frame as an image window
// (SERViewerInstance::ExecuteGlobal); the interface's instance never carries
// saveSerPath, so Apply Global never converts.
class SERViewerInterface : public ProcessInterface
{
public:
   SERViewerInterface();
   virtual ~SERViewerInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   InterfaceFeatures Features() const override;

   void ApplyInstance() const override;
   void ResetInstance() override;

   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;

   ProcessImplementation* NewProcess() const override;

   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;

   bool ImportProcess( const ProcessImplementation& ) override;

private:
   SERViewerInstance instance;

   // Open only while a file is shown; replaced by the next file, released on
   // any load failure and when the interface is hidden or closed.
   std::unique_ptr<planetary::ser::FrameSource> m_file;
   std::string m_fileInfo;  // the info block of m_file (UTF-8), without frame errors
   std::string m_frameInfo; // the info block as ShowFrame last set it (UTF-8)
   bool m_updatingFrameControls = false;

   struct GUIData
   {
      GUIData( SERViewerInterface& );

      VerticalSizer   Global_Sizer;
         HorizontalSizer Top_Sizer;
            PushButton      Open_PushButton;
            CheckBox        Debayer_CheckBox;
         HorizontalSizer Avi_Sizer;
            Label           Bayer_Label;
            ComboBox        Bayer_ComboBox;
            PushButton      SaveSer_PushButton;
         FramePreview    Preview_Control;
         HorizontalSizer Frame_Sizer;
            Label           Frame_Label;
            Slider          Frame_Slider;
            SpinBox         Frame_SpinBox;
         TextBox         Info_TextBox;
   };

   GUIData* GUI = nullptr;

   // Opens path and shows instance.p_frameIndex. On failure the viewer is
   // cleared and the error shown in the info block, plus a message box when
   // the user picked the file (interactive).
   void LoadFile( const String& path, bool interactive );
   // Decodes and shows frame `index` of the open file; a decode failure clears
   // the preview and is shown in the info block.
   void ShowFrame( int index );
   void CloseFile();
   void ShowError( const String& message, bool interactive );
   void SetInfo( const std::string& textUtf8 );
   void LoadFromInstance();

   // Runs f (an event handler or core callback body). PCL does not catch
   // exceptions across the API boundary into the PixInsight core, so anything
   // f throws is stopped here: the viewer is cleared and the error shown in
   // the info block and the Process Console (plus a message box when
   // interactive).
   template <class F>
   void Guarded( F&& f, bool interactive );
   void Fail( const String& message, bool interactive );
   // The frame row's range/value, and which controls are enabled: the frame
   // row for any open file, the Bayer pattern combo and Save as SER... for an
   // open AVI only.
   void UpdateFrameControls();
   bool AviLoaded() const;
   // Save as SER...: asks for the output path, converts the open AVI
   // (ConvertToSer). A failure or cancel is reported, never closes the AVI.
   void SaveAsSer();

   void e_Click( Button& sender, bool checked );
   void e_ItemSelected( ComboBox& sender, int itemIndex );
   void e_SliderValueUpdated( Slider& sender, int value );
   void e_SpinValueUpdated( SpinBox& sender, int value );
   void e_Show( Control& sender );
   void e_Hide( Control& sender );
};

extern SERViewerInterface* TheSERViewerInterface;

} // namespace pcl

#endif // __SERViewerInterface_h
