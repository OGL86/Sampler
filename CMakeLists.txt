cmake_minimum_required(VERSION 3.22)
project(Sampler VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(Sampler
    PRODUCT_NAME "Sampler")

target_sources(Sampler PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/SamplerEngine.cpp
    Source/SamplerRender.cpp
    Source/Instrument.cpp
    Source/DiskStreamer.cpp
    Source/Ui.cpp)

target_compile_definitions(Sampler PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Sampler,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${CMAKE_PROJECT_VERSION}")

target_link_libraries(Sampler PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_gui_basics
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Motor-tester uten GUI: cmake -B build -DSAMPLER_BUILD_TESTS=ON && cmake --build build --target SamplerTests
option(SAMPLER_BUILD_TESTS "Bygg motor-tester" OFF)

if(SAMPLER_BUILD_TESTS)
    juce_add_console_app(SamplerTests PRODUCT_NAME "SamplerTests")

    target_sources(SamplerTests PRIVATE
        Tests/EngineTests.cpp
        Source/SamplerEngine.cpp
        Source/SamplerRender.cpp
        Source/Instrument.cpp
        Source/DiskStreamer.cpp)

    target_compile_definitions(SamplerTests PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)

    target_link_libraries(SamplerTests PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_core
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    enable_testing()
    add_test(NAME engine COMMAND SamplerTests)
endif()
