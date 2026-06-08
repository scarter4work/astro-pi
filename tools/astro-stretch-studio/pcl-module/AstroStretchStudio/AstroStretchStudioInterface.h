// ----------------------------------------------------------------------------
// AstroStretchStudio Interface Header
// ----------------------------------------------------------------------------

#ifndef __AstroStretchStudioInterface_h
#define __AstroStretchStudioInterface_h

#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/NumericControl.h>
#include <pcl/ComboBox.h>
#include <pcl/CheckBox.h>
#include <pcl/PushButton.h>
#include <pcl/ToolButton.h>
#include <pcl/GroupBox.h>
#include <pcl/SectionBar.h>
#include <pcl/Timer.h>
#include <pcl/Thread.h>
#include <pcl/ScrollBox.h>
#include <pcl/ImageWindow.h>

#include "AstroStretchStudioInstance.h"
#include "AstroStretchStudioZoneHDR.h"

namespace pcl
{

// ----------------------------------------------------------------------------

class AstroStretchStudioInterface : public ProcessInterface
{
public:

   AstroStretchStudioInterface();
   virtual ~AstroStretchStudioInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   IsoString IconImageSVG() const override;
   InterfaceFeatures Features() const override;
   void ApplyInstance() const override;
   void ResetInstance() override;
   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;
   ProcessImplementation* NewProcess() const override;
   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;
   bool ImportProcess( const ProcessImplementation& ) override;

   // Real-time preview support
   void RealTimePreviewUpdated( bool active ) override;
   bool RequiresRealTimePreviewUpdate( const UInt16Image&, const View&, const Rect&, int zoomLevel ) const override;
   bool GenerateRealTimePreview( UInt16Image&, const View&, const Rect&, int zoomLevel, String& info ) const override;

private:

   AstroStretchStudioInstance m_instance;

   // Real-time preview thread
   class RealTimeThread : public Thread
   {
   public:
      RealTimeThread();
      void Reset( const UInt16Image&, const AstroStretchStudioInstance& );
      void Run() override;
      UInt16Image m_image;

   private:
      AstroStretchStudioInstance m_instance;
   };

   mutable RealTimeThread* m_realTimeThread = nullptr;

   // UI Elements
   struct GUIData
   {
      GUIData( AstroStretchStudioInterface& );

      VerticalSizer     Global_Sizer;

      // Algorithm selection
      HorizontalSizer   Algorithm_Sizer;
         Label          Algorithm_Label;
         ComboBox       Algorithm_ComboBox;

      // Algorithm parameters container
      Control           AlgorithmParams_Control;
      VerticalSizer     AlgorithmParams_Sizer;

      // OTS Panel
      Control           OTS_Control;
      VerticalSizer     OTS_Sizer;
         HorizontalSizer   OTS_ObjectType_Sizer;
            Label          OTS_ObjectType_Label;
            ComboBox       OTS_ObjectType_ComboBox;
         NumericControl    OTS_BackgroundTarget_NumericControl;
         NumericControl    OTS_StretchIntensity_NumericControl;
         NumericControl    OTS_ProtectHighlights_NumericControl;
         HorizontalSizer   OTS_PreserveColor_Sizer;
            CheckBox       OTS_PreserveColor_CheckBox;

      // SAS Panel
      Control           SAS_Control;
      VerticalSizer     SAS_Sizer;
         NumericControl    SAS_NumScales_NumericControl;
         NumericControl    SAS_BackgroundTarget_NumericControl;
         NumericControl    SAS_FineScaleGain_NumericControl;
         NumericControl    SAS_MidScaleGain_NumericControl;
         NumericControl    SAS_CoarseScaleGain_NumericControl;
         NumericControl    SAS_CompressionAlpha_NumericControl;
         NumericControl    SAS_HighlightProtection_NumericControl;
         NumericControl    SAS_NoiseThreshold_NumericControl;
         HorizontalSizer   SAS_Checkboxes_Sizer;
            CheckBox       SAS_FlattenBackground_CheckBox;
            CheckBox       SAS_PreserveColor_CheckBox;

      // Zone HDR Section
      SectionBar        ZoneHDR_SectionBar;
      Control           ZoneHDR_Control;
      VerticalSizer     ZoneHDR_Sizer;
         HorizontalSizer   ZoneHDR_Enable_Sizer;
            CheckBox       ZoneHDR_Enable_CheckBox;
            PushButton     ZoneHDR_DetectZones_Button;
         HorizontalSizer   ZoneHDR_ImageSelect_Sizer;
            Label          ZoneHDR_ImageSelect_Label;
            ComboBox       ZoneHDR_ImageSelect_ComboBox;
            ToolButton     ZoneHDR_RefreshImages_Button;
         HorizontalSizer   ZoneHDR_PreviewMode_Sizer;
            Label          ZoneHDR_PreviewMode_Label;
            ComboBox       ZoneHDR_PreviewMode_ComboBox;
         // Dynamic zone controls container
         ScrollBox         ZoneHDR_Zones_ScrollBox;
         Control           ZoneHDR_Zones_Control;
         VerticalSizer     ZoneHDR_Zones_Sizer;

      // Per-zone control structure
      struct ZoneControls
      {
         Control        Zone_Control;
         VerticalSizer  Zone_Sizer;
         HorizontalSizer Zone_Header_Sizer;
            Label       Zone_Name_Label;
            ToolButton  Zone_Solo_Button;
         NumericControl  Zone_Intensity_NumericControl;
         NumericControl  Zone_Saturation_NumericControl;
      };

      static constexpr int MaxZones = 8;
      ZoneControls      ZoneControls_Array[MaxZones];
      int               ActiveZoneCount = 0;

      // Timer for debounced real-time preview updates
      Timer UpdateRealTimePreview_Timer;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void UpdateAlgorithmPanel();
   void UpdateRealTimePreview();
   void UpdateZoneControls( int zoneCount );
   void UpdateZoneControlsWithNames( const Array<Zone>& zones );
   void UpdateImageList();
   void DetectZonesFromCurrentView();

   // Event handlers
   void e_ItemSelected( ComboBox& sender, int itemIndex );
   void e_ValueUpdated( NumericEdit& sender, double value );
   void e_Click( Button& sender, bool checked );
   void e_SectionToggle( SectionBar& sender, Control& section, bool start );
   void e_UpdateRealTimePreview_Timer( Timer& sender );
   void e_ZoneValueUpdated( NumericEdit& sender, double value );
   void e_ZoneSoloClick( Button& sender, bool checked );

   friend struct GUIData;
};

// ----------------------------------------------------------------------------

PCL_BEGIN_LOCAL
extern AstroStretchStudioInterface* TheAstroStretchStudioInterface;
PCL_END_LOCAL

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __AstroStretchStudioInterface_h

// ----------------------------------------------------------------------------
