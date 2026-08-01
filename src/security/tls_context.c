#include "security/tls_context.h"

#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mcp_tls_context {
    mbedtls_x509_crt ca;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_ssl_config server_config;
    mbedtls_ssl_config client_config;
    char fingerprint[MCP_TLS_CERT_FINGERPRINT_HEX_SIZE];
};

static void set_error(char *error, size_t error_size, const char *operation, int rc)
{
    char detail[128];

    if (!error || error_size == 0)
        return;
    mbedtls_strerror(rc, detail, sizeof(detail));
    snprintf(error, error_size, "%s: %s (-0x%04x)", operation, detail, (unsigned int)-rc);
}

int mcp_tls_certificate_fingerprint(const mbedtls_x509_crt *certificate,
                                    char out[MCP_TLS_CERT_FINGERPRINT_HEX_SIZE])
{
    unsigned char digest[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t digest_length = 0;
    size_t index;
    psa_status_t status;

    if (!certificate || !certificate->raw.p || certificate->raw.len == 0 || !out)
        return -1;

    status = psa_hash_compute(PSA_ALG_SHA_256,
                              certificate->raw.p,
                              certificate->raw.len,
                              digest,
                              sizeof(digest),
                              &digest_length);
    if (status != PSA_SUCCESS || digest_length != sizeof(digest))
        return -1;

    for (index = 0; index < digest_length; index++)
        snprintf(out + index * 2, 3, "%02x", digest[index]);
    out[digest_length * 2] = '\0';
    return 0;
}

static int configure_endpoint(mbedtls_ssl_config *config,
                              int endpoint,
                              struct mcp_tls_context *context,
                              char *error,
                              size_t error_size)
{
    int rc = mbedtls_ssl_config_defaults(config,
                                         endpoint,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        set_error(error, error_size, "mbedtls_ssl_config_defaults", rc);
        return -1;
    }

    mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(config, &context->ca, NULL);
    mbedtls_ssl_conf_min_tls_version(config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_tls13_key_exchange_modes(
        config,
        MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
    if (endpoint == MBEDTLS_SSL_IS_CLIENT)
        mbedtls_ssl_conf_session_tickets(config, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    else
        mbedtls_ssl_conf_new_session_tickets(config, 0);

    rc = mbedtls_ssl_conf_own_cert(config, &context->certificate, &context->private_key);
    if (rc != 0) {
        set_error(error, error_size, "mbedtls_ssl_conf_own_cert", rc);
        return -1;
    }
    return 0;
}

int mcp_tls_context_create(struct mcp_tls_context **out,
                           const char *ca_file,
                           const char *cert_file,
                           const char *key_file,
                           char *error,
                           size_t error_size)
{
    struct mcp_tls_context *context;
    int rc;

    if (error && error_size > 0)
        error[0] = '\0';
    if (out)
        *out = NULL;
    if (!out || !ca_file || !cert_file || !key_file ||
        ca_file[0] == '\0' || cert_file[0] == '\0' || key_file[0] == '\0') {
        if (error && error_size > 0)
            snprintf(error, error_size, "CA, certificate, and private key files are required");
        return -1;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        if (error && error_size > 0)
            snprintf(error, error_size, "psa_crypto_init failed");
        return -1;
    }

    context = calloc(1, sizeof(*context));
    if (!context) {
        if (error && error_size > 0)
            snprintf(error, error_size, "allocate TLS context failed");
        return -1;
    }
    mbedtls_x509_crt_init(&context->ca);
    mbedtls_x509_crt_init(&context->certificate);
    mbedtls_pk_init(&context->private_key);
    mbedtls_ssl_config_init(&context->server_config);
    mbedtls_ssl_config_init(&context->client_config);

    rc = mbedtls_x509_crt_parse_file(&context->ca, ca_file);
    if (rc < 0) {
        set_error(error, error_size, "load CA certificate", rc);
        goto fail;
    }
    rc = mbedtls_x509_crt_parse_file(&context->certificate, cert_file);
    if (rc < 0) {
        set_error(error, error_size, "load node certificate", rc);
        goto fail;
    }
    rc = mbedtls_pk_parse_keyfile(&context->private_key, key_file, NULL);
    if (rc != 0) {
        set_error(error, error_size, "load node private key", rc);
        goto fail;
    }
    if (mcp_tls_certificate_fingerprint(&context->certificate, context->fingerprint) != 0) {
        if (error && error_size > 0)
            snprintf(error, error_size, "calculate node certificate fingerprint failed");
        goto fail;
    }
    if (configure_endpoint(&context->server_config,
                           MBEDTLS_SSL_IS_SERVER,
                           context,
                           error,
                           error_size) != 0 ||
        configure_endpoint(&context->client_config,
                           MBEDTLS_SSL_IS_CLIENT,
                           context,
                           error,
                           error_size) != 0)
        goto fail;

    *out = context;
    return 0;

fail:
    mcp_tls_context_destroy(context);
    return -1;
}

void mcp_tls_context_destroy(struct mcp_tls_context *context)
{
    if (!context)
        return;
    mbedtls_ssl_config_free(&context->client_config);
    mbedtls_ssl_config_free(&context->server_config);
    mbedtls_pk_free(&context->private_key);
    mbedtls_x509_crt_free(&context->certificate);
    mbedtls_x509_crt_free(&context->ca);
    free(context);
}

const mbedtls_ssl_config *mcp_tls_context_server_config(const struct mcp_tls_context *context)
{
    return context ? &context->server_config : NULL;
}

const mbedtls_ssl_config *mcp_tls_context_client_config(const struct mcp_tls_context *context)
{
    return context ? &context->client_config : NULL;
}

const char *mcp_tls_context_certificate_fingerprint(const struct mcp_tls_context *context)
{
    return context ? context->fingerprint : NULL;
}
