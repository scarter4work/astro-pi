/**
 * BayesianAstro Parameters
 *
 * Process parameters for Bayesian stacking configuration.
 */

#ifndef __BayesianAstroParameters_h
#define __BayesianAstroParameters_h

#include <pcl/MetaParameter.h>

namespace pcl
{

// Fusion strategy enumeration
class BAFusionStrategy : public MetaEnumeration
{
public:
    enum { MLE = 0,
           ConfidenceWeighted = 1,
           Lucky = 2,
           MultiScale = 3,
           NumberOfItems,
           Default = ConfidenceWeighted };

    BAFusionStrategy(MetaProcess*);

    IsoString Id() const override;
    size_type NumberOfElements() const override;
    IsoString ElementId(size_type) const override;
    int ElementValue(size_type) const override;
    size_type DefaultValueIndex() const override;
};

// Input file list
class BAInputFiles : public MetaTable
{
public:
    BAInputFiles(MetaProcess*);

    IsoString Id() const override;
};

class BAInputFilePath : public MetaString
{
public:
    BAInputFilePath(MetaTable*);

    IsoString Id() const override;
};

// Outlier rejection sigma
class BAOutlierSigma : public MetaFloat
{
public:
    BAOutlierSigma(MetaProcess*);

    IsoString Id() const override;
    int Precision() const override;
    double DefaultValue() const override;
    double MinimumValue() const override;
    double MaximumValue() const override;
};

// Confidence threshold
class BAConfidenceThreshold : public MetaFloat
{
public:
    BAConfidenceThreshold(MetaProcess*);

    IsoString Id() const override;
    int Precision() const override;
    double DefaultValue() const override;
    double MinimumValue() const override;
    double MaximumValue() const override;
};

// Use GPU acceleration
class BAUseGPU : public MetaBoolean
{
public:
    BAUseGPU(MetaProcess*);

    IsoString Id() const override;
    bool DefaultValue() const override;
};

// Generate confidence map output
class BAGenerateConfidenceMap : public MetaBoolean
{
public:
    BAGenerateConfidenceMap(MetaProcess*);

    IsoString Id() const override;
    bool DefaultValue() const override;
};

// Output directory
class BAOutputDirectory : public MetaString
{
public:
    BAOutputDirectory(MetaProcess*);

    IsoString Id() const override;
};

// Output prefix
class BAOutputPrefix : public MetaString
{
public:
    BAOutputPrefix(MetaProcess*);

    IsoString Id() const override;
    String DefaultValue() const override;
};

// Extern declarations
extern BAFusionStrategy* TheBAFusionStrategyParameter;
extern BAInputFiles* TheBAInputFilesParameter;
extern BAInputFilePath* TheBAInputFilePathParameter;
extern BAOutlierSigma* TheBAOutlierSigmaParameter;
extern BAConfidenceThreshold* TheBAConfidenceThresholdParameter;
extern BAUseGPU* TheBAUseGPUParameter;
extern BAGenerateConfidenceMap* TheBAGenerateConfidenceMapParameter;
extern BAOutputDirectory* TheBAOutputDirectoryParameter;
extern BAOutputPrefix* TheBAOutputPrefixParameter;

} // namespace pcl

#endif // __BayesianAstroParameters_h
