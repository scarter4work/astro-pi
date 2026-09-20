// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotInterface.h"
#include "PICopilotProcess.h"

namespace pcl
{

PICopilotInterface* ThePICopilotInterface = nullptr;

PICopilotInterface::PICopilotInterface()
{
   ThePICopilotInterface = this;
}

PICopilotInterface::~PICopilotInterface()
{
   if ( GUI != nullptr )
      delete GUI, GUI = nullptr;
}

IsoString PICopilotInterface::Id() const
{
   return "PICopilot";
}

MetaProcess* PICopilotInterface::Process() const
{
   return ThePICopilotProcess;
}

InterfaceFeatures PICopilotInterface::Features() const
{
   return InterfaceFeature::None;
}

bool PICopilotInterface::IsInstanceGenerator() const
{
   return false;
}

bool PICopilotInterface::Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& )
{
   if ( GUI == nullptr )
   {
      GUI = new GUIData( *this );
      SetWindowTitle( "PI Copilot" );
   }

   dynamic = false;
   return true;
}

// ── GUI Construction ─────────────────────────────────────────────

PICopilotInterface::GUIData::GUIData( PICopilotInterface& w )
{
   Placeholder_Label.SetText( "PI Copilot" );
   Placeholder_Label.SetTextAlignment( TextAlign::Center | TextAlign::VertCenter );

   Global_Sizer.SetMargin( 8 );
   Global_Sizer.Add( Placeholder_Label );

   w.SetSizer( Global_Sizer );
   w.EnsureLayoutUpdated();
   w.AdjustToContents();
}

} // namespace pcl
