if(NOT DEFINED OPENSSL_EXECUTABLE OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OPENSSL_EXECUTABLE and OUTPUT_DIR are required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

function(run_openssl)
    execute_process(
        COMMAND "${OPENSSL_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_QUIET
        ERROR_VARIABLE _error
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "OpenSSL fixture generation failed: ${_error}")
    endif()
endfunction()

run_openssl(ecparam -name prime256v1 -genkey -noout -out "${OUTPUT_DIR}/ca.key.pem")
run_openssl(req -x509 -new -sha256
    -key "${OUTPUT_DIR}/ca.key.pem"
    -out "${OUTPUT_DIR}/ca.cert.pem"
    -days 3650
    -subj /CN=MCP-Test-CA
    -addext basicConstraints=critical,CA:TRUE
    -addext keyUsage=critical,keyCertSign,cRLSign)

function(issue_certificate name subject issuer_prefix eku san)
    run_openssl(ecparam -name prime256v1 -genkey -noout
        -out "${OUTPUT_DIR}/${name}.key.pem")
    set(_request_args req -new -sha256
        -key "${OUTPUT_DIR}/${name}.key.pem"
        -out "${OUTPUT_DIR}/${name}.csr.pem"
        -subj "/CN=${subject}")
    set(_extensions_file "${OUTPUT_DIR}/${name}.ext.cnf")
    file(WRITE "${_extensions_file}"
        "[v3_cert]\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=${eku}\n")
    if(NOT "${san}" STREQUAL "")
        file(APPEND "${_extensions_file}" "subjectAltName=${san}\n")
    endif()
    run_openssl(${_request_args})
    run_openssl(x509 -req -sha256
        -in "${OUTPUT_DIR}/${name}.csr.pem"
        -CA "${OUTPUT_DIR}/${issuer_prefix}.cert.pem"
        -CAkey "${OUTPUT_DIR}/${issuer_prefix}.key.pem"
        -CAcreateserial
        -out "${OUTPUT_DIR}/${name}.cert.pem"
        -days 3650
        -extfile "${_extensions_file}"
        -extensions v3_cert)
    file(REMOVE "${_extensions_file}")
endfunction()

issue_certificate(node-a localhost ca serverAuth,clientAuth DNS:localhost,IP:127.0.0.1)
issue_certificate(node-b localhost ca serverAuth,clientAuth DNS:localhost,IP:127.0.0.1)
issue_certificate(adapter mcp-stdio-proxy-test ca clientAuth "")

run_openssl(ecparam -name prime256v1 -genkey -noout
    -out "${OUTPUT_DIR}/untrusted-ca.key.pem")
run_openssl(req -x509 -new -sha256
    -key "${OUTPUT_DIR}/untrusted-ca.key.pem"
    -out "${OUTPUT_DIR}/untrusted-ca.cert.pem"
    -days 3650
    -subj /CN=MCP-Untrusted-Test-CA
    -addext basicConstraints=critical,CA:TRUE
    -addext keyUsage=critical,keyCertSign,cRLSign)
issue_certificate(untrusted-client untrusted-client untrusted-ca clientAuth "")

file(REMOVE
    "${OUTPUT_DIR}/ca.key.pem"
    "${OUTPUT_DIR}/ca.cert.srl"
    "${OUTPUT_DIR}/untrusted-ca.key.pem"
    "${OUTPUT_DIR}/untrusted-ca.cert.srl"
    "${OUTPUT_DIR}/node-a.csr.pem"
    "${OUTPUT_DIR}/node-b.csr.pem"
    "${OUTPUT_DIR}/adapter.csr.pem"
    "${OUTPUT_DIR}/untrusted-client.csr.pem"
)
