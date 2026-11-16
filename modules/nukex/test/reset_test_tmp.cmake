# ctest fixture setup (nukex_test_tmp): empties the tests' scratch directory.
# A test killed by a timeout or a signal never runs its own cleanup, so what it
# left is removed here, before the next run starts.
if(NOT DIR)
    message(FATAL_ERROR "reset_test_tmp.cmake: DIR is not set")
endif()
file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
