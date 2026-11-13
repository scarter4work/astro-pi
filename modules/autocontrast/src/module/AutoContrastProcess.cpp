// AutoContrast -- Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AutoContrastProcess.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

AutoContrastProcess* TheAutoContrastProcess = nullptr;

AutoContrastProcess::AutoContrastProcess()
{
   TheAutoContrastProcess = this;
}

IsoString AutoContrastProcess::Id() const { return "AutoContrast"; }
IsoString AutoContrastProcess::Categories() const { return "scarter4work"; }
uint32 AutoContrastProcess::Version() const { return 0x100; }

String AutoContrastProcess::Description() const
{
   return "<html><p><b>AutoContrast</b>: automatic, reference-guided contrast refinement. "
          "This build registers the process only; it does not process images yet.</p></html>";
}

ProcessInterface* AutoContrastProcess::DefaultInterface() const { return nullptr; }

ProcessImplementation* AutoContrastProcess::Create() const
{
   return new AutoContrastInstance( this );
}

ProcessImplementation* AutoContrastProcess::Clone( const ProcessImplementation& p ) const
{
   const AutoContrastInstance* instance = dynamic_cast<const AutoContrastInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "AutoContrast: internal error - cannot clone a non-AutoContrast instance" );
   return new AutoContrastInstance( *instance );
}

bool AutoContrastProcess::CanProcessViews() const { return true; }
bool AutoContrastProcess::CanProcessGlobal() const { return false; }

AutoContrastInstance::AutoContrastInstance( const MetaProcess* m ) : ProcessImplementation( m )
{
}

bool AutoContrastInstance::CanExecuteOn( const View&, String& ) const
{
   return true;
}

bool AutoContrastInstance::ExecuteOn( View& )
{
   throw Error( "AutoContrast: not implemented in this build" );
}

bool AutoContrastInstance::CanExecuteGlobal( String& whyNot ) const
{
   whyNot = "AutoContrast is a view process; it cannot be executed globally.";
   return false;
}

bool AutoContrastInstance::ExecuteGlobal()
{
   throw Error( "AutoContrast: global execution is refused (view process)" );
}

} // namespace pcl
