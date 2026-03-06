//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStackInterface.h"
#include "NukeXStackProcess.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStackInterface* TheNukeXStackInterface = nullptr;

// ----------------------------------------------------------------------------

NukeXStackInterface::NukeXStackInterface()
   : m_instance( TheNukeXStackProcess )
{
   TheNukeXStackInterface = this;
}

// ----------------------------------------------------------------------------

NukeXStackInterface::~NukeXStackInterface()
{
}

// ----------------------------------------------------------------------------

IsoString NukeXStackInterface::Id() const
{
   return "NukeXStack";
}

// ----------------------------------------------------------------------------

MetaProcess* NukeXStackInterface::Process() const
{
   return TheNukeXStackProcess;
}

// ----------------------------------------------------------------------------

String NukeXStackInterface::IconImageSVGFile() const
{
   return "@module_icons_dir/NukeX.svg";
}

// ----------------------------------------------------------------------------

InterfaceFeatures NukeXStackInterface::Features() const
{
   return InterfaceFeature::Default;
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::ApplyInstance() const
{
   m_instance.LaunchGlobal();
}

// ----------------------------------------------------------------------------

void NukeXStackInterface::ResetInstance()
{
   NukeXStackInstance defaultInstance( TheNukeXStackProcess );
   ImportProcess( defaultInstance );
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::Launch( const MetaProcess& P, const ProcessImplementation* p, bool& dynamic, unsigned& /*flags*/ )
{
   if ( p != nullptr )
      ImportProcess( *p );
   else
      ResetInstance();

   dynamic = false;
   return &P == TheNukeXStackProcess;
}

// ----------------------------------------------------------------------------

ProcessImplementation* NukeXStackInterface::NewProcess() const
{
   return new NukeXStackInstance( m_instance );
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::ValidateProcess( const ProcessImplementation& p, String& whyNot ) const
{
   if ( dynamic_cast<const NukeXStackInstance*>( &p ) != nullptr )
      return true;
   whyNot = "Not a NukeXStack instance.";
   return false;
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::RequiresInstanceValidation() const
{
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStackInterface::ImportProcess( const ProcessImplementation& p )
{
   m_instance.Assign( p );
   return true;
}

// ----------------------------------------------------------------------------

} // namespace pcl
