# Third-party sources NukeX builds from: committed archives under
# third_party/archives/, never the network.
#
#   nukex_declare_vendored_archive(<FetchContent name> <archive file name>)
#
# Declares <name> with FetchContent from the local archive, verified by CMake
# against the SHA-256 recorded for it in third_party/archives/SHA256SUMS. A
# missing archive, a missing SHA256SUMS entry, or a hash mismatch is a
# configure-time error; there is no download fallback. Where each archive comes
# from and how to re-verify it against upstream: third_party/archives/README.md.
#
# Included by path (not via CMAKE_MODULE_PATH) from the NukeX root, src/lib/io
# and test/standalone_io, because src/lib/io is also built without the NukeX
# root (test/standalone_io, the Astro Studio engine bridge).
include_guard(GLOBAL)
include(FetchContent)

function(nukex_declare_vendored_archive name archive)
    set(dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/archives")
    cmake_path(NORMAL_PATH dir)
    set(path "${dir}/${archive}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "NukeX: vendored archive ${path} is missing (NukeX never downloads it).")
    endif()
    file(STRINGS "${dir}/SHA256SUMS" lines)
    set(hash "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^([0-9a-f]+)  (.+)$" AND CMAKE_MATCH_2 STREQUAL archive)
            set(hash "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    string(LENGTH "${hash}" len)
    if(NOT len EQUAL 64)
        message(FATAL_ERROR "NukeX: no SHA-256 for ${archive} in ${dir}/SHA256SUMS.")
    endif()
    FetchContent_Declare(${name}
        URL      "${path}"
        URL_HASH SHA256=${hash})
endfunction()
