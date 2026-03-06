//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX v3 - Statistical Stacking + Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// NukeXStack Instance - Stub for compilation

#ifndef __NukeXStackInstance_h
#define __NukeXStackInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/MetaParameter.h>

namespace pcl
{

// ----------------------------------------------------------------------------

class NukeXStackInstance : public ProcessImplementation
{
public:

   NukeXStackInstance( const MetaProcess* );
   NukeXStackInstance( const NukeXStackInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
   void* LockParameter( const MetaParameter*, size_type tableRow ) override;
   bool AllocateParameter( size_type sizeOrLength, const MetaParameter*, size_type tableRow ) override;
   size_type ParameterLength( const MetaParameter*, size_type tableRow ) const override;

private:

   // Parameters will be added in Task 1.3

   friend class NukeXStackProcess;
   friend class NukeXStackInterface;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __NukeXStackInstance_h
