# NukeX Test Suite
# Usage: make test         (build and run all tests)
#        make test-unit     (unit tests only)
#        make test-component (component tests only)
#        make test-integration (integration tests, requires QNAP mount)
#        make build-tests   (build without running)

PIXINSIGHT_DIR ?= /opt/PixInsight
PCLDIR ?= $(HOME)/PCL
PCL_INCDIR = $(PIXINSIGHT_DIR)/include
PCL_LIBDIR = $(PCLDIR)/lib/x64

CXX = g++
CXXSTD = -std=c++17

WARNINGS = -Wall -Wextra -Wno-unused-parameter
OPT_FLAGS = -g -O1

PCL_CXXFLAGS = -D__PCL_LINUX \
               -D_REENTRANT \
               -I$(PCL_INCDIR) \
               -fPIC \
               -fnon-call-exceptions

# ONNX Runtime (optional)
ONNX_SYSTEM_LIB := $(wildcard /usr/lib64/libonnxruntime.so*)
ONNX_SYSTEM_HDR := $(wildcard /usr/include/onnxruntime/onnxruntime_cxx_api.h)
ifneq ($(ONNX_SYSTEM_LIB),)
    ifneq ($(ONNX_SYSTEM_HDR),)
        ONNX_CXXFLAGS = -I/usr/include/onnxruntime -DNUKEX_USE_ONNX
        ONNX_LDFLAGS = -lonnxruntime
    endif
endif

CXXFLAGS = $(CXXSTD) $(WARNINGS) $(OPT_FLAGS) $(PCL_CXXFLAGS) $(ONNX_CXXFLAGS) \
           -I../src -I../src/engine -I../src/engine/algorithms -Icatch2 -I. -fopenmp

PCL_LIBS = -lPCL-pxi -llz4-pxi -lzstd-pxi -lzlib-pxi -lRFC6234-pxi -llcms-pxi -lcminpack-pxi

LDFLAGS = -L$(PCL_LIBDIR) $(PCL_LIBS) $(ONNX_LDFLAGS) -fopenmp -lpthread

# Source directories
SRC = ../src
ENGINE = ../src/engine
ALGO = ../src/engine/algorithms

# Engine sources (everything we want to test)
ENGINE_SRCS = $(ENGINE)/DistributionFitter.cpp \
              $(ENGINE)/PixelStackAnalyzer.cpp \
              $(ENGINE)/RegionStatistics.cpp \
              $(ENGINE)/RegionAnalyzer.cpp \
              $(ENGINE)/HistogramEngine.cpp \
              $(ENGINE)/PixelSelector.cpp \
              $(ENGINE)/SelectionRules.cpp \
              $(ENGINE)/BlendEngine.cpp \
              $(ENGINE)/FrameRegistration.cpp \
              $(ENGINE)/FrameStreamer.cpp \
              $(ENGINE)/BayerDemosaic.cpp \
              $(ENGINE)/Compositor.cpp \
              $(ENGINE)/IStretchAlgorithm.cpp \
              $(ENGINE)/StretchLibrary.cpp \
              $(ENGINE)/StretchSelector.cpp \
              $(ENGINE)/ToneMapper.cpp \
              $(ENGINE)/TransitionChecker.cpp \
              $(ENGINE)/PSFModel.cpp \
              $(ENGINE)/LRGBProcessor.cpp

# Stretch algorithm sources
ALGO_SRCS = $(wildcard $(ALGO)/*.cpp)

# Segmentation sources (conditional on ONNX)
ifneq ($(ONNX_CXXFLAGS),)
ENGINE_SRCS += $(ENGINE)/ONNXInference.cpp \
               $(ENGINE)/SegmentationModel.cpp \
               $(ENGINE)/Segmentation.cpp
endif

# Object files for engine code
ENGINE_OBJS = $(ENGINE_SRCS:.cpp=.test.o)
ALGO_OBJS = $(ALGO_SRCS:.cpp=.test.o)

# Catch2 object
CATCH_OBJ = catch2/catch_amalgamated.o

# Test sources
UNIT_SRCS = $(wildcard unit/*.cpp)
COMPONENT_SRCS = $(wildcard component/*.cpp)
INTEGRATION_SRCS = $(wildcard integration/*.cpp)
HELPER_SRCS = $(wildcard helpers/*.cpp)

UNIT_OBJS = $(UNIT_SRCS:.cpp=.o)
COMPONENT_OBJS = $(COMPONENT_SRCS:.cpp=.o)
INTEGRATION_OBJS = $(INTEGRATION_SRCS:.cpp=.o)
HELPER_OBJS = $(HELPER_SRCS:.cpp=.o)

# Test binaries
TEST_BIN = nukex_tests
UNIT_BIN = nukex_unit_tests
COMPONENT_BIN = nukex_component_tests
INTEGRATION_BIN = nukex_integration_tests

ALL_TEST_OBJS = $(UNIT_OBJS) $(COMPONENT_OBJS) $(INTEGRATION_OBJS) $(HELPER_OBJS)
SHARED_OBJS = $(ENGINE_OBJS) $(ALGO_OBJS) $(CATCH_OBJ) $(HELPER_OBJS)

.PHONY: all test test-unit test-component test-integration build-tests clean

all: build-tests

# Build all test binaries
build-tests: $(TEST_BIN)

$(TEST_BIN): $(UNIT_OBJS) $(COMPONENT_OBJS) $(INTEGRATION_OBJS) $(SHARED_OBJS)
	@echo "Linking $(TEST_BIN)..."
	$(CXX) $^ -o $@ $(LDFLAGS)

# Compile rules
%.test.o: %.cpp
	@echo "Compiling (engine) $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

catch2/catch_amalgamated.o: catch2/catch_amalgamated.cpp
	@echo "Compiling Catch2..."
	$(CXX) $(CXXSTD) -Icatch2 -c $< -o $@

unit/%.o: unit/%.cpp
	@echo "Compiling (unit) $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

component/%.o: component/%.cpp
	@echo "Compiling (component) $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

integration/%.o: integration/%.cpp
	@echo "Compiling (integration) $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

helpers/%.o: helpers/%.cpp
	@echo "Compiling (helper) $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Run targets
test: $(TEST_BIN)
	@echo "Running all tests..."
	./$(TEST_BIN) --order rand

test-unit: $(TEST_BIN)
	@echo "Running unit tests..."
	./$(TEST_BIN) "[unit]" --order rand

test-component: $(TEST_BIN)
	@echo "Running component tests..."
	./$(TEST_BIN) "[component]" --order rand

test-integration: $(TEST_BIN)
	@echo "Running integration tests..."
	./$(TEST_BIN) "[integration]" --order rand

clean:
	rm -f $(ALL_TEST_OBJS) $(ENGINE_OBJS) $(ALGO_OBJS) $(CATCH_OBJ) $(TEST_BIN)
	rm -f unit/*.o component/*.o integration/*.o helpers/*.o
