juce_add_console_app(Analyzer PRODUCT_NAME "RvxAnalyzer")

target_sources(Analyzer PRIVATE
    Main.cpp
    ../../Source/dsp/ReverbEngine.cpp
    ../../Source/dsp/HallAlgorithm.cpp
    ../../Source/dsp/RoomAlgorithm.cpp
    ../../Source/dsp/PlateAlgorithm.cpp
    ../../Source/dsp/SpringAlgorithm.cpp
    ../../Source/dsp/SpringTankAlgorithm.cpp
    ../../Source/dsp/ShimmerAlgorithm.cpp
    ../../Source/dsp/CloudAlgorithm.cpp)

target_include_directories(Analyzer PRIVATE ../../Source)

target_compile_definitions(Analyzer PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Analyzer,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Analyzer,JUCE_VERSION>")

target_link_libraries(Analyzer PRIVATE
    juce::juce_audio_processors_headless
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
