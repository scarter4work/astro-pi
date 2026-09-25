# PI Copilot PixInsight Module
# Builds PICopilot-pxm.so (Linux), PICopilot-pxm.dylib (macOS), PICopilot-pxm.dll (Windows)

set(PCLDIR "$ENV{HOME}/PCL" CACHE PATH "PixInsight Class Library directory")

if(NOT EXISTS "${PCLDIR}/include/pcl/MetaModule.h")
    message(STATUS "PCL not found at ${PCLDIR} -- skipping module build")
    return()
endif()

message(STATUS "PCL found at ${PCLDIR} -- building PICopilot module")

set(MODULE_SOURCES
    PICopilotModule.cpp
    PICopilotProcess.cpp
    PICopilotInstance.cpp
    PICopilotInterface.cpp
    PICopilotSelfTest.cpp
    KeyStore.cpp
    AnthropicClient.cpp
    ConfigDialog.cpp
    ChatThread.cpp
    PICopilotVisionSelfTest.cpp
    ViewContext.cpp
    ViewPreview.cpp
    ProcessCatalog.cpp
    VisionTurn.cpp
    ViewCapture.cpp
    PICopilotAgentSelfTest.cpp
    ProcessApply.cpp
    AgentTools.cpp
    SystemPrompt.cpp
    AgentSession.cpp
    TurnEndNotes.cpp
    PICopilotInc5SelfTest.cpp
    SseStream.cpp
    ModelCatalog.cpp
    HistoryBudget.cpp
    Keyring.cpp
)

add_library(PICopilot SHARED ${MODULE_SOURCES})

# Process summaries are COMPILED IN (no runtime file dependency). configure_file
# re-runs whenever the JSON changes because CMake tracks file(READ) inputs via
# CMAKE_CONFIGURE_DEPENDS below.
set(PICOPILOT_SUMMARIES_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../data/process-summaries.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PICOPILOT_SUMMARIES_SRC}")
file(READ "${PICOPILOT_SUMMARIES_SRC}" PICOPILOT_PROCESS_SUMMARIES_JSON)
configure_file(ProcessSummaries.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ProcessSummaries.h" @ONLY)
target_include_directories(PICopilot PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# PCL include directories.
#
# PCL headers are treated as SYSTEM includes so GCC's -Wparentheses and
# other warnings originating inside PCL don't flood every build. Our own
# code is still compiled with -Wall -Wextra per the top-level CMakeLists.
target_include_directories(PICopilot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(PICopilot SYSTEM PRIVATE
    ${PCLDIR}/include
)

# PCL compile definitions
target_compile_definitions(PICopilot PRIVATE
    __PCL_LINUX
    __PCL_BUILDING_MODULE
    _REENTRANT
)

# PCL compile flags
target_compile_options(PICopilot PRIVATE
    -fPIC
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# Link PCL static libraries
target_link_directories(PICopilot PRIVATE ${PCLDIR}/lib/x64)
target_link_libraries(PICopilot PRIVATE
    PCL-pxi
    lz4-pxi
    zstd-pxi
    zlib-pxi
    RFC6234-pxi
    lcms-pxi
    cminpack-pxi
    nlohmann_json::nlohmann_json
)

# Shared library flags
set_target_properties(PICopilot PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "PICopilot-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs -L${PCLDIR}/lib/x64"
)

target_compile_features(PICopilot PRIVATE cxx_std_17)
