cmake_minimum_required(VERSION 3.20)
project(vig8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require Clang (generated code uses __builtin_rotateleft64, __builtin_bswap*, etc.)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Clang is required to build this project. Set CMAKE_CXX_COMPILER to clang-cl or clang++.")
endif()

# SIMDE (SIMD Everywhere) headers from XenonRecomp thirdparty
# Generated code includes <x86/avx.h> which resolves to simde/x86/avx.h
set(SIMDE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde")
if(NOT EXISTS "${SIMDE_INCLUDE_DIR}/x86/avx.h")
    message(FATAL_ERROR "SIMDE headers not found at ${SIMDE_INCLUDE_DIR}. Make sure XenonRecomp is cloned in tools/.")
endif()

# Generated PPC recomp sources
file(GLOB PPC_RECOMP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ppc/ppc_recomp.*.cpp")
set(PPC_FUNC_MAPPING "${CMAKE_CURRENT_SOURCE_DIR}/ppc/ppc_func_mapping.cpp")

if(NOT PPC_RECOMP_SOURCES)
    message(FATAL_ERROR "No generated ppc_recomp.*.cpp files found in ppc/. Run XenonRecomp first.")
endif()

list(LENGTH PPC_RECOMP_SOURCES PPC_FILE_COUNT)
message(STATUS "Found ${PPC_FILE_COUNT} PPC recomp source files")

# Runtime source files
set(RUNTIME_SOURCES
    src/main.cpp
    src/memory.cpp
    src/xex_loader.cpp
    src/kernel_stubs.cpp
    src/math_polyfill.cpp
)

# Build the recompiled PPC code as a static library
# This keeps compile times manageable (each .cpp compiles independently)
add_library(ppc_recomp STATIC
    ${PPC_RECOMP_SOURCES}
    ${PPC_FUNC_MAPPING}
)

target_include_directories(ppc_recomp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ppc"
    "${SIMDE_INCLUDE_DIR}"
)

# Clang flags for the generated PPC code
target_compile_options(ppc_recomp PRIVATE
    -Wno-everything          # Generated code has many warnings, suppress all
    -O2                      # Optimize for performance
    -fno-strict-aliasing     # Required for type-punning in PPC register unions
)

if(WIN32)
    target_compile_definitions(ppc_recomp PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
endif()

# Main runtime executable
add_executable(vig8 ${RUNTIME_SOURCES})

target_include_directories(vig8 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ppc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SIMDE_INCLUDE_DIR}"
)

target_link_libraries(vig8 PRIVATE ppc_recomp)

target_compile_options(vig8 PRIVATE
    -Wall
    -O2
    -fno-strict-aliasing
)

if(WIN32)
    target_compile_definitions(vig8 PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# Print build summary
message(STATUS "Vigilante 8 Arcade Recomp")
message(STATUS "  PPC source files: ${PPC_FILE_COUNT}")
message(STATUS "  Entry point: 0x823253B0 (_xstart)")
message(STATUS "  Image base: 0x82000000")
message(STATUS "  Image size: 0x4E0000")
