add_executable(mcp_server)

target_sources(mcp_server
    PRIVATE
        core/in_flight.c
        core/server.c
        gateway/gateway.c
        main/main.c
        protocol/jsonrpc.c
        registry/tool_registry.c
        tools/builtin_tools.c
        transport/stdio_transport.c
)

target_include_directories(mcp_server
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(mcp_server
    PRIVATE
        MCP::jansson
        MCP::libuv
)

target_compile_definitions(mcp_server
    PRIVATE
        MCP_SERVER_VERSION="${PROJECT_VERSION}"
        MCP_HAS_TOOL_SYSTEM=$<IF:$<STREQUAL:${MCP_TOOL_SYSTEM},n>,0,1>
        MCP_HAS_TOOL_GATEWAY=$<IF:$<STREQUAL:${MCP_TOOL_GATEWAY},n>,0,1>
        MCP_HAS_TOOL_REGISTRY=$<IF:$<STREQUAL:${MCP_TOOL_REGISTRY},n>,0,1>
        MCP_HAS_TOOL_PLUGIN_MANAGER=$<IF:$<STREQUAL:${MCP_TOOL_PLUGIN_MANAGER},n>,0,1>
        MCP_HAS_TOOL_EMBEDDED=$<IF:$<STREQUAL:${MCP_TOOL_EMBEDDED},n>,0,1>
)

if(MSVC)
    target_compile_definitions(mcp_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_server PRIVATE /W4)
else()
    target_compile_options(mcp_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
