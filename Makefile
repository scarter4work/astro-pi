# NukeX v3 - Statistical Stacking + Stretch for PixInsight
# Makefile for Linux/macOS
#
# Copyright (c) 2026 Scott Carter

# ============================================================================
# Configuration - Adjust these paths for your system
# ============================================================================

# PixInsight installation directory
PIXINSIGHT_DIR ?= /opt/PixInsight

# PCL SDK directory (contains include/ and lib/)
PCLDIR ?= $(HOME)/PCL

# PCL SDK include directory
PCL_INCDIR = $(PIXINSIGHT_DIR)/include

# PCL SDK library directory (static libraries for linking)
PCL_LIBDIR = $(PCLDIR)/lib/x64

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

# OpenMP flags for parallel processing
OPENMP_FLAGS = -fopenmp

# Optimization flags
ifeq ($(DEBUG), 1)
    OPT_FLAGS = -g -O0 -DDEBUG
else
    OPT_FLAGS = -O3 -DNDEBUG
endif

# PCL-specific flags
PCL_CXXFLAGS = -D__PCL_$(shell echo $(PLATFORM) | tr a-z A-Z) \
               -D__PCL_BUILDING_MODULE \
               -D_REENTRANT \
               -I$(PCL_INCDIR) \
               -fvisibility=hidden \
               -fvisibility-inlines-hidden \
               -fnon-call-exceptions

# Third-party vendored include paths
THIRD_PARTY_DIR = $(CURDIR)/third_party
VENDOR_CXXFLAGS = \
    -I$(THIRD_PARTY_DIR)/xtensor/include \
    -I$(THIRD_PARTY_DIR)/xtl/include \
    -I$(THIRD_PARTY_DIR)/xsimd/include \
    -I$(THIRD_PARTY_DIR)/lbfgspp/include \
    -I$(THIRD_PARTY_DIR)/eigen \
    -I$(THIRD_PARTY_DIR)/boost_math/include \
    -I$(THIRD_PARTY_DIR)/boost_config/include \
    -I$(THIRD_PARTY_DIR)/boost_assert/include \
    -I$(THIRD_PARTY_DIR)/boost_throw_exception/include \
    -I$(THIRD_PARTY_DIR)/boost_core/include \
    -I$(THIRD_PARTY_DIR)/boost_type_traits/include \
    -I$(THIRD_PARTY_DIR)/boost_static_assert/include \
    -I$(THIRD_PARTY_DIR)/boost_mp11/include \
    -I$(THIRD_PARTY_DIR)/boost_integer/include \
    -I$(THIRD_PARTY_DIR)/boost_lexical_cast/include \
    -I$(THIRD_PARTY_DIR)/boost_predef/include \
    -DXTENSOR_USE_XSIMD

# Combined flags
CXXFLAGS = $(CXXSTD) $(WARNINGS) $(OPT_FLAGS) $(PLATFORM_CXXFLAGS) \
           $(PCL_CXXFLAGS) $(VENDOR_CXXFLAGS) $(OPENMP_FLAGS)

# PCL libraries to link (static linking)
PCL_LIBS = -lPCL-pxi -llz4-pxi -lzstd-pxi -lzlib-pxi -lRFC6234-pxi -llcms-pxi -lcminpack-pxi

LDFLAGS = $(SHARED_FLAGS) $(PLATFORM_LDFLAGS) \
          -L$(PCL_LIBDIR) \
          $(PCL_LIBS) \
          $(OPENMP_FLAGS) \
          -lpthread

# ============================================================================
# Source Files
# ============================================================================

SRC_DIR = src
ENGINE_DIR = src/engine
ALGO_DIR = src/engine/algorithms

# Core source files (added as they are implemented)
CORE_SOURCES = $(wildcard $(SRC_DIR)/*.cpp)

# Engine source files
ENGINE_SOURCES = $(wildcard $(ENGINE_DIR)/*.cpp)

# Algorithm source files
ALGO_SOURCES = $(wildcard $(ALGO_DIR)/*.cpp)

# All source files
SOURCES = $(CORE_SOURCES) $(ENGINE_SOURCES) $(ALGO_SOURCES)

# Object files
OBJECTS = $(SOURCES:.cpp=.o)

# Dependency files
DEPS = $(SOURCES:.cpp=.d)

# ============================================================================
# Build Targets
# ============================================================================

.PHONY: all clean install uninstall debug release info help test

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
	@echo "Clean complete."

# Run test suite
test:
	@echo "Running tests via CMake/CTest..."
	@mkdir -p build && cd build && cmake .. && make -j$$(nproc) && ctest --output-on-failure

# Display build information
info:
	@echo "NukeX v3 Build Configuration"
	@echo "============================="
	@echo "Platform:        $(PLATFORM)"
	@echo "Target:          $(TARGET)"
	@echo "Compiler:        $(CXX)"
	@echo "C++ Standard:    $(CXXSTD)"
	@echo "PixInsight:      $(PIXINSIGHT_DIR)"
	@echo "PCL Include:     $(PCL_INCDIR)"
	@echo "PCL Library:     $(PCL_LIBDIR)"
	@echo "Output Dir:      $(OUTPUT_DIR)"
	@echo ""
	@echo "Source Files:"
	@for src in $(SOURCES); do echo "  $$src"; done

# Help information
help:
	@echo "NukeX v3 Build System"
	@echo "====================="
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
	@echo "  test      - Run test suite via CTest"
	@echo "  info      - Display build configuration"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Options:"
	@echo "  PIXINSIGHT_DIR=/path  - PixInsight installation path"
	@echo "  PCLDIR=/path          - PCL SDK path"
	@echo "  DEBUG=1               - Enable debug build"
