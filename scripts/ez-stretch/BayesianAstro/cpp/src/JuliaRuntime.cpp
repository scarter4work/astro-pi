/**
 * Julia Runtime Implementation
 *
 * Embeds Julia interpreter and provides interface to BayesianAstro.jl
 */

#include "JuliaRuntime.h"
#include <julia.h>

#include <filesystem>
#include <sstream>

namespace pcl
{

JuliaRuntime& JuliaRuntime::Instance()
{
    static JuliaRuntime instance;
    return instance;
}

JuliaRuntime::~JuliaRuntime()
{
    Shutdown();
}

bool JuliaRuntime::Initialize(const std::string& juliaHome)
{
    if (m_initialized)
        return true;

    // Set JULIA_HOME if provided
    if (!juliaHome.empty())
    {
        std::string envVar = "JULIA_HOME=" + juliaHome;
        putenv(const_cast<char*>(envVar.c_str()));
    }

    // Initialize Julia
    jl_init();

    if (!jl_is_initialized())
    {
        return false;
    }

    // Find our Julia module path (relative to the module binary)
    // TODO: Determine actual path at runtime
    m_juliaModulePath = std::filesystem::current_path().string() + "/julia";

    // Load BayesianAstro module
    if (!LoadBayesianAstroModule())
    {
        Shutdown();
        return false;
    }

    m_initialized = true;
    return true;
}

void JuliaRuntime::Shutdown()
{
    if (m_initialized)
    {
        jl_atexit_hook(0);
        m_initialized = false;
    }
}

bool JuliaRuntime::LoadBayesianAstroModule()
{
    // Add module path to Julia's LOAD_PATH
    std::ostringstream loadCmd;
    loadCmd << "push!(LOAD_PATH, \"" << m_juliaModulePath << "\")";
    jl_eval_string(loadCmd.str().c_str());

    if (jl_exception_occurred())
    {
        HandleJuliaException();
        return false;
    }

    // Load the module
    jl_eval_string("using BayesianAstro");

    if (jl_exception_occurred())
    {
        HandleJuliaException();
        return false;
    }

    // Cache function pointers for performance
    jl_module_t* baModule = (jl_module_t*)jl_eval_string("BayesianAstro");
    if (baModule)
    {
        m_processStackFunc = jl_get_function(baModule, "process_stack");
        m_validateFitsFunc = jl_get_function(baModule, "validate_fits");
    }

    return true;
}

bool JuliaRuntime::IsGPUAvailable() const
{
    if (!m_initialized)
        return false;

    jl_value_t* result = jl_eval_string("BayesianAstro.CUDA_AVAILABLE[]");
    if (jl_exception_occurred())
    {
        return false;
    }

    return jl_unbox_bool(result);
}

std::string JuliaRuntime::GetGPUInfo() const
{
    if (!m_initialized || !IsGPUAvailable())
        return "No GPU available";

    jl_value_t* result = jl_eval_string(
        "try; string(CUDA.name(CUDA.device())); catch; \"Unknown\"; end"
    );

    if (jl_exception_occurred() || !jl_is_string(result))
        return "GPU info unavailable";

    return std::string(jl_string_ptr(result));
}

ProcessingResult JuliaRuntime::ProcessStack(
    const std::vector<std::string>& inputFiles,
    const std::string& outputDirectory,
    const std::string& outputPrefix,
    const ProcessingConfig& config,
    ProgressCallback progressCallback)
{
    ProcessingResult result;

    if (!m_initialized)
    {
        result.success = false;
        result.errorMessage = "Julia runtime not initialized";
        return result;
    }

    // Build Julia array of input files
    std::ostringstream filesArrayCmd;
    filesArrayCmd << "String[";
    for (size_t i = 0; i < inputFiles.size(); ++i)
    {
        if (i > 0) filesArrayCmd << ", ";
        filesArrayCmd << "\"" << inputFiles[i] << "\"";
    }
    filesArrayCmd << "]";

    jl_value_t* filesArray = jl_eval_string(filesArrayCmd.str().c_str());
    if (jl_exception_occurred())
    {
        HandleJuliaException();
        result.success = false;
        result.errorMessage = "Failed to create input file list";
        return result;
    }

    // Build ProcessingConfig in Julia
    std::ostringstream configCmd;
    configCmd << "ProcessingConfig("
              << "fusion_strategy=" << static_cast<int>(config.fusionStrategy) << ", "
              << "confidence_threshold=" << config.confidenceThreshold << "f0, "
              << "outlier_sigma=" << config.outlierSigma << "f0, "
              << "tile_size=(" << config.tileSizeX << ", " << config.tileSizeY << "), "
              << "use_gpu=" << (config.useGPU ? "true" : "false")
              << ")";

    jl_value_t* juliaConfig = jl_eval_string(configCmd.str().c_str());
    if (jl_exception_occurred())
    {
        HandleJuliaException();
        result.success = false;
        result.errorMessage = "Failed to create processing config";
        return result;
    }

    // Report progress: starting
    if (progressCallback)
        progressCallback(0, "Loading frames...");

    // Call process_directory function
    std::ostringstream processCmd;
    processCmd << "process_directory("
               << "\"" << outputDirectory << "/" << outputPrefix << "\", "
               << "config=" << configCmd.str()
               << ")";

    // Note: This is simplified - full implementation would pass the file array
    // and handle progress callbacks via Julia's channel mechanism

    jl_eval_string(processCmd.str().c_str());

    if (jl_exception_occurred())
    {
        HandleJuliaException();
        result.success = false;
        result.errorMessage = "Processing failed - see console for details";
        return result;
    }

    // Build result paths
    result.success = true;
    result.fusedImagePath = outputDirectory + "/" + outputPrefix + "_fused.fits";
    result.confidenceMapPath = outputDirectory + "/" + outputPrefix + "_confidence.fits";

    if (progressCallback)
        progressCallback(100, "Complete");

    return result;
}

bool JuliaRuntime::ValidateFitsFile(const std::string& path) const
{
    if (!m_initialized)
        return false;

    std::ostringstream cmd;
    cmd << "try; FITS(\"" << path << "\", \"r\"); true; catch; false; end";

    jl_value_t* result = jl_eval_string(cmd.str().c_str());
    if (jl_exception_occurred())
        return false;

    return jl_unbox_bool(result);
}

std::pair<int, int> JuliaRuntime::GetImageDimensions(const std::string& path) const
{
    if (!m_initialized)
        return {0, 0};

    std::ostringstream cmd;
    cmd << "let f = FITS(\"" << path << "\", \"r\"); "
        << "sz = size(read(f[1])); close(f); (sz[1], sz[2]); end";

    jl_value_t* result = jl_eval_string(cmd.str().c_str());
    if (jl_exception_occurred() || !jl_is_tuple(result))
        return {0, 0};

    int height = jl_unbox_int64(jl_get_nth_field(result, 0));
    int width = jl_unbox_int64(jl_get_nth_field(result, 1));

    return {width, height};
}

void JuliaRuntime::HandleJuliaException()
{
    jl_value_t* ex = jl_exception_occurred();
    if (ex)
    {
        jl_value_t* str = jl_call1(jl_get_function(jl_base_module, "sprint"),
                                    jl_get_function(jl_base_module, "showerror"));
        if (str && jl_is_string(str))
        {
            // Log error - in real implementation would use Console::CriticalLn
            fprintf(stderr, "Julia exception: %s\n", jl_string_ptr(str));
        }
        jl_exception_clear();
    }
}

jl_value_t* JuliaRuntime::CallJuliaFunction(
    const char* moduleName,
    const char* funcName,
    const std::vector<jl_value_t*>& args)
{
    // Get module
    jl_module_t* mod = nullptr;
    if (strcmp(moduleName, "Base") == 0)
        mod = jl_base_module;
    else if (strcmp(moduleName, "Main") == 0)
        mod = jl_main_module;
    else
        mod = (jl_module_t*)jl_eval_string(moduleName);

    if (!mod)
        return nullptr;

    // Get function
    jl_function_t* func = jl_get_function(mod, funcName);
    if (!func)
        return nullptr;

    // Call with arguments
    jl_value_t* result = nullptr;
    JL_GC_PUSH1(&result);

    switch (args.size())
    {
    case 0:
        result = jl_call0(func);
        break;
    case 1:
        result = jl_call1(func, args[0]);
        break;
    case 2:
        result = jl_call2(func, args[0], args[1]);
        break;
    case 3:
        result = jl_call3(func, args[0], args[1], args[2]);
        break;
    default:
        result = jl_call(func, const_cast<jl_value_t**>(args.data()), args.size());
        break;
    }

    JL_GC_POP();

    if (jl_exception_occurred())
    {
        HandleJuliaException();
        return nullptr;
    }

    return result;
}

} // namespace pcl
