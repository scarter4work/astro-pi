# Catch2 amalgamated, shared with the NukeX module
set(GDG_CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../nukex/third_party/catch2")
add_library(catch2_main STATIC "${GDG_CATCH2_DIR}/catch_amalgamated.cpp")
target_include_directories(catch2_main PUBLIC "${GDG_CATCH2_DIR}")

# Absolute path of the committed goldens, for tests that read them.
set(GDG_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden")

function(gdg_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE catch2_main ${ARGN})
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/util" "${CMAKE_CURRENT_SOURCE_DIR}/unit" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_definitions(${test_name} PRIVATE GDG_GOLDEN_DIR="${GDG_GOLDEN_DIR}")
    add_test(NAME ${test_name}
             COMMAND ${test_name} --allow-running-no-tests
             WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

gdg_add_test(test_npy unit/test_npy.cpp)
gdg_add_test(test_gaussian unit/test_gaussian.cpp gdg_core)
gdg_add_test(test_orderstats unit/test_orderstats.cpp gdg_core)
gdg_add_test(test_sigmaclip unit/test_sigmaclip.cpp gdg_core)
gdg_add_test(test_morphology unit/test_morphology.cpp gdg_core)
gdg_add_test(test_daofind unit/test_daofind.cpp gdg_core)
gdg_add_test(test_transform unit/test_transform.cpp gdg_core)
gdg_add_test(test_halo unit/test_halo.cpp gdg_core)
gdg_add_test(test_render unit/test_render.cpp gdg_core)
gdg_add_test(test_nebula unit/test_nebula.cpp gdg_core)
gdg_add_test(test_display unit/test_display.cpp gdg_core)

# GaiaDepthGradeVersion.h is PCL-free (header only): the version release.sh reads.
gdg_add_test(test_version unit/test_version.cpp)

# Recon core (Task 18)
gdg_add_test(test_recon_model unit/test_recon_model.cpp gdg_core)
gdg_add_test(test_recon_channels unit/test_recon_channels.cpp gdg_core)
gdg_add_test(test_recon_ionization unit/test_recon_ionization.cpp gdg_core)
gdg_add_test(test_recon_ionizer unit/test_recon_ionizer.cpp gdg_core)
gdg_add_test(test_recon_extinction unit/test_recon_extinction.cpp gdg_core)
gdg_add_test(test_recon_cavity_terms unit/test_recon_cavity_terms.cpp gdg_core)
