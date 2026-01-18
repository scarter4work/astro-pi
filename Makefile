# NukeX Makefile for Linux
# PixInsight PCL Module Build System

# ============================================================================
# Configuration
# ============================================================================

MODULE_NAME := NukeX
MODULE_VERSION := 1.0.0

# PixInsight paths
PCLDIR := /opt/PixInsight
PCLINCDIR := $(PCLDIR)/include
PCLLIBDIR := $(PCLDIR)/bin

# ONNX Runtime (optional - set to 0 to disable)
USE_ONNX := 1
ONNXINCDIR := /usr/include/onnxruntime
ONNXLIBDIR := /usr/lib64

# Compiler
CXX := g++
CXXSTD := -std=c++17

# ============================================================================
# Source Files
# ============================================================================

# Main module sources
MAIN_SOURCES := \
	src/NukeXModule.cpp \
	src/NukeXProcess.cpp \
	src/NukeXInstance.cpp \
	src/NukeXInterface.cpp \
	src/NukeXParameters.cpp \
	src/NukeXStackProcess.cpp \
	src/NukeXStackInstance.cpp \
	src/NukeXStackInterface.cpp \
	src/NukeXStackParameters.cpp

# Engine sources
ENGINE_SOURCES := \
	src/engine/BlendEngine.cpp \
	src/engine/Compositor.cpp \
	src/engine/DistributionFitter.cpp \
	src/engine/HistogramEngine.cpp \
	src/engine/IStretchAlgorithm.cpp \
	src/engine/LayerStack.cpp \
	src/engine/LRGBProcessor.cpp \
	src/engine/ONNXInference.cpp \
	src/engine/PixelSelector.cpp \
	src/engine/PixelStackAnalyzer.cpp \
	src/engine/PSFModel.cpp \
	src/engine/RegionAnalyzer.cpp \
	src/engine/RegionStatistics.cpp \
	src/engine/Segmentation.cpp \
	src/engine/SegmentationModel.cpp \
	src/engine/SelectionRules.cpp \
	src/engine/StretchLibrary.cpp \
	src/engine/StretchSelector.cpp \
	src/engine/ToneMapper.cpp \
	src/engine/TransitionChecker.cpp

# Algorithm sources
ALGO_SOURCES := \
	src/engine/algorithms/ArcSinhStretch.cpp \
	src/engine/algorithms/GHStretch.cpp \
	src/engine/algorithms/HistogramStretch.cpp \
	src/engine/algorithms/LogStretch.cpp \
	src/engine/algorithms/LumptonStretch.cpp \
	src/engine/algorithms/MTFStretch.cpp \
	src/engine/algorithms/OTSStretch.cpp \
	src/engine/algorithms/PhotometricStretch.cpp \
	src/engine/algorithms/RNCStretch.cpp \
	src/engine/algorithms/SASStretch.cpp \
	src/engine/algorithms/StatisticalAutoStretch.cpp \
	src/engine/algorithms/VeraluxStretch.cpp

SOURCES := $(MAIN_SOURCES) $(ENGINE_SOURCES) $(ALGO_SOURCES)
OBJECTS := $(SOURCES:.cpp=.o)
DEPS := $(SOURCES:.cpp=.d)

# Output
TARGET := $(MODULE_NAME)-pxm.so

# ============================================================================
# Compiler Flags (matching PixInsight's build configuration)
# ============================================================================

# Architecture flags
ARCHFLAGS := \
	-m64 \
	-march=x86-64 \
	-mtune=znver3 \
	-mfpmath=sse \
	-msse4.2 \
	-minline-all-stringops

# Optimization
OPTFLAGS := -O3 -ffast-math

# Warning flags
WARNFLAGS := \
	-Wall \
	-Wno-deprecated-declarations \
	-Wno-unknown-pragmas \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable

# Visibility and PIC
VISFLAGS := \
	-fvisibility=hidden \
	-fvisibility-inlines-hidden \
	-fPIC

# Other flags
MISCFLAGS := \
	-fnon-call-exceptions \
	-pthread

# Preprocessor definitions
DEFINES := \
	-D_REENTRANT \
	-D__PCL_LINUX \
	-D__PCL_BUILDING_PIXINSIGHT_MODULE

# Include paths
INCLUDES := \
	-I$(PCLINCDIR) \
	-Isrc \
	-Isrc/engine \
	-Isrc/engine/algorithms

# ONNX Runtime support
ifeq ($(USE_ONNX),1)
DEFINES += -DNUKEX_USE_ONNX
INCLUDES += -I$(ONNXINCDIR)
endif

# Combined compile flags
CXXFLAGS := $(CXXSTD) $(ARCHFLAGS) $(OPTFLAGS) $(WARNFLAGS) $(VISFLAGS) $(MISCFLAGS) $(DEFINES) $(INCLUDES)

# ============================================================================
# Linker Flags
# ============================================================================

LDFLAGS := \
	-m64 \
	-shared \
	-pthread \
	-rdynamic \
	-Wl,-z,noexecstack \
	-Wl,-O1 \
	-Wl,--gc-sections \
	-L$(PCLLIBDIR)

# PCL symbols are resolved at runtime from PixInsight executable
LIBS := -Wl,-Bsymbolic

ifeq ($(USE_ONNX),1)
LDFLAGS += -L$(ONNXLIBDIR)
LIBS += -lonnxruntime
endif

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean install sign debug info

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)
	@echo "Build complete: $(TARGET)"

# Pattern rule for object files with dependency generation
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Clean build artifacts
clean:
	@echo "Cleaning..."
	rm -f $(OBJECTS) $(DEPS) $(TARGET)
	rm -f *.xsgn
	@echo "Clean complete."

# Debug build (with symbols, no optimization)
debug: OPTFLAGS := -O0 -g -DDEBUG
debug: clean all

# Print configuration info
info:
	@echo "Module:      $(MODULE_NAME) v$(MODULE_VERSION)"
	@echo "Target:      $(TARGET)"
	@echo "PCL Dir:     $(PCLDIR)"
	@echo "ONNX:        $(USE_ONNX)"
	@echo "Compiler:    $(CXX)"
	@echo "Sources:     $(words $(SOURCES)) files"

# ============================================================================
# Installation and Signing
# ============================================================================

# Sign the module
sign: $(TARGET)
	@echo "Signing module..."
	$(PCLDIR)/bin/PixInsight.sh \
		--sign-module-file=$(TARGET) \
		--xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
		--xssk-password="***REDACTED***"
	@echo "Module signed: $(MODULE_NAME)-pxm.xsgn"

# Install to PixInsight (requires sudo)
install: $(TARGET) sign
	@echo "Installing to $(PCLLIBDIR)..."
	sudo cp $(TARGET) $(PCLLIBDIR)/
	sudo cp $(MODULE_NAME)-pxm.xsgn $(PCLLIBDIR)/
	@echo "Installation complete."
	@echo "Restart PixInsight to load the module."

# Install without signing (for testing)
install-unsigned: $(TARGET)
	@echo "Installing unsigned module to $(PCLLIBDIR)..."
	sudo cp $(TARGET) $(PCLLIBDIR)/
	@echo "WARNING: Module is not signed. PixInsight may not load it."

# ============================================================================
# Development helpers
# ============================================================================

# Quick rebuild and install
rebuild: clean all install

# Check for missing includes
check-includes:
	@echo "Checking PCL includes..."
	@test -d $(PCLINCDIR)/pcl || (echo "ERROR: PCL includes not found at $(PCLINCDIR)" && exit 1)
	@echo "PCL includes: OK"
ifeq ($(USE_ONNX),1)
	@echo "Checking ONNX includes..."
	@test -f $(ONNXINCDIR)/onnxruntime_cxx_api.h || (echo "ERROR: ONNX includes not found at $(ONNXINCDIR)" && exit 1)
	@echo "ONNX includes: OK"
endif

# Count lines of code
loc:
	@echo "Lines of code:"
	@wc -l $(SOURCES) | tail -1
