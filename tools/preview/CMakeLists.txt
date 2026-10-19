juce_add_gui_app(RivagePreview
    PRODUCT_NAME "Rivage Preview"
    COMPANY_NAME "WTdusk"
    BUNDLE_ID "io.github.wtdusk.rivage.preview")

target_sources(RivagePreview PRIVATE Main.cpp)

target_include_directories(RivagePreview PRIVATE ${CMAKE_SOURCE_DIR}/Source)

target_compile_definitions(RivagePreview PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:RivagePreview,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:RivagePreview,JUCE_VERSION>")

target_link_libraries(RivagePreview PRIVATE
    RivageReverb
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
