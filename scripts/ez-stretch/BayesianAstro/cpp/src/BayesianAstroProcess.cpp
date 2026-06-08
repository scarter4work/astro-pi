/**
 * BayesianAstro Process Implementation
 */

#include "BayesianAstroProcess.h"
#include "BayesianAstroInstance.h"
#include "BayesianAstroInterface.h"
#include "BayesianAstroParameters.h"

namespace pcl
{

BayesianAstroProcess* TheBayesianAstroProcess = nullptr;

BayesianAstroProcess::BayesianAstroProcess()
{
    TheBayesianAstroProcess = this;

    // Register parameters
    new BAFusionStrategy(this);

    // Input files table with its column
    BAInputFiles* inputFiles = new BAInputFiles(this);
    new BAInputFilePath(inputFiles);  // Column for the table

    new BAOutlierSigma(this);
    new BAConfidenceThreshold(this);
    new BAUseGPU(this);
    new BAGenerateConfidenceMap(this);
    new BAOutputDirectory(this);
    new BAOutputPrefix(this);
}

IsoString BayesianAstroProcess::Id() const
{
    return "BayesianAstro";
}

IsoString BayesianAstroProcess::Category() const
{
    return "ImageIntegration";
}

uint32 BayesianAstroProcess::Version() const
{
    return 0x100;  // 1.0.0
}

String BayesianAstroProcess::Description() const
{
    return "<html>"
           "<p>BayesianAstro is a distribution-aware image stacking process that preserves "
           "statistical information across frames for intelligent fusion decisions.</p>"
           "<p><b>Key Features:</b></p>"
           "<ul>"
           "<li>Per-pixel statistical distribution tracking via Welford's algorithm</li>"
           "<li>Automatic classification of pixel behavior (Gaussian, Poisson, bimodal, artifacts)</li>"
           "<li>Confidence scoring based on distribution properties</li>"
           "<li>Multiple fusion strategies (MLE, confidence-weighted, lucky imaging, multi-scale)</li>"
           "<li>GPU acceleration via CUDA</li>"
           "</ul>"
           "</html>";
}

String BayesianAstroProcess::IconImageSVGFile() const
{
    return String();  // TODO: Add icon
}

ProcessInterface* BayesianAstroProcess::DefaultInterface() const
{
    return TheBayesianAstroInterface;
}

ProcessImplementation* BayesianAstroProcess::Create() const
{
    return new BayesianAstroInstance(this);
}

ProcessImplementation* BayesianAstroProcess::Clone(const ProcessImplementation& p) const
{
    return new BayesianAstroInstance(static_cast<const BayesianAstroInstance&>(p));
}

bool BayesianAstroProcess::CanProcessCommandLines() const
{
    return true;
}

bool BayesianAstroProcess::CanBrowseDocumentation() const
{
    return true;
}

bool BayesianAstroProcess::PrefersGlobalExecution() const
{
    return true;  // Operates on files, not views
}

} // namespace pcl
