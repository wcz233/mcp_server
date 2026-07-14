find_package(Python3 COMPONENTS Interpreter)

add_executable(mcp_file_transfer_offset_test
    file_transfer_offset_test.c
)

target_include_directories(mcp_file_transfer_offset_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/plugins/file_transfer"
)

target_compile_definitions(mcp_file_transfer_offset_test
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

if(MSVC)
    target_compile_definitions(mcp_file_transfer_offset_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_file_transfer_offset_test PRIVATE /W4)
else()
    target_compile_options(mcp_file_transfer_offset_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_file_transfer_offset
    COMMAND mcp_file_transfer_offset_test
)

find_package(Threads REQUIRED)

add_executable(mcp_file_transfer_write_queue_test
    file_transfer_write_queue_test.c
)

target_include_directories(mcp_file_transfer_write_queue_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(mcp_file_transfer_write_queue_test
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

target_link_libraries(mcp_file_transfer_write_queue_test
    PRIVATE
        MCP::jansson
        MCP::libuv
        Threads::Threads
)

if(MSVC)
    target_compile_definitions(mcp_file_transfer_write_queue_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_file_transfer_write_queue_test PRIVATE /W4)
else()
    target_compile_options(mcp_file_transfer_write_queue_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_file_transfer_write_queue
    COMMAND mcp_file_transfer_write_queue_test
)

if(NOT "${MCP_TOOL_PLUGIN_MANAGER}" STREQUAL "n")
    add_library(mcp_busy_shutdown_plugin MODULE
        busy_shutdown_plugin.c
    )

    target_include_directories(mcp_busy_shutdown_plugin
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
    )

    if(MSVC)
        target_compile_definitions(mcp_busy_shutdown_plugin PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(mcp_busy_shutdown_plugin PRIVATE /W4)
    else()
        target_compile_options(mcp_busy_shutdown_plugin PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    set_target_properties(mcp_busy_shutdown_plugin PROPERTIES
        PREFIX ""
        OUTPUT_NAME "mcp_busy_shutdown_plugin"
    )
endif()

if(Python3_Interpreter_FOUND)
    set(_mcp_file_transfer_path_plugin "-")
    if(TARGET mcp_file_transfer_plugin)
        set(_mcp_file_transfer_path_plugin "$<TARGET_FILE:mcp_file_transfer_plugin>")
    endif()

    if(NOT "${MCP_FILE_TRANSFER_PLUGIN}" STREQUAL "n")
        add_test(
            NAME mcp_file_transfer_path_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/file_transfer_path_smoke.py" "$<TARGET_FILE:mcp_server>" "${_mcp_file_transfer_path_plugin}" 18784 18785 18786 18787
        )
    endif()

    add_test(
        NAME mcp_stdio_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stdio_smoke.py" "$<TARGET_FILE:mcp_server>"
    )

    add_test(
        NAME mcp_tools_dynamic_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools_dynamic_smoke.py" "$<TARGET_FILE:mcp_server>"
    )

    if(TARGET mcp_busy_shutdown_plugin)
        add_test(
            NAME mcp_plugin_busy_unload_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/plugin_busy_unload_smoke.py" "$<TARGET_FILE:mcp_server>" "$<TARGET_FILE:mcp_busy_shutdown_plugin>"
        )
    endif()

    add_test(
        NAME mcp_shell_exec_policy_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/shell_exec_policy_smoke.py" "$<TARGET_FILE:mcp_server>"
    )

    add_test(
        NAME mcp_shell_job_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/shell_job_smoke.py" "$<TARGET_FILE:mcp_server>"
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

    if(TARGET mcp_file_transfer_plugin)
        add_test(
            NAME mcp_file_transfer_d_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/file_transfer_d_smoke.py" "$<TARGET_FILE:mcp_server>" "$<TARGET_FILE:mcp_file_transfer_plugin>" 18780 18781 18782 18783
        )
    endif()

    if(MCP_TRANSPORT_UDP)
        add_test(
            NAME mcp_udp_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/udp_smoke.py" "$<TARGET_FILE:mcp_server>" 18765
        )
    endif()
endif()
