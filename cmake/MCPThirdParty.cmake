option(MCP_USE_BUNDLED_LIBUV "Use bundled libuv from external/libuv" ON)
option(MCP_USE_BUNDLED_JANSSON "Use bundled jansson from external/jansson" ON)
option(MCP_USE_SYSTEM_LIBUV "Use system installed libuv" OFF)
option(MCP_USE_SYSTEM_JANSSON "Use system installed jansson" OFF)
option(MCP_ALLOW_FETCHCONTENT "Allow CMake to fetch missing third-party dependencies" OFF)

function(mcp_require_submodule NAME PATH EXPECTED_FILE)
    if(NOT EXISTS "${PATH}/${EXPECTED_FILE}")
        message(FATAL_ERROR
            "Required submodule ${NAME} is missing at ${PATH}.\n"
            "Run `git submodule update --init --recursive` or use scripts/bootstrap."
        )
    endif()
endfunction()

function(_mcp_create_link_alias ALIAS_NAME BACKING_TARGET)
    if(TARGET "${ALIAS_NAME}")
        return()
    endif()

    if(NOT TARGET "${BACKING_TARGET}")
        message(FATAL_ERROR "Third-party target not found: ${BACKING_TARGET}")
    endif()

    string(REPLACE "::" "_" _impl_target "${ALIAS_NAME}")
    add_library(${_impl_target} INTERFACE)
    target_link_libraries(${_impl_target} INTERFACE "${BACKING_TARGET}")
    add_library(${ALIAS_NAME} ALIAS ${_impl_target})
endfunction()

function(_mcp_setup_system_libuv)
    find_package(libuv QUIET CONFIG)

    if(TARGET libuv::libuv)
        _mcp_create_link_alias(MCP::libuv libuv::libuv)
        return()
    endif()

    if(TARGET uv_a)
        _mcp_create_link_alias(MCP::libuv uv_a)
        return()
    endif()

    if(TARGET uv)
        _mcp_create_link_alias(MCP::libuv uv)
        return()
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUV QUIET IMPORTED_TARGET libuv)
        if(TARGET PkgConfig::LIBUV)
            _mcp_create_link_alias(MCP::libuv PkgConfig::LIBUV)
            return()
        endif()
    endif()

    message(FATAL_ERROR
        "System libuv not found. Set MCP_USE_SYSTEM_LIBUV=OFF and initialize external/libuv."
    )
endfunction()

function(_mcp_setup_system_jansson)
    find_package(jansson QUIET CONFIG)

    if(TARGET jansson::jansson)
        _mcp_create_link_alias(MCP::jansson jansson::jansson)
        return()
    endif()

    if(TARGET jansson)
        _mcp_create_link_alias(MCP::jansson jansson)
        return()
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(JANSSON QUIET IMPORTED_TARGET jansson)
        if(TARGET PkgConfig::JANSSON)
            _mcp_create_link_alias(MCP::jansson PkgConfig::JANSSON)
            return()
        endif()
    endif()

    message(FATAL_ERROR
        "System jansson not found. Set MCP_USE_SYSTEM_JANSSON=OFF and initialize external/jansson."
    )
endfunction()

function(mcp_setup_third_party)
    if(MCP_ALLOW_FETCHCONTENT)
        message(FATAL_ERROR
            "MCP_ALLOW_FETCHCONTENT is disabled by repository policy. Initialize submodules instead."
        )
    endif()

    if(MCP_USE_SYSTEM_LIBUV AND MCP_USE_BUNDLED_LIBUV)
        message(FATAL_ERROR "Choose either system libuv or bundled libuv, not both")
    endif()

    if(MCP_USE_SYSTEM_JANSSON AND MCP_USE_BUNDLED_JANSSON)
        message(FATAL_ERROR "Choose either system jansson or bundled jansson, not both")
    endif()

    if(NOT MCP_USE_SYSTEM_LIBUV AND NOT MCP_USE_BUNDLED_LIBUV)
        message(FATAL_ERROR "libuv must be provided by either system or bundled mode")
    endif()

    if(NOT MCP_USE_SYSTEM_JANSSON AND NOT MCP_USE_BUNDLED_JANSSON)
        message(FATAL_ERROR "jansson must be provided by either system or bundled mode")
    endif()

    if(MCP_USE_BUNDLED_LIBUV)
        mcp_require_submodule("libuv" "${PROJECT_SOURCE_DIR}/external/libuv" "CMakeLists.txt")
    else()
        _mcp_setup_system_libuv()
    endif()

    if(MCP_USE_BUNDLED_JANSSON)
        mcp_require_submodule("jansson" "${PROJECT_SOURCE_DIR}/external/jansson" "CMakeLists.txt")
    else()
        _mcp_setup_system_jansson()
    endif()
endfunction()
