/**
 * BayesianAstro Module Implementation
 */

#include "BayesianAstroModule.h"
#include "BayesianAstroProcess.h"
#include "BayesianAstroInterface.h"

namespace pcl
{

BayesianAstroModule* TheBayesianAstroModule = nullptr;

BayesianAstroModule::BayesianAstroModule()
{
    TheBayesianAstroModule = this;
}

const char* BayesianAstroModule::Version() const
{
    // Format: PIXINSIGHT_MODULE_VERSION_MM.mm.rr.bbbb.LLL
    return "PIXINSIGHT_MODULE_VERSION_01.00.00.0001.eng";
}

IsoString BayesianAstroModule::Name() const
{
    return "BayesianAstro";
}

String BayesianAstroModule::Description() const
{
    return "Distribution-aware image stacking with per-pixel confidence scoring. "
           "Uses Welford's algorithm for numerically stable statistics accumulation, "
           "automatic distribution classification, and intelligent fusion strategies.";
}

String BayesianAstroModule::Company() const
{
    return "EZ Suite";
}

String BayesianAstroModule::Author() const
{
    return "Scott Carter";
}

String BayesianAstroModule::Copyright() const
{
    return "Copyright (c) 2025 Scott Carter. All rights reserved.";
}

String BayesianAstroModule::TradeMarks() const
{
    return "";
}

String BayesianAstroModule::OriginalFileName() const
{
#ifdef __PCL_WINDOWS
    return "BayesianAstro.dll";
#elif defined(__PCL_MACOSX)
    return "BayesianAstro.dylib";
#else
    return "BayesianAstro.so";
#endif
}

void BayesianAstroModule::GetReleaseDate(int& year, int& month, int& day) const
{
    year = 2025;
    month = 12;
    day = 30;
}

// Module singleton accessor - creates module on first call
// Uses heap allocation with intentional non-deletion to avoid static destruction order issues
BayesianAstroModule* GetBayesianAstroModuleInstance()
{
    static BayesianAstroModule* s_instance = new BayesianAstroModule;
    return s_instance;
}

// Static initializer to ensure Module global is set before IdentifyPixInsightModule
namespace {
    struct ModuleInitializer {
        ModuleInitializer() {
            GetBayesianAstroModuleInstance();
        }
    };
    static ModuleInitializer s_moduleInit;
}

} // namespace pcl

// Module entry point - IdentifyPixInsightModule is provided by PCL's API.cpp
// which uses APIInitializer to properly identify the module through TheBayesianAstroModule

PCL_MODULE_EXPORT int InstallPixInsightModule(int mode)
{
    // Module instance is created via GetBayesianAstroModuleInstance() static initializer

    if (mode == pcl::InstallMode::FullInstall)
    {
        // Julia runtime is initialized lazily on first use
        // to avoid issues with signal handlers during module load

        new pcl::BayesianAstroProcess;
        new pcl::BayesianAstroInterface;
    }

    return 0;
}
