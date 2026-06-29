cmake_minimum_required(VERSION 3.22)
project(SpectralHold VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE location. Defaults to ../JUCE (local dev); override with -DJUCE_PATH=... (CI).
set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory(${JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)

juce_add_plugin(SpectralHold
    COMPANY_NAME            "autotel"
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
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
    Source/SpectrumDisplay.cpp)

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

# ---- offline DSP smoke-test (no host, no GUI) ----
juce_add_console_app(SpectralHoldTest PRODUCT_NAME "SpectralHoldTest")
target_sources(SpectralHoldTest PRIVATE Source/test_main.cpp Source/SpectralEngine.cpp)
target_compile_definitions(SpectralHoldTest PRIVATE
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(SpectralHoldTest PRIVATE
    juce::juce_dsp
    juce::juce_recommended_config_flags)
