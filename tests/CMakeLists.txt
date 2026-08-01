find_package(Python3 COMPONENTS Interpreter)

set(MCP_TEST_TLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/tls-fixtures")
file(MAKE_DIRECTORY "${MCP_TEST_TLS_DIR}")
if(Python3_Interpreter_FOUND AND MCP_TCP_SECURITY STREQUAL "mtls")
    find_program(MCP_TEST_OPENSSL_EXECUTABLE NAMES openssl REQUIRED)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                "-DOPENSSL_EXECUTABLE=${MCP_TEST_OPENSSL_EXECUTABLE}"
                "-DOUTPUT_DIR=${MCP_TEST_TLS_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/generate_tls_fixtures.cmake"
        RESULT_VARIABLE _mcp_tls_fixture_result
    )
    if(NOT _mcp_tls_fixture_result EQUAL 0)
        message(FATAL_ERROR "Failed to generate TLS test fixtures")
    endif()
endif()

add_executable(mcp_network_access_policy_test
    network_access_policy_test.c
    "${PROJECT_SOURCE_DIR}/src/network/network_access_policy.c"
)

target_include_directories(mcp_network_access_policy_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(mcp_network_access_policy_test
    PRIVATE
        MCP::jansson
        MCP::libuv
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(mcp_network_access_policy_test
        PRIVATE
            _GNU_SOURCE
            _POSIX_C_SOURCE=200809L
    )
endif()

if(MSVC)
    target_compile_definitions(mcp_network_access_policy_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_network_access_policy_test PRIVATE /W4)
else()
    target_compile_options(mcp_network_access_policy_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_network_access_policy
    COMMAND mcp_network_access_policy_test
)

add_executable(mcp_shell_policy_test
    shell_policy_test.c
    "${PROJECT_SOURCE_DIR}/src/tools/shell_policy.c"
)

target_include_directories(mcp_shell_policy_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(mcp_shell_policy_test
    PRIVATE
        MCP::jansson
)

if(MSVC)
    target_compile_definitions(mcp_shell_policy_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_shell_policy_test PRIVATE /W4)
else()
    target_compile_options(mcp_shell_policy_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_shell_policy
    COMMAND mcp_shell_policy_test
)

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

add_executable(mcp_file_transfer_deadline_test
    file_transfer_deadline_test.c
)

target_include_directories(mcp_file_transfer_deadline_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(mcp_file_transfer_deadline_test
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

target_link_libraries(mcp_file_transfer_deadline_test
    PRIVATE
        MCP::jansson
        MCP::libuv
        Threads::Threads
)

if(MSVC)
    target_compile_definitions(mcp_file_transfer_deadline_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_file_transfer_deadline_test PRIVATE /W4)
else()
    target_compile_options(mcp_file_transfer_deadline_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_file_transfer_deadline
    COMMAND mcp_file_transfer_deadline_test
)

add_executable(mcp_file_transfer_negotiation_test
    file_transfer_negotiation_test.c
    "${PROJECT_SOURCE_DIR}/src/transport/peer_transport.c"
)

target_include_directories(mcp_file_transfer_negotiation_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)

target_compile_definitions(mcp_file_transfer_negotiation_test
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

target_link_libraries(mcp_file_transfer_negotiation_test
    PRIVATE
        MCP::jansson
        MCP::libuv
        Threads::Threads
)

if(MSVC)
    target_compile_definitions(mcp_file_transfer_negotiation_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_file_transfer_negotiation_test PRIVATE /W4)
else()
    target_compile_options(mcp_file_transfer_negotiation_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_file_transfer_negotiation
    COMMAND mcp_file_transfer_negotiation_test
)

add_executable(mcp_file_transfer_resume_test
    file_transfer_resume_test.c
)

target_include_directories(mcp_file_transfer_resume_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(mcp_file_transfer_resume_test
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

target_link_libraries(mcp_file_transfer_resume_test
    PRIVATE
        MCP::jansson
        MCP::libuv
        Threads::Threads
)

if(MSVC)
    target_compile_definitions(mcp_file_transfer_resume_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_file_transfer_resume_test PRIVATE /W4)
else()
    target_compile_options(mcp_file_transfer_resume_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_file_transfer_resume
    COMMAND mcp_file_transfer_resume_test
)

add_executable(mcp_file_transfer_protocol_bounds_test
    file_transfer_protocol_bounds_test.c
)

target_include_directories(mcp_file_transfer_protocol_bounds_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(mcp_file_transfer_protocol_bounds_test
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

target_link_libraries(mcp_file_transfer_protocol_bounds_test
    PRIVATE
        MCP::jansson
        MCP::libuv
        Threads::Threads
)

if(MSVC)
    target_compile_definitions(mcp_file_transfer_protocol_bounds_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_file_transfer_protocol_bounds_test PRIVATE /W4)
else()
    target_compile_options(mcp_file_transfer_protocol_bounds_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_file_transfer_protocol_bounds
    COMMAND mcp_file_transfer_protocol_bounds_test
)

add_executable(mcp_json_counter_test
    json_counter_test.c
    "${PROJECT_SOURCE_DIR}/src/registry/tool_registry.c"
)

target_include_directories(mcp_json_counter_test
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)

target_compile_definitions(mcp_json_counter_test
    PRIVATE
        $<$<PLATFORM_ID:Linux>:_POSIX_C_SOURCE=200809L>
)

target_link_libraries(mcp_json_counter_test
    PRIVATE
        MCP::jansson
        MCP::libuv
)

if(MSVC)
    target_compile_definitions(mcp_json_counter_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_json_counter_test PRIVATE /W4)
else()
    target_compile_options(mcp_json_counter_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(
    NAME mcp_json_counter
    COMMAND mcp_json_counter_test
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
        NAME mcp_sandbox_ctl_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/sandbox_ctl_smoke.py" "$<TARGET_FILE:mcp_server>"
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

    if(MCP_TCP_SECURITY STREQUAL "mtls")
        add_test(
            NAME mcp_tls_security_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tls_security_smoke.py" "$<TARGET_FILE:mcp_server>" 18788
        )
    endif()

    add_test(
        NAME mcp_stdio_proxy_adapter_tcp_smoke
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/mcp_stdio_proxy_adapter/tests/proxy_tcp_smoke.py"
                "$<TARGET_FILE:mcp_stdio_proxy_adapter>"
                "$<TARGET_FILE:mcp_server>"
                18789
                "${MCP_TCP_SECURITY}"
                "${MCP_TEST_TLS_DIR}"
    )

    if(MCP_TCP_SECURITY STREQUAL "mtls")
        add_test(
            NAME mcp_mtls_build_plaintext_proxy_smoke
            COMMAND "${Python3_EXECUTABLE}"
                    "${PROJECT_SOURCE_DIR}/mcp_stdio_proxy_adapter/tests/proxy_tcp_smoke.py"
                    "$<TARGET_FILE:mcp_stdio_proxy_adapter>"
                    "$<TARGET_FILE:mcp_server>"
                    18796
                    plaintext
                    "${MCP_TEST_TLS_DIR}"
        )
    endif()

    if(MCP_TCP_SECURITY STREQUAL "mtls")
        add_test(
            NAME mcp_network_access_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/network_access_smoke.py" "$<TARGET_FILE:mcp_server>" 18790 18791 18792 18793 18794
        )
    endif()

    add_test(
        NAME mcp_server_discovery_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server_discovery_smoke.py" "$<TARGET_FILE:mcp_server>" 18768 18769 18770 18771
    )

    if(MCP_TCP_SECURITY STREQUAL "mtls")
        add_test(
            NAME mcp_server_discovery_liveness_smoke
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server_discovery_liveness_smoke.py" "$<TARGET_FILE:mcp_server>" 18772 18773 18774 18775 18776 18777 18778 18779 "${MCP_FILE_TRANSFER_PLUGIN}" "${_mcp_file_transfer_path_plugin}"
        )
    endif()

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

    get_property(_mcp_all_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_mcp_all_tests} PROPERTIES
        ENVIRONMENT
            "MCP_TEST_TLS_DIR=${MCP_TEST_TLS_DIR};MCP_TEST_TCP_SECURITY=${MCP_TCP_SECURITY}"
    )
endif()
