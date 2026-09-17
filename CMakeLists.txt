####################################################################################################
# occlude3d - Ashita v4 plugin (32-bit).
#
# Configure (Windows, x86 toolset; ASHITA4_SDK_PATH must point at a folder containing Ashita.h):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# Output: build/Release/occlude3d.dll  ->  copy to Ashita-v4beta-main/plugins/
####################################################################################################
cmake_minimum_required(VERSION 3.22)

project(occlude3d LANGUAGES CXX)

# Locate the Ashita SDK via the ASHITA4_SDK_PATH environment variable.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(AshitaSDK REQUIRED)

# Apply the SDK's recommended global CMake configuration.
ashita_sdk_initialize()

# Ashita plugins are MODULE DLLs (loaded via LoadLibrary, never linked against).
add_library(occlude3d MODULE
    src/occlude3d.cpp
    src/occlude3d.hpp
    src/exports.def
)

# Emit exactly "occlude3d.dll" (no "lib" prefix, explicit .dll suffix).
set_target_properties(occlude3d PROPERTIES
    PREFIX ""
    OUTPUT_NAME "occlude3d"
    SUFFIX ".dll"
)

# Attach SDK includes/libs and the recommended compiler/linker defaults.
ashita_sdk_attach(occlude3d)
ashita_sdk_set_compiler_defaults(occlude3d)
ashita_sdk_set_linker_defaults(occlude3d)
ashita_sdk_set_compiler_warn_level(occlude3d 3)

# IMPORTANT: do NOT enable Safe-SEH (ashita_sdk_set_linker_enable_safe_seh). The legacy Direct3D8
# import libs are not /SAFESEH-compatible and linking fails if it is enabled.
