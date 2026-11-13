// AutoContrast -- Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "AutoContrastReferencesProcess.h"

#include <pcl/ErrorHandler.h>

namespace pcl
{

AutoContrastReferencesProcess* TheAutoContrastReferencesProcess = nullptr;

AutoContrastReferencesProcess::AutoContrastReferencesProcess()
{
   TheAutoContrastReferencesProcess = this;
}

IsoString AutoContrastReferencesProcess::Id() const { return "AutoContrastReferences"; }
IsoString AutoContrastReferencesProcess::Categories() const { return "scarter4work"; }
uint32 AutoContrastReferencesProcess::Version() const { return 0x100; }

String AutoContrastReferencesProcess::Description() const
{
   return "<html><p><b>AutoContrastReferences</b>: manages the reference library used by AutoContrast. "
          "This build registers the process only; it does nothing yet.</p></html>";
}

ProcessInterface* AutoContrastReferencesProcess::DefaultInterface() const { return nullptr; }

ProcessImplementation* AutoContrastReferencesProcess::Create() const
{
   return new AutoContrastReferencesInstance( this );
}

ProcessImplementation* AutoContrastReferencesProcess::Clone( const ProcessImplementation& p ) const
{
   const AutoContrastReferencesInstance* instance = dynamic_cast<const AutoContrastReferencesInstance*>( &p );
   if ( instance == nullptr )
      throw Error( "AutoContrastReferences: internal error - cannot clone a non-AutoContrastReferences instance" );
   return new AutoContrastReferencesInstance( *instance );
}

bool AutoContrastReferencesProcess::CanProcessViews() const { return false; }
bool AutoContrastReferencesProcess::CanProcessGlobal() const { return true; }

AutoContrastReferencesInstance::AutoContrastReferencesInstance( const MetaProcess* m ) : ProcessImplementation( m )
{
}

bool AutoContrastReferencesInstance::CanExecuteGlobal( String& ) const
{
   return true;
}

bool AutoContrastReferencesInstance::ExecuteGlobal()
{
   throw Error( "AutoContrastReferences: not implemented in this build" );
}

} // namespace pcl
