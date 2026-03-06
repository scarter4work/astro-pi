//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStretch Instance - Stub for compilation

#ifndef __NukeXStretchInstance_h
#define __NukeXStretchInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStretchInstance : public ProcessImplementation
{
public:

   NukeXStretchInstance( const MetaProcess* );
   NukeXStretchInstance( const NukeXStretchInstance& );

   void Assign( const ProcessImplementation& ) override;
   UndoFlags UndoMode( const View& ) const override;
   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool ExecuteOn( View& ) override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

private:

   // Parameters will be added when stretch is ported (Task 4.x)

   friend class NukeXStretchProcess;
   friend class NukeXStretchInterface;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStretchInstance_h
