cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(dlssnr_composite LANGUAGES C CXX)

# Static CRT: the DLL has no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# DLSS Neural Rendering at half resolution, composited onto the native
# frame. One DLL: loads as a ReShade addon (.addon64, with an overlay toggle),
# as an ASI plugin (.asi) or as a plain DLL through any injector.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Shaders are compiled at build time with the Windows SDK's fxc and embedded.
find_program(FXC fxc.exe
    HINTS "$ENV{WindowsSdkVerBinPath}x64" "$ENV{WindowsSdkDir}bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    PATHS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64")
if(NOT FXC)
    message(FATAL_ERROR "fxc.exe not found (Windows SDK)")
endif()
set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/dlssnr_composite.hlsl")
set(SHADER_GEN "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_GEN}")
foreach(entry Reduce Compose)
    add_custom_command(OUTPUT "${SHADER_GEN}/${entry}.h"
        COMMAND "${FXC}" /nologo /T cs_5_1 /E ${entry} /O3 /Vn k${entry}CS /Fh "${SHADER_GEN}/${entry}.h" "${SHADER_SRC}"
        DEPENDS "${SHADER_SRC}"
        COMMENT "fxc ${entry}")
    list(APPEND SHADER_HEADERS "${SHADER_GEN}/${entry}.h")
endforeach()

add_library(dlssnr_composite SHARED
    src/dlssnr_composite.cpp
    ${SHADER_HEADERS}
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)

set_target_properties(dlssnr_composite PROPERTIES
    OUTPUT_NAME "dlssnr_composite"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)

target_include_directories(dlssnr_composite PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SHADER_GEN}"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include")

target_compile_definitions(dlssnr_composite PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(dlssnr_composite PRIVATE d3d12 dxgi)

# The same binary under the three names loaders look for.
add_custom_command(TARGET dlssnr_composite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dlssnr_composite>" "$<TARGET_FILE_DIR:dlssnr_composite>/dlssnr_composite.addon64"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dlssnr_composite>" "$<TARGET_FILE_DIR:dlssnr_composite>/dlssnr_composite.asi"
    COMMENT "dlssnr_composite.dll / .addon64 / .asi")
