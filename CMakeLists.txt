cmake_minimum_required(VERSION 3.22)

project(ReverbVSTCreation VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/MP)

    # Release builds link the C/C++ runtime statically so the shipped VST3
    # loads on a machine that has never had the Visual C++ redistributable
    # installed. Override on the command line if a host needs the shared CRT.
    if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

option(RIVAGE_GENERATE_MAP "Emit a linker map file for the VST3 target" OFF)
option(RIVAGE_FETCH_JUCE "Download JUCE with FetchContent instead of using JUCE-9.0.2/" OFF)

set(RIVAGE_JUCE_TAG "9.0.2")

if (RIVAGE_FETCH_JUCE)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${RIVAGE_JUCE_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE-${RIVAGE_JUCE_TAG}/CMakeLists.txt")
    add_subdirectory("JUCE-${RIVAGE_JUCE_TAG}")
else()
    message(FATAL_ERROR
        "JUCE ${RIVAGE_JUCE_TAG} was not found.\n"
        "Either clone it next to this file:\n"
        "  git clone --depth 1 --branch ${RIVAGE_JUCE_TAG} https://github.com/juce-framework/JUCE.git JUCE-${RIVAGE_JUCE_TAG}\n"
        "or configure with -DRIVAGE_FETCH_JUCE=ON so CMake fetches it.")
endif()

add_subdirectory(Source)
add_subdirectory(tools/analyzer)
add_subdirectory(tools/preview)
