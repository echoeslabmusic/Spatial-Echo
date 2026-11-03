# The HRTF data is embedded in the binary (juce_add_binary_data), so bundles
# carry only the licence texts.
set(SPATIAL_ECHO_LICENSE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/licenses/APACHE-2.0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/licenses/HUTUBS.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/licenses/THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/licenses/VST3_SDK_MIT_LICENSE.txt")

function(spatial_echo_copy_bundle_resources bundle_target)
    if(NOT TARGET "${bundle_target}")
        return()
    endif()

    if(APPLE)
        set(resources_dir "$<TARGET_BUNDLE_DIR:${bundle_target}>/Contents/Resources")
    elseif(WIN32 AND bundle_target MATCHES "_VST3$")
        # Windows VST3 bundles share the macOS Contents/Resources layout. JUCE
        # stores the artefact path as an unevaluated generator expression.
        set(resources_dir "$<GENEX_EVAL:$<TARGET_PROPERTY:${bundle_target},JUCE_PLUGIN_ARTEFACT_FILE>>/Contents/Resources")
    elseif(WIN32)
        set(resources_dir "$<TARGET_FILE_DIR:${bundle_target}>/Resources")
    else()
        return()
    endif()

    add_custom_command(TARGET "${bundle_target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${resources_dir}/Licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${SPATIAL_ECHO_LICENSE_FILES} "${resources_dir}/Licenses"
        VERBATIM)
endfunction()

function(spatial_echo_add_resource_copy_targets base_target)
    spatial_echo_copy_bundle_resources("${base_target}_Standalone")
    spatial_echo_copy_bundle_resources("${base_target}_VST3")
    spatial_echo_copy_bundle_resources("${base_target}_AU")
endfunction()
