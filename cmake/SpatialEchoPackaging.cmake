set(SPATIAL_ECHO_PACKAGE_STAGE "${CMAKE_BINARY_DIR}/package_stage" CACHE PATH "Directory for staged package artifacts")

if(APPLE)
    add_custom_target(SpatialEchoStageReleaseArtifacts
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SPATIAL_ECHO_PACKAGE_STAGE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPATIAL_ECHO_PACKAGE_STAGE}/VST3"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPATIAL_ECHO_PACKAGE_STAGE}/AU"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPATIAL_ECHO_PACKAGE_STAGE}/Standalone"
        VERBATIM)

    if(TARGET SpatialEcho_VST3)
        add_custom_command(TARGET SpatialEchoStageReleaseArtifacts POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:SpatialEcho_VST3>" "${SPATIAL_ECHO_PACKAGE_STAGE}/VST3/Spatial-Echo.vst3"
            VERBATIM)
        add_dependencies(SpatialEchoStageReleaseArtifacts SpatialEcho_VST3)
    endif()

    if(TARGET SpatialEcho_AU)
        add_custom_command(TARGET SpatialEchoStageReleaseArtifacts POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:SpatialEcho_AU>" "${SPATIAL_ECHO_PACKAGE_STAGE}/AU/Spatial-Echo.component"
            VERBATIM)
        add_dependencies(SpatialEchoStageReleaseArtifacts SpatialEcho_AU)
    endif()

    if(TARGET SpatialEcho_Standalone)
        add_custom_command(TARGET SpatialEchoStageReleaseArtifacts POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:SpatialEcho_Standalone>" "${SPATIAL_ECHO_PACKAGE_STAGE}/Standalone/Spatial-Echo.app"
            VERBATIM)
        add_dependencies(SpatialEchoStageReleaseArtifacts SpatialEcho_Standalone)
    endif()
endif()
