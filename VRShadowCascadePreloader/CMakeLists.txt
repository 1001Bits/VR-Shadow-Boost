cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)

project(
    VRShadowCascadePreloader
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output as version.dll
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(OUTPUT_NAME "version")

add_library(${PROJECT_NAME} SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/cascade_patch.cpp
    src/cascade_patch.h
    src/version.def
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# Do NOT link against version.lib - we're replacing it
# target_link_libraries(${PROJECT_NAME} PRIVATE version)

# Set output name to version.dll, use static CRT (/MT)
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME ${OUTPUT_NAME}
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
