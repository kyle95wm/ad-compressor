cmake_minimum_required(VERSION 3.22)

# The release workflow passes -DAD_COMPRESSOR_VERSION=<tag without the leading v>,
# so the version baked into the plugin (and used by the update checker) always
# matches the GitHub Release it ships in.
set(AD_COMPRESSOR_VERSION "0.3.0" CACHE STRING "Plugin version (major.minor.patch)")

# Must be set before project() to take effect.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
endif()

project(ADCompressor VERSION ${AD_COMPRESSOR_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AD_COMPRESSOR_BUILD_AU "Build the Audio Unit as well as the VST3 (macOS only)" ON)
option(AD_COMPRESSOR_BUILD_STANDALONE "Build a standalone app (handy for quick UI testing)" OFF)
option(AD_COMPRESSOR_BUILD_TESTS "Build the headless test executable" ON)
option(AD_COMPRESSOR_BUILD_SCREENSHOT_TOOL "Build a tool that renders the UI to docs/screenshot.png" OFF)
option(AD_COMPRESSOR_COPY_AFTER_BUILD "Install the plugins into your user plug-in folders after building" OFF)

set(AD_COMPRESSOR_GITHUB_REPO "kyle95wm/ad-compressor")
set(AD_COMPRESSOR_ACCENT      "0xffffb238")   # amber; the plugin's one accent colour (ARGB)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if one is given, otherwise fetch a pinned release.
# Keep the tag identical across all kyle95wm plugins (see CONVENTIONS.md).
# ---------------------------------------------------------------------------
set(AD_COMPRESSOR_JUCE_PATH "" CACHE PATH "Path to an existing JUCE checkout (optional)")

if(AD_COMPRESSOR_JUCE_PATH)
    add_subdirectory(${AD_COMPRESSOR_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.17-security
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(AD_COMPRESSOR_FORMATS VST3)
if(APPLE AND AD_COMPRESSOR_BUILD_AU)
    list(APPEND AD_COMPRESSOR_FORMATS AU)
endif()
if(AD_COMPRESSOR_BUILD_STANDALONE)
    list(APPEND AD_COMPRESSOR_FORMATS Standalone)
endif()

juce_add_plugin(ADCompressor
    PRODUCT_NAME             "ADCompressor"
    COMPANY_NAME             "kyle95wm"          # shared: groups every plugin in one Logic menu
    COMPANY_WEBSITE          "https://github.com/kyle95wm/ad-compressor"
    BUNDLE_ID                "com.kyle95wm.adcompressor"
    PLUGIN_MANUFACTURER_CODE Ky95                # shared by every kyle95wm plugin
    PLUGIN_CODE              Adcp
    FORMATS                  ${AD_COMPRESSOR_FORMATS}
    IS_SYNTH                 FALSE
    NEEDS_CURL               TRUE                # update checker on Linux
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD  ${AD_COMPRESSOR_COPY_AFTER_BUILD}
    VST3_CATEGORIES          Fx Dynamics
    AU_MAIN_TYPE             kAudioUnitType_Effect
    AU_EXPORT_PREFIX         ADCompressorAU
    DESCRIPTION              "Single-knob snappy voice-over compressor with automatic loudness matching")

set(AD_COMPRESSOR_PLUGIN_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UpdateChecker.cpp)

target_sources(ADCompressor PRIVATE ${AD_COMPRESSOR_PLUGIN_SOURCES})
target_include_directories(ADCompressor PRIVATE Source)

# Read by the shared files (UpdateChecker, BrandLookAndFeel), which stay
# byte-identical across plugins.
set(AD_COMPRESSOR_SHARED_DEFINITIONS
    PLUGIN_GITHUB_REPO="${AD_COMPRESSOR_GITHUB_REPO}"
    BRAND_ACCENT_ARGB=${AD_COMPRESSOR_ACCENT})

target_compile_definitions(ADCompressor PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    ${AD_COMPRESSOR_SHARED_DEFINITIONS})

target_link_libraries(ADCompressor
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The test and screenshot tools compile the plugin's processor/editor directly
# (no plugin host needed), so they need the macros the plugin wrapper would set.
set(AD_COMPRESSOR_TOOL_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JucePlugin_Name="ADCompressor"
    JucePlugin_VersionString="${PROJECT_VERSION}"
    JucePlugin_Build_Standalone=0
    ${AD_COMPRESSOR_SHARED_DEFINITIONS})

# ---------------------------------------------------------------------------
# Headless tests: ctest --test-dir build -C Release
#   Also: ADCompressorTest --render <file.wav>   (renders a take at 0/50/100%)
# ---------------------------------------------------------------------------
if(AD_COMPRESSOR_BUILD_TESTS)
    enable_testing()
    juce_add_console_app(ADCompressorTest PRODUCT_NAME "ADCompressorTest")
    target_sources(ADCompressorTest PRIVATE Tests/TestHost.cpp ${AD_COMPRESSOR_PLUGIN_SOURCES})
    target_include_directories(ADCompressorTest PRIVATE Source)
    target_compile_definitions(ADCompressorTest PRIVATE ${AD_COMPRESSOR_TOOL_DEFINITIONS})
    target_link_libraries(ADCompressorTest PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_recommended_config_flags)
    add_test(NAME ADCompressorTests COMMAND ADCompressorTest)
endif()

# ---------------------------------------------------------------------------
# Optional: render the editor to a PNG for the README (see Tools/RenderScreenshot.cpp)
# ---------------------------------------------------------------------------
if(AD_COMPRESSOR_BUILD_SCREENSHOT_TOOL)
    juce_add_console_app(ADCompressorScreenshot PRODUCT_NAME "ADCompressorScreenshot")
    target_sources(ADCompressorScreenshot PRIVATE Tools/RenderScreenshot.cpp ${AD_COMPRESSOR_PLUGIN_SOURCES})
    target_include_directories(ADCompressorScreenshot PRIVATE Source)
    target_compile_definitions(ADCompressorScreenshot PRIVATE ${AD_COMPRESSOR_TOOL_DEFINITIONS}
        JUCE_MODAL_LOOPS_PERMITTED=1)   # lets the tool pump the message loop while the update check finishes
    target_link_libraries(ADCompressorScreenshot PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
