//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStretchInterface.h"
#include "NukeXStretchProcess.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStretchInterface* TheNukeXStretchInterface = nullptr;

// ----------------------------------------------------------------------------

NukeXStretchInterface::NukeXStretchInterface()
   : m_instance( TheNukeXStretchProcess )
{
   TheNukeXStretchInterface = this;
}

// ----------------------------------------------------------------------------

NukeXStretchInterface::~NukeXStretchInterface()
{
}

// ----------------------------------------------------------------------------

IsoString NukeXStretchInterface::Id() const
{
   return "NukeXStretch";
}

// ----------------------------------------------------------------------------

MetaProcess* NukeXStretchInterface::Process() const
{
   return TheNukeXStretchProcess;
}

// ----------------------------------------------------------------------------

String NukeXStretchInterface::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

InterfaceFeatures NukeXStretchInterface::Features() const
{
   return InterfaceFeature::Default;
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::ApplyInstance() const
{
   m_instance.LaunchOnCurrentView();
}

// ----------------------------------------------------------------------------

void NukeXStretchInterface::ResetInstance()
{
   NukeXStretchInstance defaultInstance( TheNukeXStretchProcess );
   ImportProcess( defaultInstance );
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::Launch( const MetaProcess& P, const ProcessImplementation* p, bool& dynamic, unsigned& /*flags*/ )
{
   if ( p != nullptr )
      ImportProcess( *p );
   else
      ResetInstance();

   dynamic = false;
   return &P == TheNukeXStretchProcess;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStretchInterface::NewProcess() const
{
   return new NukeXStretchInstance( m_instance );
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const NukeXStretchInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not a NukeXStretch instance.";
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::RequiresInstanceValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInterface::ImportProcess( const ProcessImplementation& p )
{
   m_instance.Assign( p );
   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
