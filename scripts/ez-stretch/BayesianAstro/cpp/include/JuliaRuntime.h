/**
 * Julia Runtime
 *
 * Manages embedded Julia runtime and provides C++ interface to BayesianAstro.jl
 */

#ifndef __JuliaRuntime_h
#define __JuliaRuntime_h

#include <string>
#include <vector>
#include <functional>
#include <memory>

// Forward declare Julia types to avoid including julia.h in header
typedef struct _jl_value_t jl_value_t;

namespace pcl
{

// Fusion strategy enum (mirrors Julia)
enum class FusionStrategy : int
{
    MLE = 1,
    ConfidenceWeighted = 2,
    Lucky = 3,
    MultiScale = 4
};

// Processing configuration
struct ProcessingConfig
{
    FusionStrategy fusionStrategy = FusionStrategy::ConfidenceWeighted;
    float confidenceThreshold = 0.1f;
    float outlierSigma = 3.0f;
    int tileSizeX = 1024;
    int tileSizeY = 1024;
    bool useGPU = true;
};

// Processing result
struct ProcessingResult
{
    bool success = false;
    std::string errorMessage;
    std::string fusedImagePath;
    std::string confidenceMapPath;

    // Statistics
    int totalPixels = 0;
    float meanConfidence = 0.0f;
    int gaussianPixels = 0;
    int poissonPixels = 0;
    int bimodalPixels = 0;
    int artifactPixels = 0;
};

// Progress callback type
using ProgressCallback = std::function<void(int percent, const std::string& status)>;

/**
 * JuliaRuntime - Singleton managing Julia interpreter
 */
class JuliaRuntime
{
public:
    // Singleton access
    static JuliaRuntime& Instance();

    // Lifecycle
    bool Initialize(const std::string& juliaHome = "");
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // GPU status
    bool IsGPUAvailable() const;
    std::string GetGPUInfo() const;

    // Main processing entry point
    ProcessingResult ProcessStack(
        const std::vector<std::string>& inputFiles,
        const std::string& outputDirectory,
        const std::string& outputPrefix,
        const ProcessingConfig& config,
        ProgressCallback progressCallback = nullptr
    );

    // Utility functions
    bool ValidateFitsFile(const std::string& path) const;
    std::pair<int, int> GetImageDimensions(const std::string& path) const;

private:
    JuliaRuntime() = default;
    ~JuliaRuntime();

    // Prevent copies
    JuliaRuntime(const JuliaRuntime&) = delete;
    JuliaRuntime& operator=(const JuliaRuntime&) = delete;

    // Internal helpers
    bool LoadBayesianAstroModule();
    jl_value_t* CallJuliaFunction(const char* moduleName, const char* funcName,
                                   const std::vector<jl_value_t*>& args);
    void HandleJuliaException();

    bool m_initialized = false;
    std::string m_juliaModulePath;

    // Cached Julia function pointers for performance
    jl_value_t* m_processStackFunc = nullptr;
    jl_value_t* m_validateFitsFunc = nullptr;
};

} // namespace pcl

#endif // __JuliaRuntime_h
