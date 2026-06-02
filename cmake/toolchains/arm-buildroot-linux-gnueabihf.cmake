set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(MCP_ARM_BUILDROOT_SDK
    "/home/alinx/prj/board/100ask_stm32mp157_pro-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot"
    CACHE PATH "Path to the arm-buildroot-linux-gnueabihf Buildroot SDK")

if(DEFINED ENV{MCP_ARM_BUILDROOT_SDK})
    set(MCP_ARM_BUILDROOT_SDK "$ENV{MCP_ARM_BUILDROOT_SDK}" CACHE PATH
        "Path to the arm-buildroot-linux-gnueabihf Buildroot SDK" FORCE)
endif()

set(_mcp_cross_compile "arm-buildroot-linux-gnueabihf")
if(DEFINED ENV{CROSS_COMPILE})
    string(REGEX REPLACE "-$" "" _mcp_cross_compile "$ENV{CROSS_COMPILE}")
endif()

set(_mcp_toolchain_bin "${MCP_ARM_BUILDROOT_SDK}/bin")
set(_mcp_c_compiler_names
    "${_mcp_cross_compile}-gcc.br_real"
    "${_mcp_cross_compile}-gcc"
)
set(_mcp_cxx_compiler_names
    "${_mcp_cross_compile}-g++.br_real"
    "${_mcp_cross_compile}-g++"
)

find_program(CMAKE_C_COMPILER
    NAMES ${_mcp_c_compiler_names}
    HINTS "${_mcp_toolchain_bin}"
    NO_DEFAULT_PATH
)

find_program(CMAKE_CXX_COMPILER
    NAMES ${_mcp_cxx_compiler_names}
    HINTS "${_mcp_toolchain_bin}"
    NO_DEFAULT_PATH
)

find_program(CMAKE_AR
    NAMES "${_mcp_cross_compile}-ar"
    HINTS "${_mcp_toolchain_bin}"
    NO_DEFAULT_PATH
)

find_program(CMAKE_RANLIB
    NAMES "${_mcp_cross_compile}-ranlib"
    HINTS "${_mcp_toolchain_bin}"
    NO_DEFAULT_PATH
)

find_program(CMAKE_STRIP
    NAMES "${_mcp_cross_compile}-strip"
    HINTS "${_mcp_toolchain_bin}"
    NO_DEFAULT_PATH
)

if(NOT CMAKE_C_COMPILER)
    message(FATAL_ERROR
        "Unable to find ${_mcp_cross_compile}-gcc in ${_mcp_toolchain_bin}. "
        "Set MCP_ARM_BUILDROOT_SDK or CROSS_COMPILE before configuring."
    )
endif()

execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-sysroot
    OUTPUT_VARIABLE _mcp_sysroot
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(_mcp_sysroot)
    set(CMAKE_SYSROOT "${_mcp_sysroot}" CACHE PATH "Target sysroot")
endif()

set(CMAKE_FIND_ROOT_PATH
    "${MCP_ARM_BUILDROOT_SDK}"
    "${CMAKE_SYSROOT}"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(MCP_TARGET_TRIPLET "${_mcp_cross_compile}" CACHE STRING "Target compiler triplet")
