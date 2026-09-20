// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotProcess.h"
#include "PICopilotInstance.h"
#include "PICopilotInterface.h"
#include "PICopilotVersion.h"
#include <pcl/ErrorHandler.h>

namespace pcl
{

PICopilotProcess* ThePICopilotProcess = nullptr;

PICopilotProcess::PICopilotProcess()
{
   ThePICopilotProcess = this;
}

IsoString PICopilotProcess::Id() const
{
   return "PICopilot";
}

IsoString PICopilotProcess::Categories() const
{
   return "Utilities";
}

uint32 PICopilotProcess::Version() const
{
   return 0x100;  // Version 1.0.0
}

String PICopilotProcess::Description() const
{
   return
      "<html>"
      "<p><b>PI Copilot v" PICOPILOT_STR(PICOPILOT_MODULE_VERSION_MAJOR) "</b> "
      "&mdash; In-app AI assistant for PixInsight.</p>"
      "<p>Increment 1: process/interface registration skeleton. The chat "
      "UI and assistant backend land in later increments.</p>"
      "</html>";
}

ProcessInterface* PICopilotProcess::DefaultInterface() const
{
   return ThePICopilotInterface;
}

ProcessImplementation* PICopilotProcess::Create() const
{
   return new PICopilotInstance( this );
}

ProcessImplementation* PICopilotProcess::Clone( const ProcessImplementation& p ) const
{
   const PICopilotInstance* instance = dynamic_cast<const PICopilotInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "PICopilot: Internal error - cannot clone non-PICopilot instance" );
   return new PICopilotInstance( *instance );
}

bool PICopilotProcess::CanProcessViews() const
{
   return false;  // Global-only utility process
}

bool PICopilotProcess::CanProcessGlobal() const
{
   return true;
}

bool PICopilotProcess::NeedsValidation() const
{
   return false;
}

} // namespace pcl
