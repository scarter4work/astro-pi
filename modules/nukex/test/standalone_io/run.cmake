# cmake -P driver for the test_fits_concurrent_standalone ctest: configure and
# build test/standalone_io (offline, from the parent build's fetched sources),
# then run its test binary. Any step failing fails the test.
#
# Inputs: NUKEX_ROOT, BIN, C_COMPILER, CXX_COMPILER, BUILD_TYPE,
#         CFITSIO_SRC, JSON_SRC.
foreach(v NUKEX_ROOT BIN C_COMPILER CXX_COMPILER BUILD_TYPE CFITSIO_SRC JSON_SRC)
    if(NOT DEFINED ${v} OR "${${v}}" STREQUAL "")
        message(FATAL_ERROR "run.cmake: ${v} not set")
    endif()
endforeach()

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S ${NUKEX_ROOT}/test/standalone_io -B ${BIN}
        -DNUKEX_ROOT=${NUKEX_ROOT}
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE}
        -DCMAKE_C_COMPILER=${C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
        -DFETCHCONTENT_SOURCE_DIR_CFITSIO=${CFITSIO_SRC}
        -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${JSON_SRC}
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "standalone io: configure failed (${rc})")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${BIN} --target test_fits_concurrent_standalone --parallel 8
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "standalone io: build failed (${rc})")
endif()

execute_process(
    COMMAND ${BIN}/test_fits_concurrent_standalone
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "standalone io: test_fits_concurrent_standalone failed (${rc})")
endif()
