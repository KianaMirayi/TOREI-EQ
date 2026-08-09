cmake_minimum_required(VERSION 3.22)
project(TOREI-EQ VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(JUCE_ROOT "E:/Programing/C++Hell/JUCE" CACHE PATH "JUCE root directory")
add_subdirectory(${JUCE_ROOT} juce_build)

juce_add_plugin(TOREI-EQ
    COMPANY_NAME              "TOREI"
    PLUGIN_NAME               "TOREI-EQ"
    PRODUCT_NAME              "TOREI-EQ"
    VERSION                   "1.0.0"
    FORMATS                   VST3 Standalone
    PLUGIN_MANUFACTURER_CODE  Tore
    PLUGIN_CODE               Eq00
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE
)

juce_generate_juce_header(TOREI-EQ)

target_sources(TOREI-EQ PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
)

target_include_directories(TOREI-EQ PRIVATE src)

# MSVC: 源文件为 UTF-8（中文注释），强制按 UTF-8 解析，避免 GBK 误读吞行
if(MSVC)
    target_compile_options(TOREI-EQ PRIVATE /utf-8)
endif()

# === WebView2 ===
set(WEBVIEW2_SDK "C:/Users/Admin/.nuget/packages/Microsoft.Web.WebView2/1.0.1901.177")
target_include_directories(TOREI-EQ PRIVATE "${WEBVIEW2_SDK}/build/native/include")
target_compile_definitions(TOREI-EQ PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2=1
)

# === WebView2Loader.dll ===
set(WEBVIEW2_DLL "${WEBVIEW2_SDK}/build/native/x64/WebView2Loader.dll")
add_custom_command(TARGET TOREI-EQ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WEBVIEW2_DLL}"
        "$<TARGET_FILE_DIR:TOREI-EQ>/Standalone/WebView2Loader.dll"
)
add_custom_command(TARGET TOREI-EQ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WEBVIEW2_DLL}"
        "$<TARGET_FILE_DIR:TOREI-EQ>/VST3/TOREI-EQ.vst3/Contents/x86_64-win/WebView2Loader.dll"
)

target_link_libraries(TOREI-EQ
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
