/**
 * BayesianAstro Instance Implementation
 */

#include "BayesianAstroInstance.h"
#include "BayesianAstroParameters.h"
#include "JuliaRuntime.h"

#include <pcl/Console.h>
#include <pcl/StandardStatus.h>

namespace pcl
{

BayesianAstroInstance::BayesianAstroInstance(const MetaProcess* m)
    : ProcessImplementation(m)
    , p_fusionStrategy(BAFusionStrategy::Default)
    , p_outlierSigma(TheBAOutlierSigmaParameter->DefaultValue())
    , p_confidenceThreshold(TheBAConfidenceThresholdParameter->DefaultValue())
    , p_useGPU(TheBAUseGPUParameter->DefaultValue())
    , p_generateConfidenceMap(TheBAGenerateConfidenceMapParameter->DefaultValue())
    , p_outputPrefix(TheBAOutputPrefixParameter->DefaultValue())
{
}

BayesianAstroInstance::BayesianAstroInstance(const BayesianAstroInstance& x)
    : ProcessImplementation(x)
    , p_fusionStrategy(x.p_fusionStrategy)
    , p_inputFiles(x.p_inputFiles)
    , p_outlierSigma(x.p_outlierSigma)
    , p_confidenceThreshold(x.p_confidenceThreshold)
    , p_useGPU(x.p_useGPU)
    , p_generateConfidenceMap(x.p_generateConfidenceMap)
    , p_outputDirectory(x.p_outputDirectory)
    , p_outputPrefix(x.p_outputPrefix)
{
}

void BayesianAstroInstance::Assign(const ProcessImplementation& p)
{
    const BayesianAstroInstance* x = dynamic_cast<const BayesianAstroInstance*>(&p);
    if (x != nullptr)
    {
        p_fusionStrategy = x->p_fusionStrategy;
        p_inputFiles = x->p_inputFiles;
        p_outlierSigma = x->p_outlierSigma;
        p_confidenceThreshold = x->p_confidenceThreshold;
        p_useGPU = x->p_useGPU;
        p_generateConfidenceMap = x->p_generateConfidenceMap;
        p_outputDirectory = x->p_outputDirectory;
        p_outputPrefix = x->p_outputPrefix;
    }
}

bool BayesianAstroInstance::IsHistoryUpdater(const View&) const
{
    return false;
}

UndoFlags BayesianAstroInstance::UndoMode(const View&) const
{
    return UndoFlag::DefaultMode;
}

bool BayesianAstroInstance::CanExecuteOn(const View&, String& whyNot) const
{
    whyNot = "BayesianAstro operates on files, not views. Use global execution.";
    return false;
}

bool BayesianAstroInstance::CanExecuteGlobal(String& whyNot) const
{
    if (p_inputFiles.IsEmpty())
    {
        whyNot = "No input files specified.";
        return false;
    }

    if (p_outputDirectory.IsEmpty())
    {
        whyNot = "No output directory specified.";
        return false;
    }

    if (!JuliaRuntime::Instance().IsInitialized())
    {
        whyNot = "Julia runtime not initialized.";
        return false;
    }

    return true;
}

bool BayesianAstroInstance::ExecuteGlobal()
{
    Console console;

    console.WriteLn("<b>BayesianAstro</b>");
    console.WriteLn(String().Format("Processing %d frames...", p_inputFiles.Length()));

    // Convert StringList to std::vector
    std::vector<std::string> inputFiles;
    for (const String& s : p_inputFiles)
        inputFiles.push_back(s.ToUTF8().c_str());

    // Build config
    ProcessingConfig config;
    config.fusionStrategy = static_cast<pcl::FusionStrategy>(p_fusionStrategy + 1);  // Julia is 1-indexed
    config.outlierSigma = p_outlierSigma;
    config.confidenceThreshold = p_confidenceThreshold;
    config.useGPU = p_useGPU;

    // Progress callback
    StandardStatus status;
    StatusMonitor monitor;
    monitor.SetCallback(&status);
    monitor.Initialize("BayesianAstro", p_inputFiles.Length());

    auto progressCallback = [&](int percent, const std::string& msg) {
        console.WriteLn(String(msg.c_str()));
    };

    // Execute
    ProcessingResult result = JuliaRuntime::Instance().ProcessStack(
        inputFiles,
        p_outputDirectory.ToUTF8().c_str(),
        p_outputPrefix.ToUTF8().c_str(),
        config,
        progressCallback
    );

    monitor.Complete();

    if (!result.success)
    {
        console.CriticalLn("** Processing failed: " + String(result.errorMessage.c_str()));
        return false;
    }

    console.WriteLn("Fused image: " + String(result.fusedImagePath.c_str()));
    if (p_generateConfidenceMap)
        console.WriteLn("Confidence map: " + String(result.confidenceMapPath.c_str()));

    console.WriteLn(String().Format("Mean confidence: %.3f", result.meanConfidence));

    return true;
}

void* BayesianAstroInstance::LockParameter(const MetaParameter* p, size_type tableRow)
{
    if (p == TheBAFusionStrategyParameter)
        return &p_fusionStrategy;
    if (p == TheBAInputFilePathParameter)
        return p_inputFiles[tableRow].Begin();
    if (p == TheBAOutlierSigmaParameter)
        return &p_outlierSigma;
    if (p == TheBAConfidenceThresholdParameter)
        return &p_confidenceThreshold;
    if (p == TheBAUseGPUParameter)
        return &p_useGPU;
    if (p == TheBAGenerateConfidenceMapParameter)
        return &p_generateConfidenceMap;
    if (p == TheBAOutputDirectoryParameter)
        return p_outputDirectory.Begin();
    if (p == TheBAOutputPrefixParameter)
        return p_outputPrefix.Begin();

    return nullptr;
}

bool BayesianAstroInstance::AllocateParameter(size_type length, const MetaParameter* p, size_type tableRow)
{
    if (p == TheBAInputFilesParameter)
    {
        p_inputFiles.Clear();
        if (length > 0)
            p_inputFiles.Add(String(), length);
    }
    else if (p == TheBAInputFilePathParameter)
    {
        p_inputFiles[tableRow].Clear();
        if (length > 0)
            p_inputFiles[tableRow].SetLength(length);
    }
    else if (p == TheBAOutputDirectoryParameter)
    {
        p_outputDirectory.Clear();
        if (length > 0)
            p_outputDirectory.SetLength(length);
    }
    else if (p == TheBAOutputPrefixParameter)
    {
        p_outputPrefix.Clear();
        if (length > 0)
            p_outputPrefix.SetLength(length);
    }
    else
        return false;

    return true;
}

size_type BayesianAstroInstance::ParameterLength(const MetaParameter* p, size_type tableRow) const
{
    if (p == TheBAInputFilesParameter)
        return p_inputFiles.Length();
    if (p == TheBAInputFilePathParameter)
        return p_inputFiles[tableRow].Length();
    if (p == TheBAOutputDirectoryParameter)
        return p_outputDirectory.Length();
    if (p == TheBAOutputPrefixParameter)
        return p_outputPrefix.Length();

    return 0;
}

bool BayesianAstroInstance::ValidateInputFiles() const
{
    for (const String& path : p_inputFiles)
    {
        if (!JuliaRuntime::Instance().ValidateFitsFile(path.ToUTF8().c_str()))
            return false;
    }
    return true;
}

} // namespace pcl
