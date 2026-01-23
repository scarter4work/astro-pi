# NukeX - Intelligent Region-Aware Stretch for PixInsight
# Makefile for Linux/macOS
#
# Copyright (c) 2026 Scott Carter
# MIT License

# ============================================================================
# Configuration - Adjust these paths for your system
# ============================================================================

# PixInsight installation directory
# Typical locations:
#   Linux:  /opt/PixInsight
#   macOS:  /Applications/PixInsight/PixInsight.app/Contents
PIXINSIGHT_DIR ?= /opt/PixInsight

# PCL SDK directory (contains include/ and lib/)
PCLDIR ?= $(HOME)/PCL

# PCL SDK include directory
PCL_INCDIR = $(PIXINSIGHT_DIR)/include

# PCL SDK library directory (static libraries for linking)
PCL_LIBDIR = $(PCLDIR)/lib/x64

# ONNX Runtime directory (optional, for AI segmentation)
# Leave empty to auto-detect system installation
ONNX_DIR ?=

# Auto-detect system ONNX Runtime if not specified
ifeq ($(ONNX_DIR),)
    ONNX_SYSTEM_LIB := $(wildcard /usr/lib64/libonnxruntime.so*)
    ONNX_SYSTEM_HDR := $(wildcard /usr/include/onnxruntime/onnxruntime_cxx_api.h)
    ifneq ($(ONNX_SYSTEM_LIB),)
        ifneq ($(ONNX_SYSTEM_HDR),)
            ONNX_USE_SYSTEM := 1
        endif
    endif
endif

# Output directory for the compiled module
OUTPUT_DIR = $(PIXINSIGHT_DIR)/bin

# ============================================================================
# Platform Detection
# ============================================================================

UNAME := $(shell uname -s)

ifeq ($(UNAME), Linux)
    PLATFORM = linux
    TARGET = NukeX-pxm.so
    SHARED_FLAGS = -shared
    PLATFORM_CXXFLAGS = -fPIC
    PLATFORM_LDFLAGS = -Wl,-z,defs
endif

ifeq ($(UNAME), Darwin)
    PLATFORM = macosx
    TARGET = NukeX-pxm.dylib
    SHARED_FLAGS = -dynamiclib -install_name @rpath/$(TARGET)
    PLATFORM_CXXFLAGS = -fPIC
    PLATFORM_LDFLAGS = -Wl,-undefined,error
endif

# ============================================================================
# Compiler Configuration
# ============================================================================

CXX = g++
CXXSTD = -std=c++17

# Warning flags
WARNINGS = -Wall -Wextra -Wno-unused-parameter

# Optimization flags
ifeq ($(DEBUG), 1)
    OPT_FLAGS = -g -O0 -DDEBUG
else
    OPT_FLAGS = -O3 -DNDEBUG
endif

# PCL-specific flags
PCL_CXXFLAGS = -D__PCL_$(shell echo $(PLATFORM) | tr a-z A-Z) \
               -D__PCL_BUILDING_MODULE \
               -I$(PCL_INCDIR)

# ONNX Runtime disabled (ML segmentation removed from build)
# To re-enable, uncomment below and see NukeX2 backup for ML code
ONNX_CXXFLAGS =
ONNX_LDFLAGS =
# ifneq ($(ONNX_DIR),)
#     ONNX_CXXFLAGS = -I$(ONNX_DIR)/include -DNUKEX_USE_ONNX
#     ONNX_LDFLAGS = -L$(ONNX_DIR)/lib -lonnxruntime
# else ifeq ($(ONNX_USE_SYSTEM),1)
#     ONNX_CXXFLAGS = -I/usr/include/onnxruntime -DNUKEX_USE_ONNX
#     ONNX_LDFLAGS = -lonnxruntime
# endif

# Combined flags
CXXFLAGS = $(CXXSTD) $(WARNINGS) $(OPT_FLAGS) $(PLATFORM_CXXFLAGS) \
           $(PCL_CXXFLAGS) $(ONNX_CXXFLAGS)

# PCL libraries to link (static linking)
PCL_LIBS = -lPCL-pxi -llz4-pxi -lzstd-pxi -lzlib-pxi -lRFC6234-pxi -llcms-pxi -lcminpack-pxi

LDFLAGS = $(SHARED_FLAGS) $(PLATFORM_LDFLAGS) \
          -L$(PCL_LIBDIR) \
          $(PCL_LIBS) \
          $(ONNX_LDFLAGS) \
          -lpthread

# ============================================================================
# Source Files
# ============================================================================

SRC_DIR = src
ENGINE_DIR = src/engine
ALGO_DIR = src/engine/algorithms

# Core source files
CORE_SOURCES = \
    $(SRC_DIR)/NukeXModule.cpp \
    $(SRC_DIR)/NukeXParameters.cpp \
    $(SRC_DIR)/NukeXProcess.cpp \
    $(SRC_DIR)/NukeXInstance.cpp \
    $(SRC_DIR)/NukeXInterface.cpp \
    $(SRC_DIR)/NukeXStackParameters.cpp \
    $(SRC_DIR)/NukeXStackProcess.cpp \
    $(SRC_DIR)/NukeXStackInstance.cpp \
    $(SRC_DIR)/NukeXStackInterface.cpp

# Engine source files (excluding ML segmentation for simplified version)
ENGINE_SOURCES = \
    $(filter-out $(ENGINE_DIR)/ONNXInference.cpp $(ENGINE_DIR)/SegmentationModel.cpp $(ENGINE_DIR)/Segmentation.cpp, \
    $(wildcard $(ENGINE_DIR)/*.cpp))

# Algorithm source files (add as they are implemented)
ALGO_SOURCES = \
    $(wildcard $(ALGO_DIR)/*.cpp)

# All source files
SOURCES = $(CORE_SOURCES) $(ENGINE_SOURCES) $(ALGO_SOURCES)

# Object files
OBJECTS = $(SOURCES:.cpp=.o)

# Dependency files
DEPS = $(SOURCES:.cpp=.d)

# ============================================================================
# Build Targets
# ============================================================================

.PHONY: all clean install uninstall debug release info help

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Compile source files
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# Debug build
debug:
	$(MAKE) DEBUG=1

# Release build (default)
release:
	$(MAKE) DEBUG=0

# Install to PixInsight library directory
install: $(TARGET)
	@echo "Installing $(TARGET) to $(OUTPUT_DIR)..."
	@mkdir -p $(OUTPUT_DIR)
	cp $(TARGET) $(OUTPUT_DIR)/
	@echo "Installation complete."
	@echo "Restart PixInsight to load the module."

# Uninstall from PixInsight
uninstall:
	@echo "Removing $(TARGET) from $(OUTPUT_DIR)..."
	rm -f $(OUTPUT_DIR)/$(TARGET)
	@echo "Uninstallation complete."

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(OBJECTS) $(DEPS) $(TARGET)
	# Also remove excluded segmentation files in case they exist from old builds
	rm -f $(ENGINE_DIR)/ONNXInference.o $(ENGINE_DIR)/SegmentationModel.o $(ENGINE_DIR)/Segmentation.o
	rm -f $(ENGINE_DIR)/ONNXInference.d $(ENGINE_DIR)/SegmentationModel.d $(ENGINE_DIR)/Segmentation.d
	@echo "Clean complete."

# Display build information
info:
	@echo "NukeX Build Configuration"
	@echo "========================="
	@echo "Platform:        $(PLATFORM)"
	@echo "Target:          $(TARGET)"
	@echo "Compiler:        $(CXX)"
	@echo "C++ Standard:    $(CXXSTD)"
	@echo "PixInsight:      $(PIXINSIGHT_DIR)"
	@echo "PCL Include:     $(PCL_INCDIR)"
	@echo "PCL Library:     $(PCL_LIBDIR)"
ifneq ($(ONNX_DIR),)
	@echo "ONNX Runtime:    $(ONNX_DIR)"
else ifeq ($(ONNX_USE_SYSTEM),1)
	@echo "ONNX Runtime:    System (/usr/lib64)"
else
	@echo "ONNX Runtime:    Not configured (install onnxruntime-devel)"
endif
	@echo "Output Dir:      $(OUTPUT_DIR)"
	@echo ""
	@echo "Source Files:"
	@for src in $(SOURCES); do echo "  $$src"; done

# Help information
help:
	@echo "NukeX Build System"
	@echo "=================="
	@echo ""
	@echo "Usage: make [target] [options]"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build the module (default)"
	@echo "  debug     - Build with debug symbols"
	@echo "  release   - Build optimized release"
	@echo "  install   - Install to PixInsight library"
	@echo "  uninstall - Remove from PixInsight library"
	@echo "  clean     - Remove build artifacts"
	@echo "  info      - Display build configuration"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Options:"
	@echo "  PIXINSIGHT_DIR=/path  - PixInsight installation path"
	@echo "  ONNX_DIR=/path        - ONNX Runtime installation path"
	@echo "  DEBUG=1               - Enable debug build"
	@echo ""
	@echo "Examples:"
	@echo "  make"
	@echo "  make PIXINSIGHT_DIR=/opt/PixInsight"
	@echo "  make install"
	@echo "  make clean"
