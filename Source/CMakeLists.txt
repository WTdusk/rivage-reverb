# Rivage Reverb - WTdusk
#
# Identity fields:
#   PLUGIN_MANUFACTURER_CODE / PLUGIN_CODE form the VST3 CID. Do not change them
#   after a public release: DAWs identify the plug-in by this pair, so a change
#   makes existing sessions treat Rivage as a different plug-in.
#   BUNDLE_ID uses the GitHub-account form of reverse-DNS (no owned domain yet).
juce_add_plugin(RivageReverb
    COMPANY_NAME "WTdusk"
    COMPANY_COPYRIGHT "© 2026 WTdusk"
    COMPANY_WEBSITE ""
    BUNDLE_ID "io.github.wtdusk.rivage"
    PLUGIN_MANUFACTURER_CODE WTdk
    PLUGIN_CODE Rivg
    VERSION 0.1.0
    FORMATS VST3
    PRODUCT_NAME "Rivage Reverb"
    DESCRIPTION "Six-machine guitar reverb"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx Reverb")

target_sources(RivageReverb PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
    Parameters.cpp
    dsp/ReverbEngine.cpp
    dsp/HallAlgorithm.cpp
    dsp/RoomAlgorithm.cpp
    dsp/PlateAlgorithm.cpp
    dsp/SpringAlgorithm.cpp
    dsp/ShimmerAlgorithm.cpp
    dsp/CloudAlgorithm.cpp
    ui/RivageLookAndFeel.cpp)

# The artwork that fills the editor is embedded in the binary as binary data.
juce_add_binary_data(RivageArt SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../assets/rivage-waterlilies.jpg"
    NAMESPACE RivageArt
    HEADER_NAME RivageArt.h)

target_include_directories(RivageReverb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(RivageReverb PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:RivageReverb,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:RivageReverb,JUCE_VERSION>")

if (MSVC AND RIVAGE_GENERATE_MAP)
    target_link_options(RivageReverb_VST3 PRIVATE "/DEBUG" "/MAP")
    target_link_options(RivageReverb_VST3 PRIVATE "/OPT:REF" "/OPT:ICF")
endif()


target_link_libraries(RivageReverb PRIVATE
    RivageArt
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
