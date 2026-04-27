# PackageAndSign.cmake — `make sign` / `make package` targets.
#
# The mechanical work (sign .so, stage, tarball, SHA1, update xri, re-sign)
# is in tools/release.sh so the logic is shell-testable without going
# through CMake.  This file just wires the CMake targets to the script
# and makes sure the module is built first.

add_custom_target(sign
    COMMAND env NUKEX_BUILD_DIR=${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/tools/release.sh sign
    DEPENDS NukeX-pxm
    COMMENT "Signing NukeX-pxm.so in the build tree"
    VERBATIM
)

add_custom_target(package
    COMMAND env NUKEX_BUILD_DIR=${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/tools/release.sh package
    DEPENDS NukeX-pxm
    COMMENT "Sign module, stage to repository/bin/, create dated tarball, update + re-sign updates.xri"
    VERBATIM
)
