#ifndef MCP_SRC_SECURITY_TLS_CONTEXT_H
#define MCP_SRC_SECURITY_TLS_CONTEXT_H

#include <stddef.h>

#include <mbedtls/ssl.h>

#define MCP_TLS_CERT_FINGERPRINT_HEX_SIZE 65

struct mcp_tls_context;

int mcp_tls_context_create(struct mcp_tls_context **out,
                           const char *ca_file,
                           const char *cert_file,
                           const char *key_file,
                           char *error,
                           size_t error_size);
void mcp_tls_context_destroy(struct mcp_tls_context *context);

const mbedtls_ssl_config *mcp_tls_context_server_config(const struct mcp_tls_context *context);
const mbedtls_ssl_config *mcp_tls_context_client_config(const struct mcp_tls_context *context);
const char *mcp_tls_context_certificate_fingerprint(const struct mcp_tls_context *context);

int mcp_tls_certificate_fingerprint(const mbedtls_x509_crt *certificate,
                                    char out[MCP_TLS_CERT_FINGERPRINT_HEX_SIZE]);

#endif
