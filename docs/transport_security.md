# Transport Security

## Security Contract

- Every TCP connection uses TLS 1.3 with mutual X.509 authentication.
- The CA, leaf certificate, and private key are mandatory when TCP is enabled.
- TLS 1.2, plaintext fallback, PSK key exchange, session tickets, and 0-RTT are not accepted.
- Existing `uint32_be length + JSON/MFT1 payload` framing is carried inside TLS unchanged.
- Named pipes and Unix-domain sockets remain local transports and do not use TLS.

Mbed TLS performs an ephemeral TLS 1.3 key exchange. The certificates authenticate each
endpoint; the handshake derives fresh symmetric traffic keys. Application payloads are then
encrypted and integrity-protected with the negotiated TLS 1.3 AEAD cipher suite.

## Certificate Policy

Use a private CA dedicated to the MCP trust domain. Issue a different ECDSA P-256 leaf
certificate and private key to every server and adapter instance. Server nodes need both
`serverAuth` and `clientAuth` extended key usages because they accept inbound connections and
initiate discovery peer connections. Adapter certificates need `clientAuth`. Put every DNS name
or IP address used by an adapter in the server certificate SAN. Every IP address advertised by
server discovery must also appear as an IP SAN in that server node's certificate.

Keep CA private keys offline. Runtime hosts receive only the CA certificate, their own leaf
certificate, and their own private key. Restrict private-key reads to the service identity. Rotate
a node by issuing a new leaf certificate and restarting that node; revoke or remove trust for a
compromised CA rather than distributing private keys between nodes.

## Runtime Configuration

The server requires these environment variables whenever `MCP_ENABLE_TCP=1`:

```text
MCP_TLS_CA_FILE=/absolute/path/ca.cert.pem
MCP_TLS_CERT_FILE=/absolute/path/node.cert.pem
MCP_TLS_KEY_FILE=/absolute/path/node.key.pem
```

The adapter TCP mode requires equivalent command-line arguments:

```text
--tls-ca FILE --tls-cert FILE --tls-key FILE [--tls-server-name NAME]
```

`--tls-server-name` defaults to `--host` and is checked against the server certificate SAN.
There is deliberately no private-key password command-line option because process arguments are
observable; deploy an unencrypted service key protected by operating-system file permissions or
add an external secret-provider integration as a separate change.

## Discovery Trust Boundary

UDP discovery is an unauthenticated locator, not an authorization channel. Announcements carry
the leaf-certificate SHA-256 fingerprint. A discovered endpoint becomes usable only after all of
the following checks pass:

1. Network allowlist policy permits the address.
2. The TLS certificate chains to the configured CA.
3. The certificate IP SAN matches the address advertised by discovery.
4. The live leaf certificate fingerprint matches the advertised fingerprint.
5. `initialize.mcp_peer_identity.certificate_fingerprint` matches the live TLS certificate.

A forged UDP packet can cause connection attempts or temporary liveness noise, but it cannot
create an authenticated MCP peer or expose framed payloads. Deployments that require discovery
availability against active datagram injection should place discovery on a protected network or
add a signed, replay-protected discovery protocol as a separately versioned change.

## Test Material

`tests/generate_tls_fixtures.cmake` creates test-only certificates and private keys under the
build directory. They must never be installed in a real environment.
