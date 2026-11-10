// EZTools -- EZHazeKill process.
// Copyright and licence: see src/EZToolsLicense.h.

#ifndef __EZHazeKillProcess_h
#define __EZHazeKillProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class EZHazeKillProcess : public MetaProcess
{
public:
   EZHazeKillProcess();

   IsoString Id() const override;   // "EZHazeKill" -- STABLE, never rename
   IsoString Categories() const override;
   uint32 Version() const override;
   String Description() const override;
   ProcessInterface* DefaultInterface() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;
   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
};

PCL_BEGIN_LOCAL
extern EZHazeKillProcess* TheEZHazeKillProcess;
PCL_END_LOCAL

} // namespace pcl

#endif
