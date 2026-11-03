cmake_minimum_required(VERSION 3.25)

project(SpatialEcho VERSION 1.0.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(SpatialEchoOptions)
include(SpatialEchoSources)
include(SpatialEchoResources)

add_subdirectory("${SPATIAL_ECHO_JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

# Embedded resources are generated at build time. Only resources actually
# referenced from code belong here.
juce_add_binary_data(SpatialEchoBinaryData
    HEADER_NAME BinaryData.h
    NAMESPACE BinaryData
    SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Source/Resources/HRTF/pp1_HRIRs_measured.sofa"
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Source/Resources/Fonts/PxPlus_IBM_VGA_9x16.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Source/Resources/Brand/echoes-lab-seal-green.svg")

juce_add_plugin(SpatialEcho
    COMPANY_NAME "Echoes Lab"
    COMPANY_WEBSITE "https://echoeslabmusic.com/lab"
    # Plug-in identity. Changing the bundle ID, manufacturer code or plug-in
    # code breaks existing host sessions.
    BUNDLE_ID "com.echoeslab.spatialecho"
    PLUGIN_NAME "Spatial Echo"
    PRODUCT_NAME "Spatial-Echo"
    DESCRIPTION "Tape echo, reverb, and spatial processing"
    PLUGIN_MANUFACTURER_CODE EchL
    PLUGIN_CODE SpEc
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    AU_EXPORT_PREFIX SpatialEchoAU
    VST3_CATEGORIES Fx
    FORMATS ${SPATIAL_ECHO_FORMATS}
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SpatialEcho PRIVATE ${SPATIAL_ECHO_SHARED_SOURCES})
target_compile_features(SpatialEcho PRIVATE cxx_std_17)
target_compile_definitions(SpatialEcho PRIVATE
    ${SPATIAL_ECHO_COMPILE_DEFINITIONS})
# Test TUs honor SPATIAL_ECHO_ENABLE_HEADLESS_TESTS (in addition to JUCE_DEBUG),
# so only define it when this configuration actually builds the headless runner;
# otherwise Release plugin binaries would compile the test suite in.
if(SPATIAL_ECHO_BUILD_HEADLESS)
    target_compile_definitions(SpatialEcho PRIVATE SPATIAL_ECHO_ENABLE_HEADLESS_TESTS=1)
endif()
target_include_directories(SpatialEcho BEFORE PRIVATE ${SPATIAL_ECHO_INCLUDE_DIRS})
target_link_libraries(SpatialEcho PRIVATE
    SpatialEchoBinaryData
    juce::juce_audio_utils
    juce::juce_audio_processors_headless
    juce::juce_dsp
    juce::juce_opengl
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

juce_generate_juce_header(SpatialEcho)
spatial_echo_add_resource_copy_targets(SpatialEcho)

if(SPATIAL_ECHO_BUILD_HEADLESS)
    add_executable(SpatialEchoHeadlessTests)
    set_target_properties(SpatialEchoHeadlessTests PROPERTIES
        OUTPUT_NAME "Spatial-Echo-HeadlessTests")

    target_sources(SpatialEchoHeadlessTests PRIVATE ${SPATIAL_ECHO_HEADLESS_MAIN})
    target_compile_features(SpatialEchoHeadlessTests PRIVATE cxx_std_17)
    target_compile_definitions(SpatialEchoHeadlessTests PRIVATE
        ${SPATIAL_ECHO_COMPILE_DEFINITIONS}
        SPATIAL_ECHO_ENABLE_HEADLESS_TESTS=1
        JucePlugin_Build_VST=0
        JucePlugin_Build_VST3=0
        JucePlugin_Build_AU=0
        JucePlugin_Build_AUv3=0
        JucePlugin_Build_AAX=0
        JucePlugin_Build_Standalone=1
        JucePlugin_Build_Unity=0
        JucePlugin_Build_LV2=0)
    target_include_directories(SpatialEchoHeadlessTests BEFORE PRIVATE
        ${SPATIAL_ECHO_INCLUDE_DIRS}
        "$<TARGET_PROPERTY:SpatialEcho,JUCE_GENERATED_SOURCES_DIRECTORY>")

    target_link_libraries(SpatialEchoHeadlessTests PRIVATE
        SpatialEcho
        SpatialEchoBinaryData
        juce::juce_audio_utils
        juce::juce_audio_processors_headless
        juce::juce_dsp
        juce::juce_opengl
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    include(SpatialEchoTests)
endif()

if(SPATIAL_ECHO_BUILD_HOST_CHECK)
    # Offline AU/VST3 host for release contracts; it loads the built
    # plug-in binary and never links the plug-in sources.
    juce_add_console_app(SpatialEchoHostCheck)
    target_sources(SpatialEchoHostCheck PRIVATE tools/PluginHostCheck/PluginHostCheck.cpp)
    target_compile_features(SpatialEchoHostCheck PRIVATE cxx_std_17)
    target_compile_definitions(SpatialEchoHostCheck PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(SpatialEchoHostCheck PRIVATE
        juce::juce_audio_processors
        "$<$<PLATFORM_ID:Darwin>:-framework CoreAudioKit>"
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

include(SpatialEchoValidation)
include(SpatialEchoPackaging)
