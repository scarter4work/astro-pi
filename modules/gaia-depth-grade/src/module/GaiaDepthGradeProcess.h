// The GaiaDepthGrade process (plan Task 16): process id GaiaDepthGrade, category scarter4work
// (the script's menu path was scarter4work > Gaia Depth Grade). Both ids are STABLE once shipped.
#ifndef __GaiaDepthGradeProcess_h
#define __GaiaDepthGradeProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class GaiaDepthGradeProcess : public MetaProcess
{
public:

   GaiaDepthGradeProcess();

   IsoString Id() const override;
   IsoString Categories() const override;
   uint32 Version() const override;
   String Description() const override;
   ProcessInterface* DefaultInterface() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;
   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
};

extern GaiaDepthGradeProcess* TheGaiaDepthGradeProcess;

} // namespace pcl

#endif // __GaiaDepthGradeProcess_h
