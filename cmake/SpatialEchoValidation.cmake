find_program(SPATIAL_ECHO_PLUGINVAL_EXECUTABLE pluginval)

if(TARGET SpatialEcho_VST3 AND SPATIAL_ECHO_PLUGINVAL_EXECUTABLE)
    add_custom_target(SpatialEchoValidateVST3
        COMMAND "${SPATIAL_ECHO_PLUGINVAL_EXECUTABLE}" --strictness-level 5 "$<TARGET_BUNDLE_DIR:SpatialEcho_VST3>"
        DEPENDS SpatialEcho_VST3
        VERBATIM)
endif()

if(APPLE AND TARGET SpatialEcho_AU)
    # auval uses the installed AU registry, not the build-tree bundle. The
    # official AU must be installed separately before running this target.
    add_custom_target(SpatialEchoValidateAU
        COMMAND auval -strict -v aufx SpEc EchL
        DEPENDS SpatialEcho_AU
        VERBATIM)
endif()
