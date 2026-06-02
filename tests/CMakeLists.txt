find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
    add_test(
        NAME mcp_stdio_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stdio_smoke.py" "$<TARGET_FILE:mcp_server>"
    )

    add_test(
        NAME mcp_tools_dynamic_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools_dynamic_smoke.py" "$<TARGET_FILE:mcp_server>"
    )

    add_test(
        NAME mcp_shell_exec_policy_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/shell_exec_policy_smoke.py" "$<TARGET_FILE:mcp_server>"
    )

    add_test(
        NAME mcp_startup_failure_cleanup_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/startup_failure_cleanup_smoke.py" "$<TARGET_FILE:mcp_server>"
    )

    add_test(
        NAME mcp_no_stdio_listener_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/no_stdio_listener_smoke.py" "$<TARGET_FILE:mcp_server>" 18766
    )

    add_test(
        NAME mcp_tcp_framed_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tcp_framed_smoke.py" "$<TARGET_FILE:mcp_server>" 18767
    )

    add_test(
        NAME mcp_server_discovery_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server_discovery_smoke.py" "$<TARGET_FILE:mcp_server>" 18768 18769 18770 18771
    )

    add_test(
        NAME mcp_server_discovery_liveness_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server_discovery_liveness_smoke.py" "$<TARGET_FILE:mcp_server>" 18772 18773 18774 18775 18776 18777 18778 18779
    )

    if(MCP_TRANSPORT_UDP)
        add_test(
            NAME mcp_udp_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/udp_smoke.py" "$<TARGET_FILE:mcp_server>" 18765
        )
    endif()
endif()
