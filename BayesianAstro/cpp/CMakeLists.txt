cmake_minimum_required(VERSION 3.16)
project(BayesianAstro VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# PixInsight paths
set(PCLDIR "/opt/PixInsight")
set(PCLINCDIR "${PCLDIR}/include")
set(PCLSRCDIR "${PCLDIR}/src")
set(PCLLIBDIR "${PCLDIR}/bin")

# Julia paths (auto-detect)
execute_process(
    COMMAND julia -e "print(joinpath(Sys.BINDIR, Base.INCLUDEDIR, \"julia\"))"
    OUTPUT_VARIABLE JULIA_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND julia -e "print(joinpath(Sys.BINDIR, Base.LIBDIR))"
    OUTPUT_VARIABLE JULIA_LIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Julia include: ${JULIA_INCLUDE_DIR}")
message(STATUS "Julia lib: ${JULIA_LIB_DIR}")

# Source files
set(SOURCES
    src/BayesianAstroModule.cpp
    src/BayesianAstroProcess.cpp
    src/BayesianAstroInstance.cpp
    src/BayesianAstroInterface.cpp
    src/BayesianAstroParameters.cpp
    src/JuliaRuntime.cpp
)

# PCL source files needed for module
file(GLOB PCL_SOURCES "${PCLSRCDIR}/pcl/*.cpp")
# Exclude platform-specific files
list(FILTER PCL_SOURCES EXCLUDE REGEX ".*/(freebsd|macosx|windows|host)/.*")
list(FILTER PCL_SOURCES EXCLUDE REGEX ".*/Win32Exception\\.cpp$")
list(FILTER PCL_SOURCES EXCLUDE REGEX ".*/GetVolumeAvailableCapacity\\.mm$")

# Build shared library (PixInsight module)
add_library(BayesianAstro-pxm SHARED ${SOURCES})

# Compile flags matching PixInsight's build
target_compile_options(BayesianAstro-pxm PRIVATE
    -m64
    -march=x86-64
    -mtune=znver3
    -mfpmath=sse
    -msse4.2
    -minline-all-stringops
    -O3
    -ffast-math
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fnon-call-exceptions
    -fPIC
    -pthread
    -Wno-deprecated-declarations
)

target_compile_definitions(BayesianAstro-pxm PRIVATE
    _REENTRANT
    __PCL_LINUX
    __PCL_BUILDING_SHARED_LIBRARY
)

target_include_directories(BayesianAstro-pxm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PCLINCDIR}
    ${JULIA_INCLUDE_DIR}
)

# Link options
target_link_options(BayesianAstro-pxm PRIVATE
    -m64
    -pthread
    -rdynamic
    -Wl,-z,noexecstack
    -Wl,-O1
    -Wl,--gc-sections
)

target_link_directories(BayesianAstro-pxm PRIVATE
    ${PCLLIBDIR}
    ${JULIA_LIB_DIR}
)

target_link_libraries(BayesianAstro-pxm PRIVATE
    PCL-pxi
    julia
    -Wl,-Bsymbolic
)

# Output settings
set_target_properties(BayesianAstro-pxm PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Install rule
install(TARGETS BayesianAstro-pxm
    LIBRARY DESTINATION "${PCLDIR}/bin"
)
