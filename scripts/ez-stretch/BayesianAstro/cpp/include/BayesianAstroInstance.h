/**
 * BayesianAstro Instance
 *
 * Process instance containing runtime state and execution logic.
 */

#ifndef __BayesianAstroInstance_h
#define __BayesianAstroInstance_h

#include <pcl/ProcessImplementation.h>
#include <pcl/StringList.h>

#include "BayesianAstroParameters.h"

namespace pcl
{

class BayesianAstroInstance : public ProcessImplementation
{
public:
    BayesianAstroInstance(const MetaProcess*);
    BayesianAstroInstance(const BayesianAstroInstance&);

    void Assign(const ProcessImplementation&) override;
    bool IsHistoryUpdater(const View&) const override;
    UndoFlags UndoMode(const View&) const override;
    bool CanExecuteOn(const View&, String& whyNot) const override;
    bool CanExecuteGlobal(String& whyNot) const override;
    bool ExecuteGlobal() override;
    void* LockParameter(const MetaParameter*, size_type tableRow) override;
    bool AllocateParameter(size_type sizeOrLength, const MetaParameter*, size_type tableRow) override;
    size_type ParameterLength(const MetaParameter*, size_type tableRow) const override;

    // Accessors for React UI bridge
    pcl_enum FusionStrategy() const { return p_fusionStrategy; }
    void SetFusionStrategy(pcl_enum v) { p_fusionStrategy = v; }

    const StringList& InputFiles() const { return p_inputFiles; }
    void SetInputFiles(const StringList& files) { p_inputFiles = files; }
    void AddInputFile(const String& path) { p_inputFiles.Add(path); }
    void ClearInputFiles() { p_inputFiles.Clear(); }

    float OutlierSigma() const { return p_outlierSigma; }
    void SetOutlierSigma(float v) { p_outlierSigma = v; }

    float ConfidenceThreshold() const { return p_confidenceThreshold; }
    void SetConfidenceThreshold(float v) { p_confidenceThreshold = v; }

    bool UseGPU() const { return p_useGPU; }
    void SetUseGPU(bool v) { p_useGPU = v; }

    bool GenerateConfidenceMap() const { return p_generateConfidenceMap; }
    void SetGenerateConfidenceMap(bool v) { p_generateConfidenceMap = v; }

    const String& OutputDirectory() const { return p_outputDirectory; }
    void SetOutputDirectory(const String& v) { p_outputDirectory = v; }

    const String& OutputPrefix() const { return p_outputPrefix; }
    void SetOutputPrefix(const String& v) { p_outputPrefix = v; }

private:
    // Parameters
    pcl_enum   p_fusionStrategy;
    StringList p_inputFiles;
    float      p_outlierSigma;
    float      p_confidenceThreshold;
    pcl_bool   p_useGPU;
    pcl_bool   p_generateConfidenceMap;
    String     p_outputDirectory;
    String     p_outputPrefix;

    // Internal methods
    bool ValidateInputFiles() const;
    void ProcessStack();

    friend class BayesianAstroProcess;
    friend class BayesianAstroInterface;
};

} // namespace pcl

#endif // __BayesianAstroInstance_h
