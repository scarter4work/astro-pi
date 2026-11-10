# cfitsio is linked into the PixInsight module. Most Linux distros build their
# system cfitsio --with-curl, which transitively needs OPENSSL_3.2.0 symbols
# from libssl. PixInsight ships an older libssl.so.3 and prepends its bin/lib
# to LD_LIBRARY_PATH, so PI's bundled libssl wins at module load time and
# cannot satisfy those newer symbols — the module fails to load.
#
# Default path: FetchContent pulls a pinned cfitsio and builds it statically
# with --disable-curl, linked privately into nukex4_io. Hermetic, reproducible,
# no user bootstrap.
#
# Escape hatch: -DNUKEX_USE_SYSTEM_CFITSIO=ON reverts to system pkg-config
# cfitsio for fast dev iteration. Combined with -DNUKEX_RELEASE_BUILD=ON,
# this is a hard error — distribution builds must not link system cfitsio.

option(NUKEX_USE_SYSTEM_CFITSIO
       "Use system cfitsio (dev only — module will not load in PixInsight on most distros)" OFF)

if(NUKEX_USE_SYSTEM_CFITSIO)
    if(NUKEX_RELEASE_BUILD)
        message(FATAL_ERROR
            "NUKEX_USE_SYSTEM_CFITSIO=ON is incompatible with NUKEX_RELEASE_BUILD=ON. "
            "Distribution builds require the vendored cfitsio (built --disable-curl) to avoid "
            "the libcurl → libssl OPENSSL_3.2.0 symbol conflict with PixInsight's bundled OpenSSL. "
            "Either unset NUKEX_USE_SYSTEM_CFITSIO, or drop NUKEX_RELEASE_BUILD for dev-only builds.")
    endif()
    message(WARNING
        "Linking against system cfitsio — DEV BUILD ONLY. The resulting module will fail to load "
        "inside PixInsight on Fedora/most distros due to libcurl/OpenSSL ABI conflict with PI's bundled libs.")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CFITSIO REQUIRED cfitsio)
    set(_nukex_cfitsio_libs ${CFITSIO_LIBRARIES})
    set(_nukex_cfitsio_inc  ${CFITSIO_INCLUDE_DIRS})
    # The test tree links a `cfitsio` target by name (the vendored path
    # defines a real one); give the system path the same name.
    add_library(cfitsio INTERFACE IMPORTED GLOBAL)
    target_link_libraries(cfitsio INTERFACE ${CFITSIO_LIBRARIES})
    target_include_directories(cfitsio INTERFACE ${CFITSIO_INCLUDE_DIRS})
    target_compile_definitions(cfitsio INTERFACE NUKEX_SYSTEM_CFITSIO=1)
else()
    enable_language(C)
    include(FetchContent)

    set(USE_CURL          OFF CACHE BOOL "" FORCE)  # the whole point of vendoring
    set(TESTS             OFF CACHE BOOL "" FORCE)
    set(UTILS             OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # static into NukeX-pxm.so
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)         # -fPIC so objects can go in a .so

    # No network drivers. USE_CURL=OFF only drops libcurl (https); CFITSIO's own
    # http://, ftp:// and root:// drivers (drvrnet.c) are compiled whenever its
    # CMake probe finds gethostbyname() and connect(), and CFITSIO has no option
    # to turn that off. Its probes skip themselves when their result variable is
    # already cached, so pre-seeding both as FALSE leaves HAVE_NET_SERVICES
    # undefined and drvrnet.c compiles to stubs: no network code in NukeX-pxm.so.
    # (fits_reader.cpp also opens frames with fits_open_diskfile(), which never
    # parses a name as a URL.)
    set(CMAKE_HAVE_GETHOSTBYNAME FALSE CACHE INTERNAL "NukeX: no CFITSIO network drivers" FORCE)
    set(CMAKE_HAVE_CONNECT       FALSE CACHE INTERNAL "NukeX: no CFITSIO network drivers" FORCE)

    # No shared-memory driver. CFITSIO compiles drvrsmem.c and registers shmem://
    # (System V shm + semaphores, /tmp/.shmem-lockfile) whenever its
    # CHECK_C_SOURCE_COMPILES probe sets HAVE_SHMEM_SERVICES. The probe is skipped
    # when the variable is already defined, so pre-seeding it FALSE leaves
    # HAVE_SHMEM_SERVICES undefined: drvrsmem.c compiles empty and shmem:// is
    # never registered. (CFITSIO's stdin://, stdinfile:// and stream:// drivers
    # have no configure switch; they are registered unconditionally in
    # fits_init_cfitsio(). They are unreachable from NukeX, which opens frames
    # only with fits_open_diskfile().)
    set(HAVE_SHMEM_SERVICES FALSE CACHE INTERNAL "NukeX: no CFITSIO shared-memory driver" FORCE)

    FetchContent_Declare(
        cfitsio
        GIT_REPOSITORY https://github.com/HEASARC/cfitsio.git
        GIT_TAG        cfitsio-4.6.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(cfitsio)

    set(_nukex_cfitsio_libs cfitsio)
    set(_nukex_cfitsio_inc  ${cfitsio_SOURCE_DIR})
endif()

add_library(nukex4_io STATIC
    src/image.cpp
    src/fits_reader.cpp
    src/debayer.cpp
    src/flat_calibration.cpp
    src/filter_classifier.cpp
    src/filter_alias.cpp
)

target_include_directories(nukex4_io
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${_nukex_cfitsio_inc}
)

target_link_libraries(nukex4_io
    PUBLIC  nukex4_core
    PRIVATE ${_nukex_cfitsio_libs} nlohmann_json::nlohmann_json
)

target_compile_features(nukex4_io PUBLIC cxx_std_20)
