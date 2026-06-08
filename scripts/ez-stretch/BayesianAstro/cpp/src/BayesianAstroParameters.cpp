/**
 * BayesianAstro Parameters Implementation
 */

#include "BayesianAstroParameters.h"

namespace pcl
{

// Parameter instances
BAFusionStrategy* TheBAFusionStrategyParameter = nullptr;
BAInputFiles* TheBAInputFilesParameter = nullptr;
BAInputFilePath* TheBAInputFilePathParameter = nullptr;
BAOutlierSigma* TheBAOutlierSigmaParameter = nullptr;
BAConfidenceThreshold* TheBAConfidenceThresholdParameter = nullptr;
BAUseGPU* TheBAUseGPUParameter = nullptr;
BAGenerateConfidenceMap* TheBAGenerateConfidenceMapParameter = nullptr;
BAOutputDirectory* TheBAOutputDirectoryParameter = nullptr;
BAOutputPrefix* TheBAOutputPrefixParameter = nullptr;

// BAFusionStrategy

BAFusionStrategy::BAFusionStrategy(MetaProcess* p) : MetaEnumeration(p)
{
    TheBAFusionStrategyParameter = this;
}

IsoString BAFusionStrategy::Id() const { return "fusionStrategy"; }
size_type BAFusionStrategy::NumberOfElements() const { return NumberOfItems; }

IsoString BAFusionStrategy::ElementId(size_type i) const
{
    switch (i)
    {
    case MLE: return "MLE";
    case ConfidenceWeighted: return "ConfidenceWeighted";
    case Lucky: return "Lucky";
    case MultiScale: return "MultiScale";
    default: return "";
    }
}

int BAFusionStrategy::ElementValue(size_type i) const { return int(i); }
size_type BAFusionStrategy::DefaultValueIndex() const { return Default; }

// BAInputFiles

BAInputFiles::BAInputFiles(MetaProcess* p) : MetaTable(p)
{
    TheBAInputFilesParameter = this;
}

IsoString BAInputFiles::Id() const { return "inputFiles"; }

// BAInputFilePath

BAInputFilePath::BAInputFilePath(MetaTable* t) : MetaString(t)
{
    TheBAInputFilePathParameter = this;
}

IsoString BAInputFilePath::Id() const { return "filePath"; }

// BAOutlierSigma

BAOutlierSigma::BAOutlierSigma(MetaProcess* p) : MetaFloat(p)
{
    TheBAOutlierSigmaParameter = this;
}

IsoString BAOutlierSigma::Id() const { return "outlierSigma"; }
int BAOutlierSigma::Precision() const { return 2; }
double BAOutlierSigma::DefaultValue() const { return 3.0; }
double BAOutlierSigma::MinimumValue() const { return 0.5; }
double BAOutlierSigma::MaximumValue() const { return 10.0; }

// BAConfidenceThreshold

BAConfidenceThreshold::BAConfidenceThreshold(MetaProcess* p) : MetaFloat(p)
{
    TheBAConfidenceThresholdParameter = this;
}

IsoString BAConfidenceThreshold::Id() const { return "confidenceThreshold"; }
int BAConfidenceThreshold::Precision() const { return 2; }
double BAConfidenceThreshold::DefaultValue() const { return 0.1; }
double BAConfidenceThreshold::MinimumValue() const { return 0.0; }
double BAConfidenceThreshold::MaximumValue() const { return 1.0; }

// BAUseGPU

BAUseGPU::BAUseGPU(MetaProcess* p) : MetaBoolean(p)
{
    TheBAUseGPUParameter = this;
}

IsoString BAUseGPU::Id() const { return "useGPU"; }
bool BAUseGPU::DefaultValue() const { return true; }

// BAGenerateConfidenceMap

BAGenerateConfidenceMap::BAGenerateConfidenceMap(MetaProcess* p) : MetaBoolean(p)
{
    TheBAGenerateConfidenceMapParameter = this;
}

IsoString BAGenerateConfidenceMap::Id() const { return "generateConfidenceMap"; }
bool BAGenerateConfidenceMap::DefaultValue() const { return true; }

// BAOutputDirectory

BAOutputDirectory::BAOutputDirectory(MetaProcess* p) : MetaString(p)
{
    TheBAOutputDirectoryParameter = this;
}

IsoString BAOutputDirectory::Id() const { return "outputDirectory"; }

// BAOutputPrefix

BAOutputPrefix::BAOutputPrefix(MetaProcess* p) : MetaString(p)
{
    TheBAOutputPrefixParameter = this;
}

IsoString BAOutputPrefix::Id() const { return "outputPrefix"; }
String BAOutputPrefix::DefaultValue() const { return "bayesian"; }

} // namespace pcl
