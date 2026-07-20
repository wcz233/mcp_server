add_executable(mcp_server)

if(NOT (MCP_FILE_TRANSFER_PLUGIN STREQUAL "y" OR
        MCP_FILE_TRANSFER_PLUGIN STREQUAL "m" OR
        MCP_FILE_TRANSFER_PLUGIN STREQUAL "n"))
    message(FATAL_ERROR "Invalid MCP_FILE_TRANSFER_PLUGIN: ${MCP_FILE_TRANSFER_PLUGIN}")
endif()
message(STATUS "MCP file transfer plugin mode: ${MCP_FILE_TRANSFER_PLUGIN}")

target_sources(mcp_server
    PRIVATE
        core/in_flight.c
        core/server.c
        discovery/server_discovery.c
        gateway/gateway.c
        listener/framed_listener.c
        main/main.c
        plugin/plugin_manager.c
        protocol/jsonrpc.c
        registry/tool_registry.c
        tools/builtin_tools.c
        tools/schema.c
        tools/shell_exec.c
        tools/shell_policy.c
        tools/system_status.c
        tools/tool_result.c
        transport/peer_transport.c
        transport/stdio_transport.c
        $<$<BOOL:${MCP_TRANSPORT_UDP}>:transport/udp_transport.c>
        $<$<STREQUAL:${MCP_FILE_TRANSFER_PLUGIN},y>:plugins/file_transfer/file_transfer_plugin.c>
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
        MCP_HAS_TRANSPORT_UDP=$<IF:$<BOOL:${MCP_TRANSPORT_UDP}>,1,0>
        MCP_HAS_FILE_TRANSFER_PLUGIN_BUILTIN=$<IF:$<STREQUAL:${MCP_FILE_TRANSFER_PLUGIN},y>,1,0>
        $<$<STREQUAL:${MCP_FILE_TRANSFER_PLUGIN},y>:MCP_FILE_TRANSFER_PLUGIN_BUILTIN=1>
        MCP_SHELL_EXEC_DEFAULT_CONFIG="${PROJECT_SOURCE_DIR}/config/tools/shell_exec.json"
        MCP_SHELL_EXEC_INSTALLED_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/mcp_server/config/tools/shell_exec.json"
        $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

if(MSVC)
    target_compile_definitions(mcp_server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_server PRIVATE /W4)
else()
    target_compile_options(mcp_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MCP_FILE_TRANSFER_PLUGIN STREQUAL "m")
    find_package(Threads REQUIRED)

    add_library(mcp_file_transfer_plugin MODULE
        plugins/file_transfer/file_transfer_plugin.c
    )

    target_include_directories(mcp_file_transfer_plugin
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
    )

    target_compile_definitions(mcp_file_transfer_plugin
        PRIVATE
            $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
            $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
    )

    target_link_libraries(mcp_file_transfer_plugin
        PRIVATE
            MCP::jansson
            MCP::libuv
            Threads::Threads
    )

    set_target_properties(mcp_file_transfer_plugin PROPERTIES
        PREFIX ""
        OUTPUT_NAME "mcp_file_transfer_plugin"
    )
else()
    set(_mcp_file_transfer_plugin_outputs
        "mcp_file_transfer_plugin${CMAKE_SHARED_MODULE_SUFFIX}"
        "mcp_file_transfer_plugin.so"
        "mcp_file_transfer_plugin.dll"
        "mcp_file_transfer_plugin.dylib"
    )
    foreach(_mcp_config_dir "" Debug Release RelWithDebInfo MinSizeRel)
        if(_mcp_config_dir STREQUAL "")
            set(_mcp_module_dir "${CMAKE_CURRENT_BINARY_DIR}")
        else()
            set(_mcp_module_dir "${CMAKE_CURRENT_BINARY_DIR}/${_mcp_config_dir}")
        endif()
        foreach(_mcp_output IN LISTS _mcp_file_transfer_plugin_outputs)
            file(REMOVE "${_mcp_module_dir}/${_mcp_output}")
        endforeach()
    endforeach()
    unset(_mcp_file_transfer_plugin_outputs)
    unset(_mcp_config_dir)
    unset(_mcp_module_dir)
    unset(_mcp_output)
endif()
