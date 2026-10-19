cmake_minimum_required(VERSION 3.22)

project(ReverbVSTCreation VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/MP)
endif()

option(RIVAGE_GENERATE_MAP "Emit a linker map file for the VST3 target" OFF)

add_subdirectory(JUCE-9.0.2)

add_subdirectory(Source)
add_subdirectory(tools/analyzer)
add_subdirectory(tools/preview)
