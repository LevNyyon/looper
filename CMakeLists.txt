cmake_minimum_required(VERSION 3.22)
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")
project(Looper VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# git clone --depth 1 --branch 8.0.15 https://github.com/juce-framework/JUCE.git third_party/JUCE
add_subdirectory(third_party/JUCE EXCLUDE_FROM_ALL)

add_library(engine STATIC engine/Engine.cpp)
target_include_directories(engine PUBLIC engine)

add_executable(selfcheck engine/selfcheck.cpp)
target_link_libraries(selfcheck PRIVATE engine)

juce_add_gui_app(Looper
    PRODUCT_NAME "Looper"
    BUNDLE_ID com.nyyon.looper
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Looper records your instrument so it can loop it.")

file(GLOB APP_SOURCES CONFIGURE_DEPENDS app/*.cpp app/*.mm)
target_sources(Looper PRIVATE ${APP_SOURCES})

target_compile_definitions(Looper PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Looper PRIVATE
    engine
    "-framework AVFoundation"
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
