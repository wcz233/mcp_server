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

    if(MCP_TRANSPORT_UDP)
        add_test(
            NAME mcp_udp_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/udp_smoke.py" "$<TARGET_FILE:mcp_server>" 18765
        )
    endif()
endif()
