# Third-party dependencies of AutoContrast.

# nlohmann/json: header-only, same declaration as modules/pi-copilot/CMakeLists.txt.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# SQLite: the repo's vendored amalgamation (third_party/sqlite, public domain), same flags as
# modules/pi-copilot: hidden visibility (no sqlite3_* symbol leaks out of the module), no extension
# loading. Built only when something links it (the fingerprint store, a later task).
get_filename_component(AC_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(AC_SQLITE_DIR "${AC_REPO_ROOT}/third_party/sqlite")
add_library(ac_sqlite3 STATIC EXCLUDE_FROM_ALL "${AC_SQLITE_DIR}/sqlite3.c")
set_target_properties(ac_sqlite3 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    LINKER_LANGUAGE C)
target_compile_definitions(ac_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DQS=0)
target_compile_options(ac_sqlite3 PRIVATE -w)   # third-party code: its warnings are not ours to fix
target_include_directories(ac_sqlite3 PUBLIC "${AC_SQLITE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(ac_sqlite3 PUBLIC Threads::Threads m)

# ---------------------------------------------------------------------------------------------------------------
# Raster decoders (core/io/Raster.cpp): libjpeg-turbo, libpng, libtiff, libwebp.
#
# Fetched at configure time (FetchContent, URL + SHA-256: these bytes end up inside a module signed with the
# release key) and built at build time as static, position-independent archives with hidden symbols, each with
# the smallest feature set the decoders need. They are built by ExternalProject on the fetched sources because
# libjpeg-turbo's build refuses add_subdirectory() (its CMakeLists.txt says ExternalProject_Add is the supported
# integration); libpng, libtiff and libwebp use the same route so all four get identical flags and none of their
# install/test/tool targets leak into this build.
#
#   libjpeg-turbo 3.1.4.1  the release Pillow 12.3.0 bundles (PIL.features.version("libjpeg_turbo")), so the
#                          decode is the reference's own code. WITH_SIMD=OFF (no nasm in the build image); the
#                          C IDCT/upsampling/colour paths are the reference's arithmetic -- test_raster_real proves
#                          it bit for bit on all 45 renders. Arithmetic decoding on (Pillow's build has it).
#   libpng 1.6.59          8- and 16-bit PNG (pixels only; no transforms are requested).
#   libtiff 4.7.2          deflate (zlib), LZW, PackBits and the other internal codecs; no JPEG/JBIG/LERC/LZMA/
#                          ZSTD/WebP/libdeflate/PixarLog, no C++ API, no tools.
#   libwebp 1.6.0          the release Pillow 12.3.0 bundles (PIL.features.version("webp")); decoder + demux only
#                          (WebPAnimDecoder, as Pillow decodes every WebP), no encoder, tools or threads.
#
# zlib: a module (or container) build links PCL's own zlib-pxi -- the archive PCL already puts on the module's
# link line, with its headers from PCL's source tree -- so the module carries one zlib. A host test build
# (AC_BUILD_MODULE=OFF) uses the system zlib.
include(ExternalProject)

if(AC_BUILD_MODULE)
    set(AC_ZLIB_INCLUDE_DIR "${PCLDIR}/src/3rdparty/zlib")
    set(AC_ZLIB_LIBRARY "${PCL_zlib-pxi_LIBRARY}")
    if(NOT EXISTS "${AC_ZLIB_INCLUDE_DIR}/zlib.h" OR NOT EXISTS "${AC_ZLIB_LIBRARY}")
        message(FATAL_ERROR "PCL zlib not found (${AC_ZLIB_INCLUDE_DIR}/zlib.h, ${AC_ZLIB_LIBRARY})")
    endif()
else()
    find_package(ZLIB REQUIRED)
    set(AC_ZLIB_INCLUDE_DIR "${ZLIB_INCLUDE_DIRS}")
    list(GET ZLIB_LIBRARIES 0 AC_ZLIB_LIBRARY)
endif()
add_library(ac_zlib UNKNOWN IMPORTED GLOBAL)
set_target_properties(ac_zlib PROPERTIES IMPORTED_LOCATION "${AC_ZLIB_LIBRARY}"
                                         INTERFACE_INCLUDE_DIRECTORIES "${AC_ZLIB_INCLUDE_DIR}")
message(STATUS "AutoContrast raster zlib: ${AC_ZLIB_LIBRARY}")

FetchContent_Declare(ac_libjpeg_turbo
    URL      https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.4.1/libjpeg-turbo-3.1.4.1.tar.gz
    URL_HASH SHA256=ecae8008e2cc9ade2f2c1bb9d5e6d4fb73e7c433866a056bd82980741571a022
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  ac-populate-only)    # no CMakeLists.txt there: populate, never add_subdirectory
FetchContent_Declare(ac_libpng
    URL      https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz
    URL_HASH SHA256=2540302a1844ad2b2b501977abecfa850f265f97b78f065a712ab4074a89f5b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  ac-populate-only)
FetchContent_Declare(ac_libtiff
    URL      https://download.osgeo.org/libtiff/tiff-4.7.2.tar.gz
    URL_HASH SHA256=672bd7d10aee4606171afb864f3570b83340f6a33e2c186dc0512f7145ffdf6a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  ac-populate-only)
FetchContent_Declare(ac_libwebp
    URL      https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz
    URL_HASH SHA256=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  ac-populate-only)
FetchContent_MakeAvailable(ac_libjpeg_turbo ac_libpng ac_libtiff ac_libwebp)

set(AC_RASTER_PREFIX "${CMAKE_BINARY_DIR}/ac-raster-deps")
file(MAKE_DIRECTORY "${AC_RASTER_PREFIX}/include")
set(AC_RASTER_COMMON_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    "-DCMAKE_C_FLAGS=-fPIC -fvisibility=hidden -w"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_INSTALL_PREFIX=${AC_RASTER_PREFIX}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_INSTALL_INCLUDEDIR=include
    -DBUILD_SHARED_LIBS=OFF)

ExternalProject_Add(ac_libjpeg_turbo_build
    SOURCE_DIR "${ac_libjpeg_turbo_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ac-libjpeg-turbo-build"
    CMAKE_ARGS ${AC_RASTER_COMMON_ARGS}
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DREQUIRE_SIMD=OFF
        -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_JAVA=OFF
        -DWITH_ARITH_DEC=ON -DWITH_ARITH_ENC=OFF -DWITH_JPEG7=OFF -DWITH_JPEG8=OFF
    BUILD_BYPRODUCTS "${AC_RASTER_PREFIX}/lib/libjpeg.a"
    UPDATE_COMMAND "" PATCH_COMMAND "")

ExternalProject_Add(ac_libpng_build
    SOURCE_DIR "${ac_libpng_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ac-libpng-build"
    CMAKE_ARGS ${AC_RASTER_COMMON_ARGS}
        -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF
        -DZLIB_INCLUDE_DIR=${AC_ZLIB_INCLUDE_DIR} -DZLIB_LIBRARY=${AC_ZLIB_LIBRARY}
    BUILD_BYPRODUCTS "${AC_RASTER_PREFIX}/lib/libpng16.a"
    UPDATE_COMMAND "" PATCH_COMMAND "")

ExternalProject_Add(ac_libtiff_build
    SOURCE_DIR "${ac_libtiff_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ac-libtiff-build"
    CMAKE_ARGS ${AC_RASTER_COMMON_ARGS}
        -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF -Dtiff-docs=OFF -Dtiff-opengl=OFF -Dtiff-cxx=OFF
        -Dzlib=ON -Dlibdeflate=OFF -Dpixarlog=OFF -Djpeg=OFF -Dold-jpeg=OFF -Djbig=OFF -Dlerc=OFF -Dlzma=OFF
        -Dzstd=OFF -Dwebp=OFF -Dsphinx=OFF
        -DZLIB_INCLUDE_DIR=${AC_ZLIB_INCLUDE_DIR} -DZLIB_LIBRARY=${AC_ZLIB_LIBRARY}
        -DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON -DCMAKE_DISABLE_FIND_PACKAGE_Deflate=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_LibLZMA=ON -DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_WebP=ON -DCMAKE_DISABLE_FIND_PACKAGE_JBIG=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_LERC=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_GLUT=ON
    BUILD_BYPRODUCTS "${AC_RASTER_PREFIX}/lib/libtiff.a"
    UPDATE_COMMAND "" PATCH_COMMAND "")

ExternalProject_Add(ac_libwebp_build
    SOURCE_DIR "${ac_libwebp_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ac-libwebp-build"
    CMAKE_ARGS ${AC_RASTER_COMMON_ARGS}
        -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF
        -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF
        -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_FUZZTEST=OFF -DWEBP_USE_THREAD=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON
        # types.h marks the API visibility("default") unless WEBP_EXTERN is predefined: keep it hidden like the
        # other archives (ac_core compiles its webp includes with the same definition)
        "-DCMAKE_C_FLAGS=-fPIC -fvisibility=hidden -w -DWEBP_EXTERN=extern"
    BUILD_BYPRODUCTS "${AC_RASTER_PREFIX}/lib/libwebpdecoder.a" "${AC_RASTER_PREFIX}/lib/libwebpdemux.a"
    UPDATE_COMMAND "" PATCH_COMMAND "")

# libwebp: only the decoder-only archive (libwebpdecoder: VP8/VP8L decode + its DSP, runtime-dispatched SSE2/
# SSE4.1 kernels, no encoder) and libwebpdemux (WebPAnimDecoder, the API Pillow 12.3.0's WebP plugin decodes
# through) are linked; libwebp.a / libsharpyuv.a are built by its CMake but never linked.
foreach(_ac_lib jpeg png16 tiff webpdemux webpdecoder)
    add_library(ac_${_ac_lib} STATIC IMPORTED GLOBAL)
    set_target_properties(ac_${_ac_lib} PROPERTIES
        IMPORTED_LOCATION "${AC_RASTER_PREFIX}/lib/lib${_ac_lib}.a"
        INTERFACE_INCLUDE_DIRECTORIES "${AC_RASTER_PREFIX}/include")
endforeach()
add_dependencies(ac_jpeg ac_libjpeg_turbo_build)
add_dependencies(ac_png16 ac_libpng_build)
add_dependencies(ac_tiff ac_libtiff_build)
add_dependencies(ac_webpdemux ac_libwebp_build)
add_dependencies(ac_webpdecoder ac_libwebp_build)
set_property(TARGET ac_webpdemux APPEND PROPERTY INTERFACE_LINK_LIBRARIES ac_webpdecoder)
set_property(TARGET ac_webpdecoder APPEND PROPERTY INTERFACE_LINK_LIBRARIES m)
set_property(TARGET ac_png16 APPEND PROPERTY INTERFACE_LINK_LIBRARIES ac_zlib m)
set_property(TARGET ac_tiff APPEND PROPERTY INTERFACE_LINK_LIBRARIES ac_zlib m)
