# Included by CTest (see cmake/SpatialEchoTests.cmake) with RUNNER set to the
# built headless runner. Adds one CTest test per name the runner lists.

execute_process(
    COMMAND "${RUNNER}" --list
    OUTPUT_VARIABLE test_list
    RESULT_VARIABLE list_result
    ERROR_QUIET)
if(NOT list_result EQUAL 0)
    add_test(SpatialEchoHeadlessTests_LIST_FAILED "${RUNNER}" --list)
    return()
endif()

# Tests that deliberately provoke a defensive JUCE assertion; every other test
# fails if an assertion appears in its output, like scripts/run_tests.sh.
set(expected_assertion_tests
    ReverbUndersizedBufferContract
    TapeDelayUndersizedBufferContract
    ToneBufferBoundsContract)

string(REPLACE "\r" "" test_list "${test_list}")
string(REPLACE "\n" ";" test_list "${test_list}")
foreach(name IN LISTS test_list)
    if(name STREQUAL "")
        continue()
    endif()
    add_test("${name}" "${RUNNER}" --test "${name}")
    if(name IN_LIST expected_assertion_tests)
        set_tests_properties("${name}" PROPERTIES LABELS headless)
    else()
        set_tests_properties("${name}" PROPERTIES LABELS headless FAIL_REGULAR_EXPRESSION "JUCE Assertion failure")
    endif()
endforeach()
