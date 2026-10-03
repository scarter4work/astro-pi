# Research drivers.  Built only when NUKEX_BUILD_RESEARCH_TOOLS=ON, so a
# release build never sees them.
#
# spike_stack runs StackingEngine over a real corpus with NO PixInsight at
# all -- which is the point: a measurement must never borrow
# /opt/PixInsight/bin out from under a live imaging session.  The build
# command used to live only in a session transcript, and was lost with it;
# it is a CMake target now so that cannot happen again.
add_executable(spike_stack spike_stack.cpp)
target_link_libraries(spike_stack PRIVATE nukex4_stacker)
