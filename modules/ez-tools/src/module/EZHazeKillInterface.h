// EZTools -- EZHazeKill interface with PixInsight's native Real-Time Preview.
// Copyright and licence: see src/EZToolsLicense.h.

#ifndef __EZHazeKillInterface_h
#define __EZHazeKillInterface_h

#include <pcl/Label.h>
#include <pcl/NumericControl.h>
#include <pcl/ProcessInterface.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>

#include "EZHazeKillInstance.h"
#include "hazekill/SkyCache.h"

namespace pcl
{

class EZHazeKillInterface : public ProcessInterface
{
public:
   EZHazeKillInterface();
   virtual ~EZHazeKillInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   InterfaceFeatures Features() const override;
   void ResetInstance() override;
   void RealTimePreviewUpdated( bool active ) override;
   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;
   ProcessImplementation* NewProcess() const override;
   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;
   bool ImportProcess( const ProcessImplementation& ) override;

   bool RequiresRealTimePreviewUpdate( const Image&, const View&, const Rect&, int zoomLevel ) const override;
   bool GenerateRealTimePreview( Image&, const View&, const Rect&, int zoomLevel, String& info ) const override;

   bool WantsImageNotifications() const override;
   void ImageUpdated( const View& ) override;
   void ImageDeleted( const View& ) override;
   void ImageSTFEnabled( const View& ) override;
   void ImageSTFDisabled( const View& ) override;
   void ImageSTFUpdated( const View& ) override;

private:
   EZHazeKillInstance m_instance;
   // The sky of each view's FULL image, measured once (on the RTP thread) and reused by every
   // rendition until PixInsight reports that the image changed.
   mutable eztools::hazekill::SkyCache m_cache;

   struct GUIData
   {
      GUIData( EZHazeKillInterface& );

      VerticalSizer   Global_Sizer;
         Label           Help_Label;
         NumericControl  Amount_NumericControl;
         NumericControl  Neutralize_NumericControl;
         NumericControl  Target_NumericControl;
         HorizontalSizer Measure_Sizer;
            PushButton      Measure_PushButton;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void UpdateRealTimePreview();
   void Measure();

   void e_ValueUpdated( NumericEdit& sender, double value );
   void e_Click( Button& sender, bool checked );

   friend struct GUIData;
};

PCL_BEGIN_LOCAL
extern EZHazeKillInterface* TheEZHazeKillInterface;
PCL_END_LOCAL

} // namespace pcl

#endif
