set(SPATIAL_ECHO_JUCE_DIR "$ENV{JUCE_DIR}" CACHE PATH "Path to the JUCE source checkout")
if(NOT SPATIAL_ECHO_JUCE_DIR)
    if(DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/JUCE/CMakeLists.txt")
        set(SPATIAL_ECHO_JUCE_DIR "$ENV{HOME}/JUCE" CACHE PATH "Path to the JUCE source checkout" FORCE)
    else()
        message(FATAL_ERROR "Set JUCE_DIR or SPATIAL_ECHO_JUCE_DIR to a JUCE source checkout.")
    endif()
endif()

if(NOT EXISTS "${SPATIAL_ECHO_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SPATIAL_ECHO_JUCE_DIR does not point to a JUCE source checkout: ${SPATIAL_ECHO_JUCE_DIR}")
endif()

include(SpatialEchoJucePin)
spatial_echo_verify_juce_checkout("${SPATIAL_ECHO_JUCE_DIR}" "${SPATIAL_ECHO_JUCE_REVISION}")

set(SPATIAL_ECHO_FORMATS "Standalone" CACHE STRING "Plugin formats to build")
set(SPATIAL_ECHO_BUILD_HEADLESS ON CACHE BOOL "Build the headless test runner")
option(SPATIAL_ECHO_BUILD_HOST_CHECK "Build the offline AU/VST3 release host check" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target" FORCE)
endif()

set(SPATIAL_ECHO_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/plugin/Source")
set(SPATIAL_ECHO_INCLUDE_DIRS
    "${SPATIAL_ECHO_SOURCE_ROOT}"
    "${SPATIAL_ECHO_SOURCE_ROOT}/DSP/dependencies"
    "${SPATIAL_ECHO_SOURCE_ROOT}/DSP/dependencies/CloudSeedCore"
    "${SPATIAL_ECHO_SOURCE_ROOT}/DSP/dependencies/CloudSeedCore/DSP"
    "${SPATIAL_ECHO_SOURCE_ROOT}/DSP/dependencies/saf_utilities"
    "${SPATIAL_ECHO_SOURCE_ROOT}/DSP/dependencies/sofa/libmysofa"
    "${SPATIAL_ECHO_SOURCE_ROOT}/DSP/dependencies/sofa/libmysofa/internal"
    "${SPATIAL_ECHO_SOURCE_ROOT}/DSP/dependencies/sofa/zlib"
    "${SPATIAL_ECHO_JUCE_DIR}/modules"
    "${SPATIAL_ECHO_JUCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK"
    "${SPATIAL_ECHO_JUCE_DIR}/modules/juce_audio_plugin_client/AU")

set(SPATIAL_ECHO_COMPILE_DEFINITIONS
    # Keep the generated JuceHeader.h from injecting `using namespace juce`
    # into every file that includes it; files that want it opt in.
    DONT_SET_USING_JUCE_NAMESPACE=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    SAF_ENABLE_SOFA_READER_MODULE
    BUFFER_SIZE=1024)

if(WIN32)
    list(APPEND SPATIAL_ECHO_COMPILE_DEFINITIONS _USE_MATH_DEFINES)
endif()

if(MSVC)
    # Sources contain UTF-8 UI strings without a BOM; large translation units
    # (processor, tests) exceed the default COFF section limit.
    add_compile_options(/utf-8 /bigobj)
endif()
