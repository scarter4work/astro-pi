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

# CUDA support (optional) — check PATH first, then common install locations
NVCC_PATH := $(shell which nvcc 2>/dev/null)
ifndef NVCC_PATH
    ifneq (,$(wildcard /usr/local/cuda/bin/nvcc))
        NVCC_PATH := /usr/local/cuda/bin/nvcc
    endif
endif
ifdef NVCC_PATH
    NUKEX_HAS_CUDA = 1
    # Derive CUDA toolkit root from nvcc path (e.g. /usr/local/cuda-12.8/bin/nvcc -> /usr/local/cuda-12.8)
    CUDA_TOOLKIT_ROOT := $(realpath $(dir $(NVCC_PATH))..)
    CUDA_DIR = src/engine/cuda
    CUDA_SOURCES = $(wildcard $(CUDA_DIR)/*.cu)
    CUDA_OBJECTS = $(CUDA_SOURCES:.cu=.o)
    NVCC_FLAGS = -std=c++17 -O3 -allow-unsupported-compiler \
                 -ccbin /usr/bin/g++-14 \
                 -arch=native \
                 -Xcompiler "-fPIC -fvisibility=hidden" \
                 -I$(PCL_INCDIR) -I$(CURDIR)/src -I$(CURDIR)/src/engine \
                 -I$(CURDIR)/src/engine/algorithms \
                 -I$(CUDA_TOOLKIT_ROOT)/include \
                 -DNUKEX_HAS_CUDA -DBOOST_MATH_STANDALONE=1
    CUDA_CXXFLAGS = -DNUKEX_HAS_CUDA -I$(CUDA_TOOLKIT_ROOT)/include
    CUDA_LDFLAGS = -L$(CUDA_TOOLKIT_ROOT)/lib64 -lcudart
else
    CUDA_CXXFLAGS =
    CUDA_LDFLAGS =
endif

# Optimization flags
ifeq ($(DEBUG), 1)
    OPT_FLAGS = -g -O0 -DDEBUG
else
    OPT_FLAGS = -O3 -march=native -DNDEBUG
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

# Project include paths
PROJECT_CXXFLAGS = -I$(SRC_DIR) -I$(ENGINE_DIR) -I$(ALGO_DIR) \
                   -DBOOST_MATH_STANDALONE=1

# Combined flags
CXXFLAGS = $(CXXSTD) $(WARNINGS) $(OPT_FLAGS) $(PLATFORM_CXXFLAGS) \
           $(PCL_CXXFLAGS) $(VENDOR_CXXFLAGS) $(PROJECT_CXXFLAGS) $(OPENMP_FLAGS) \
           $(CUDA_CXXFLAGS)

# PCL libraries to link (static linking)
PCL_LIBS = -lPCL-pxi -llz4-pxi -lzstd-pxi -lzlib-pxi -lRFC6234-pxi -llcms-pxi -lcminpack-pxi

LDFLAGS = $(SHARED_FLAGS) $(PLATFORM_LDFLAGS) \
          -L$(PCL_LIBDIR) \
          $(PCL_LIBS) \
          $(OPENMP_FLAGS) \
          -lpthread \
          $(CUDA_LDFLAGS)

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

# CUDA runtime support files (compiled with g++, not nvcc)
CUDA_CPP_SOURCES = $(wildcard src/engine/cuda/*.cpp)

# All source files
SOURCES = $(CORE_SOURCES) $(ENGINE_SOURCES) $(ALGO_SOURCES) $(CUDA_CPP_SOURCES)

# Object files
OBJECTS = $(SOURCES:.cpp=.o)
ifdef NVCC_PATH
    OBJECTS += $(CUDA_OBJECTS)
endif

# Dependency files
DEPS = $(SOURCES:.cpp=.d)

# ============================================================================
# Build Targets
# ============================================================================

.PHONY: all clean install uninstall debug release info help test sign package

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Compile source files
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Compile CUDA source files
ifdef NVCC_PATH
$(CUDA_DIR)/%.o: $(CUDA_DIR)/%.cu
	@echo "Compiling CUDA $<..."
	$(NVCC_PATH) $(NVCC_FLAGS) -c $< -o $@
endif

# Include dependency files
-include $(DEPS)

# Debug build
debug:
	$(MAKE) DEBUG=1

# Release build (default)
release:
	$(MAKE) DEBUG=0

# Sign module for PixInsight
SIGN_KEYS = /home/scarter4work/projects/keys/scarter4work_keys.xssk
SIGN_PASS ?= $(shell cat /tmp/.pi_codesign_pass 2>/dev/null || echo "$$PI_SIGN_PASS")

sign: $(TARGET)
	@echo "Signing $(TARGET)..."
	$(PIXINSIGHT_DIR)/bin/PixInsight.sh \
		--sign-module-file=$(TARGET) \
		--xssk-file=$(SIGN_KEYS) \
		--xssk-password="$(SIGN_PASS)"
	@echo "Module signed."

# Install to PixInsight library directory (signs first)
install: sign
	@echo "Installing $(TARGET) to $(OUTPUT_DIR)..."
	@mkdir -p $(OUTPUT_DIR)
	cp $(TARGET) $(OUTPUT_DIR)/
	@if [ -f NukeX-pxm.xsgn ]; then cp NukeX-pxm.xsgn $(OUTPUT_DIR)/; fi
	@echo "Installation complete."
	@echo "Restart PixInsight to load the module."

# Package for distribution: build, sign, tarball, update manifest, sign XRI
REPO_DIR = repository
PKG_DATE = $(shell date +%Y%m%d)
PKG_NAME = $(PKG_DATE)-linux-x64-NukeX.tar.gz
PKG_VERSION = $(shell grep 'MODULE_VERSION_MAJOR' src/NukeXModule.cpp | head -1 | awk '{print $$3}').$(shell grep 'MODULE_VERSION_MINOR' src/NukeXModule.cpp | head -1 | awk '{print $$3}').$(shell grep 'MODULE_VERSION_REVISION' src/NukeXModule.cpp | head -1 | awk '{print $$3}').$(shell grep 'MODULE_VERSION_BUILD' src/NukeXModule.cpp | head -1 | awk '{print $$3}')
SIGN_XRI = $(REPO_DIR)/updates.xri

package: sign
	@echo "Packaging $(PKG_NAME)..."
	@rm -rf /tmp/nukex-pkg && mkdir -p /tmp/nukex-pkg/bin
	@cp $(TARGET) /tmp/nukex-pkg/bin/
	@cp NukeX-pxm.xsgn /tmp/nukex-pkg/bin/
	@cd /tmp/nukex-pkg && tar czf $(CURDIR)/$(REPO_DIR)/$(PKG_NAME) bin/
	@NEW_SHA=$$(sha1sum $(REPO_DIR)/$(PKG_NAME) | cut -d' ' -f1); \
	 echo "  SHA1: $$NEW_SHA"; \
	 sed -i "s/sha1=\"[^\"]*\"/sha1=\"$$NEW_SHA\"/" $(SIGN_XRI); \
	 sed -i "s/fileName=\"[^\"]*\"/fileName=\"$(PKG_NAME)\"/" $(SIGN_XRI); \
	 sed -i "s/releaseDate=\"[^\"]*\"/releaseDate=\"$(PKG_DATE)\"/" $(SIGN_XRI); \
	 sed -i "s|<title>NukeX [^<]*</title>|<title>NukeX $(PKG_VERSION)</title>|" $(SIGN_XRI)
	@sed -i '/<Signature developerId=/d' $(SIGN_XRI)
	$(PIXINSIGHT_DIR)/bin/PixInsight.sh \
		--sign-xml-file=$(SIGN_XRI) \
		--xssk-file=$(SIGN_KEYS) \
		--xssk-password="$(SIGN_PASS)"
	@rm -rf /tmp/nukex-pkg
	@echo "Package complete: $(REPO_DIR)/$(PKG_NAME)"
	@echo "Ready to commit and push. Run 'sudo make install' to install locally."

# Uninstall from PixInsight
uninstall:
	@echo "Removing $(TARGET) from $(OUTPUT_DIR)..."
	rm -f $(OUTPUT_DIR)/$(TARGET)
	@echo "Uninstallation complete."

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(OBJECTS) $(DEPS) $(TARGET)
ifdef NVCC_PATH
	rm -f $(CUDA_OBJECTS)
endif
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
ifdef NVCC_PATH
	@echo "CUDA:            $(NVCC_PATH)"
else
	@echo "CUDA:            Not found (GPU acceleration disabled)"
endif
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
	@echo "  sign      - Sign module for PixInsight"
	@echo "  install   - Sign and install to PixInsight (use sudo)"
	@echo "  package   - Build, sign, tarball, update manifest (for distribution)"
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
