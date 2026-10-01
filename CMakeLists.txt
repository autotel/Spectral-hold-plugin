cmake_minimum_required(VERSION 3.22)
project(SpectralHold VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE location. Defaults to ../JUCE (local dev); override with -DJUCE_PATH=... (CI).
set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory(${JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)

juce_add_plugin(SpectralHold
    COMPANY_NAME            "autotel"
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        TRUE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Jqna
    PLUGIN_CODE             Sphd
    FORMATS                 VST3 Standalone
    PRODUCT_NAME            "Spectral Hold")

target_sources(SpectralHold PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/SpectralEngine.cpp
    Source/PlateReverb.cpp
    Source/SpectrumDisplay.cpp
    Source/SpectralLookAndFeel.cpp
    Source/VerticalToggle.cpp)

target_compile_definitions(SpectralHold PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SpectralHold PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- CLAP (agent-wiki/plan-roadmap.md B10) ----
# Fetches clap-juce-extensions over the network; guarded so a network-less local build can
# turn it off (-DSPECTRALHOLD_CLAP=OFF).
#
# Pinned to a `main` commit, NOT the latest tag (0.26.0): the tag still includes
# JUCE_LegacyAudioParameter.cpp via the old `juce_audio_processors/format_types/...` path,
# which doesn't exist in JUCE 8's split (juce_audio_processors_headless/format_types/...) --
# fails with "No such file or directory" at CLAP-target compile time. `main` (as of the pinned
# commit) already updated that include path for the new layout; verified building here.
option(SPECTRALHOLD_CLAP "Build the CLAP plugin target (fetches clap-juce-extensions)" ON)
if(SPECTRALHOLD_CLAP)
    include(FetchContent)
    FetchContent_Declare(clap-juce-extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG        16e9d4ca7b1e86c76e04584b2c08e85a764bcda8) # main, 2026-06-24
    FetchContent_MakeAvailable(clap-juce-extensions)

    clap_juce_extensions_plugin(TARGET SpectralHold
        CLAP_ID       "com.autotel.spectralhold"
        CLAP_FEATURES audio-effect)
endif()

# ---- offline DSP smoke-test (no host, no GUI) ----
juce_add_console_app(SpectralHoldTest PRODUCT_NAME "SpectralHoldTest")
target_sources(SpectralHoldTest PRIVATE Source/test_main.cpp Source/SpectralEngine.cpp Source/PlateReverb.cpp)
target_compile_definitions(SpectralHoldTest PRIVATE
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(SpectralHoldTest PRIVATE
    juce::juce_dsp
    juce::juce_recommended_config_flags)
