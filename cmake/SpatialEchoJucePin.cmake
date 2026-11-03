# Keep dependency provenance reproducible without fetching during configure.
# Updating this revision requires a clean configure/build and affected tests.
set(SPATIAL_ECHO_JUCE_REVISION "501c07674e1ad693085a7e7c398f205c2677f5da") # JUCE 8.0.12

function(spatial_echo_verify_juce_checkout checkout expected_revision)
    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" rev-parse --show-toplevel
        RESULT_VARIABLE root_result OUTPUT_VARIABLE git_root
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT root_result EQUAL 0)
        message(FATAL_ERROR "JUCE must be a Git checkout at ${expected_revision}: ${checkout}")
    endif()
    file(REAL_PATH "${checkout}" checkout_real)
    file(REAL_PATH "${git_root}" git_root_real)
    if(NOT checkout_real STREQUAL git_root_real)
        message(FATAL_ERROR "JUCE path must be the checkout root: ${checkout}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" rev-parse HEAD
        RESULT_VARIABLE revision_result OUTPUT_VARIABLE actual_revision
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT revision_result EQUAL 0 OR NOT actual_revision STREQUAL expected_revision)
        message(FATAL_ERROR
            "Unsupported JUCE revision '${actual_revision}'. Expected ${expected_revision} (JUCE 8.0.12). Use a separate checkout; do not reset a checkout with local work.")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" status --porcelain --untracked-files=normal
        RESULT_VARIABLE status_result OUTPUT_VARIABLE checkout_status
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT status_result EQUAL 0 OR NOT checkout_status STREQUAL "")
        message(FATAL_ERROR
            "JUCE checkout has local changes or its status could not be read. Use a clean checkout at ${expected_revision}; local work will not be modified.")
    endif()
    message(STATUS "JUCE pinned revision: ${actual_revision}")
endfunction()
