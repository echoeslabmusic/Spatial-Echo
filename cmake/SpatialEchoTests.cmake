# CTest registration for the headless runner: one CTest test per default-suite
# entry. CTest asks the built runner for the list (`--list`) each time it
# loads the tests, so the registry in
# plugin/Source/Tests/Headless/HeadlessTestRunner.cpp stays the only list.

enable_testing()

set(SPATIAL_ECHO_CTEST_DIR "${CMAKE_BINARY_DIR}/ctest")

# Runner path per configuration, read by the include file below.
file(GENERATE OUTPUT "${SPATIAL_ECHO_CTEST_DIR}/runner-$<CONFIG>.txt"
     CONTENT "$<TARGET_FILE:SpatialEchoHeadlessTests>")

# Single-configuration generators run CTest without -C; fall back to the
# configured build type there.
file(WRITE "${SPATIAL_ECHO_CTEST_DIR}/include.cmake" "\
if(NOT CTEST_CONFIGURATION_TYPE)
    set(CTEST_CONFIGURATION_TYPE \"${CMAKE_BUILD_TYPE}\")
endif()
set(runner_file \"${SPATIAL_ECHO_CTEST_DIR}/runner-\${CTEST_CONFIGURATION_TYPE}.txt\")
if(EXISTS \"\${runner_file}\")
    file(READ \"\${runner_file}\" RUNNER)
endif()
if(RUNNER AND EXISTS \"\${RUNNER}\")
    include(\"${CMAKE_CURRENT_LIST_DIR}/SpatialEchoTestList.cmake\")
else()
    add_test(SpatialEchoHeadlessTests_NOT_BUILT \"${CMAKE_COMMAND}\" -E echo
        \"Build SpatialEchoHeadlessTests (\${CTEST_CONFIGURATION_TYPE}) before running CTest\")
    set_tests_properties(SpatialEchoHeadlessTests_NOT_BUILT PROPERTIES WILL_FAIL TRUE)
endif()
")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${SPATIAL_ECHO_CTEST_DIR}/include.cmake")
