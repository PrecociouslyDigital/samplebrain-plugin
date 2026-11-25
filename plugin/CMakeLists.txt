option(SB_COPY_PLUGIN "Install the plugin into ~/Library/Audio/Plug-Ins after building" OFF)

add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
juce_set_ara_sdk_path(${ARA_SDK_DIR})

juce_add_plugin(Samplebrain
  PRODUCT_NAME "Samplebrain"
  COMPANY_NAME "PrecociouslyDigital"
  BUNDLE_ID com.precociouslydigital.samplebrain
  PLUGIN_MANUFACTURER_CODE Prdg
  PLUGIN_CODE Sbrn
  FORMATS VST3 AU
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  IS_ARA_EFFECT TRUE
  ARA_PLAYBACK_TRANSFORMATIONS "kARAPlaybackTransformationNoChanges"
  COPY_PLUGIN_AFTER_BUILD ${SB_COPY_PLUGIN}
  VST3_CATEGORIES Fx
  AU_MAIN_TYPE kAudioUnitType_Effect
)

target_sources(Samplebrain PRIVATE
  document_controller.cpp
  editor.cpp
  model.cpp
  engine_service.cpp
  playback_renderer.cpp
  processor.cpp
)

target_compile_definitions(Samplebrain PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(Samplebrain
  PRIVATE
    samplebrain_engine
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_basics
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# Clang checks that the nonblocking playback callback only calls
# nonblocking code. RealtimeSanitizer can't do this in a host, because
# hardened hosts like REAPER won't load its runtime.
target_compile_options(Samplebrain PRIVATE -Werror=function-effects)
