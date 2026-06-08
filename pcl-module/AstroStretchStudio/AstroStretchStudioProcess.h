// ----------------------------------------------------------------------------
// AstroStretchStudio Process Header
// ----------------------------------------------------------------------------

#ifndef __AstroStretchStudioProcess_h
#define __AstroStretchStudioProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

// ----------------------------------------------------------------------------

class AstroStretchStudioProcess : public MetaProcess
{
public:

   AstroStretchStudioProcess();

   IsoString Id() const override;
   IsoString Category() const override;
   uint32 Version() const override;
   String Description() const override;
   IsoString IconImageSVG() const override;
   ProcessInterface* DefaultInterface() const override;
   ProcessImplementation* Create() const override;
   ProcessImplementation* Clone( const ProcessImplementation& ) const override;
   bool NeedsValidation() const override;
   bool CanProcessViews() const override;
   bool CanProcessGlobal() const override;
};

// ----------------------------------------------------------------------------

PCL_BEGIN_LOCAL
extern AstroStretchStudioProcess* TheAstroStretchStudioProcess;
PCL_END_LOCAL

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __AstroStretchStudioProcess_h

// ----------------------------------------------------------------------------
