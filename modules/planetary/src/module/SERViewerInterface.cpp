// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "SERViewerInterface.h"
#include "SERViewerProcess.h"

namespace pcl
{

SERViewerInterface* TheSERViewerInterface = nullptr;

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
   return true;
}

SERViewerInterface::GUIData::GUIData( SERViewerInterface& w )
{
   w.SetSizer( Global_Sizer );
   w.SetMinWidth( 300 );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
}

} // namespace pcl
