/**
 * BayesianAstro Process
 *
 * Process definition for the Bayesian stacking algorithm.
 */

#ifndef __BayesianAstroProcess_h
#define __BayesianAstroProcess_h

#include <pcl/MetaProcess.h>

namespace pcl
{

class BayesianAstroProcess : public MetaProcess
{
public:
    BayesianAstroProcess();

    IsoString Id() const override;
    IsoString Category() const override;
    uint32 Version() const override;
    String Description() const override;
    String IconImageSVGFile() const override;
    ProcessInterface* DefaultInterface() const override;
    ProcessImplementation* Create() const override;
    ProcessImplementation* Clone(const ProcessImplementation&) const override;
    bool CanProcessCommandLines() const override;
    bool CanBrowseDocumentation() const override;
    bool PrefersGlobalExecution() const override;
};

extern BayesianAstroProcess* TheBayesianAstroProcess;

} // namespace pcl

#endif // __BayesianAstroProcess_h
