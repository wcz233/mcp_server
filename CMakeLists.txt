cmake_minimum_required(VERSION 3.18)

project(mcp_server
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(MCPPlatform)
include(MCPOptions)
include(MCPFeature)
include(MCPThirdParty)
include(MCPInstall)

mcp_detect_platform()
mcp_load_default_options()
mcp_load_kconfig_options()
mcp_setup_third_party()

add_subdirectory(external)
add_subdirectory(src)

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS mcp_server
    RUNTIME DESTINATION bin
)

install(FILES "${PROJECT_SOURCE_DIR}/config/tools/shell_exec.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mcp_server/config/tools"
)

install(FILES "${PROJECT_SOURCE_DIR}/config/network/network_access.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mcp_server/config/network"
)
