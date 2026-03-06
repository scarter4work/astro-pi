//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "NukeXStretchInstance.h"

namespace pcl
{

// ----------------------------------------------------------------------------

NukeXStretchInstance::NukeXStretchInstance( const MetaProcess* m )
   : ProcessImplementation( m )
{
}

// ----------------------------------------------------------------------------

NukeXStretchInstance::NukeXStretchInstance( const NukeXStretchInstance& x )
   : ProcessImplementation( x )
{
}

// ----------------------------------------------------------------------------

void NukeXStretchInstance::Assign( const ProcessImplementation& p )
{
   const NukeXStretchInstance* x = dynamic_cast<const NukeXStretchInstance*>( &p );
   if ( x != nullptr )
   {
      // Copy parameters when ported (Task 4.x)
   }
}

// ----------------------------------------------------------------------------

UndoFlags NukeXStretchInstance::UndoMode( const View& ) const
{
   return UndoFlag::PixelData;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInstance::CanExecuteOn( const View& view, String& whyNot ) const
{
   if ( view.Image().IsComplexSample() )
   {
      whyNot = "NukeXStretch cannot be executed on complex images.";
      return false;
   }
   return true;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInstance::ExecuteOn( View& view )
{
   // Stub - full implementation in Task 4.x
   return false;
}

// ----------------------------------------------------------------------------

void* NukeXStretchInstance::LockParameter( const MetaParameter* p, size_type tableRow )
{
   // Will map parameters in Task 4.x
   return nullptr;
}

// ----------------------------------------------------------------------------

bool NukeXStretchInstance::AllocateParameter( size_type sizeOrLength, const MetaParameter* p, size_type tableRow )
{
   // Will allocate in Task 4.x
   return true;
}

// ----------------------------------------------------------------------------

size_type NukeXStretchInstance::ParameterLength( const MetaParameter* p, size_type tableRow ) const
{
   // Will return lengths in Task 4.x
   return 0;
}

// ----------------------------------------------------------------------------

} // namespace pcl
