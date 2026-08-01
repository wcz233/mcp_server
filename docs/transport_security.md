# Transport Security

## Build Modes

Both `mcp_server` and `mcp_stdio_proxy_adapter` use the CMake cache variable
`MCP_TCP_SECURITY`; the default is `mtls`.

```bash
cmake -S . -B build-plaintext -DMCP_TCP_SECURITY=plaintext -DCMAKE_BUILD_TYPE=Release
cmake --build build-plaintext --parallel

cmake -S . -B build-mtls -DMCP_TCP_SECURITY=mtls -DCMAKE_BUILD_TYPE=Release
cmake --build build-mtls --parallel
```

The plaintext build omits all TLS sources and Mbed TLS targets. The adapter also omits
Jansson because it has no security JSON to parse. Its TCP framing and discovery behavior
match the implementation before mTLS was introduced. The server still uses Jansson for
its MCP protocol and other existing JSON configuration.

The mTLS build includes bundled Mbed TLS plus the security JSON parser. Bundled Mbed TLS
generates a few sources during the build. Install its pinned Python generator requirements
before the first mTLS build:

```bash
python3 -m pip install -r external/mbedtls/scripts/basic.requirements.txt
```

No OpenSSL runtime library is required. OpenSSL is used only when the test suite creates
test certificates.

## Runtime Selection

Runtime selection applies only when TCP is enabled. `network_security.json` is located by
`MCP_NETWORK_SECURITY_CONFIG`; the server otherwise tries its compiled source path and
installed data path. The adapter also accepts `--network-security-config`, which has
priority over the environment variable.

| Compiled mode | `MCP_TCP_SECURITY` | JSON `enabled` | Result |
| --- | --- | --- | --- |
| `plaintext` | unset or `plaintext` | not read | plaintext TCP |
| `plaintext` | `mtls` | not read | startup failure |
| `mtls` | unset | `false` / `true` | JSON selects plaintext / mTLS |
| `mtls` | `plaintext` | `false` | plaintext TCP |
| `mtls` | `mtls` | `true` | mTLS TCP |
| `mtls` | explicit mode | conflicting value | startup failure |

Invalid values and conflicts fail closed. Named pipes, Unix-domain sockets, stdio, and UDP
are unaffected by this selector.

## Configuration Schema

`config/network/network_security.json` is strict JSON v1. Every field is required,
duplicate or unknown keys are rejected, and `enabled=true` requires non-empty certificate
paths.

```json
{
  "version": 1,
  "enabled": true,
  "mtls": {
    "ca_file": "/etc/mcp/tls/ca.cert.pem",
    "certificate_file": "/etc/mcp/tls/node.cert.pem",
    "private_key_file": "/etc/mcp/tls/node.key.pem",
    "server_name": "mcp-node.example.internal"
  }
}
```

The server ignores `server_name`. The adapter validates the server certificate against it;
an empty value falls back to `--host`. With `enabled=false`, all four strings may be empty.
Keep private keys outside the repository and restrict them to the service identity.

## mTLS Contract

When enabled, every TCP connection uses mutually authenticated TLS 1.3. TLS 1.2, PSK,
session tickets, 0-RTT, and same-port plaintext fallback are disabled. Existing
`uint32_be length + JSON/MFT1 payload` framing is carried inside TLS unchanged.

Use a private CA dedicated to the MCP trust domain and issue a distinct ECDSA P-256 leaf
certificate to each node and adapter. Server nodes need `serverAuth` and `clientAuth` EKUs;
adapter certificates need `clientAuth`. Put all adapter hostnames and advertised discovery
IP addresses in server certificate SANs.

UDP discovery remains an unauthenticated locator. In mTLS mode, an endpoint becomes usable
only after the network allowlist permits it, its certificate chains to the configured CA,
the SAN matches, and the advertised and initialized certificate fingerprints match the live
TLS connection. Plaintext mode intentionally restores the pre-mTLS discovery protocol and
does not send or require certificate fingerprints.

## Deployment

Deploy matching server and adapter binaries together with:

- `config/network/network_white_list.json`
- `config/network/network_security.json` for mTLS-capable builds
- certificate files referenced by the security JSON when enabled

For a LAN that can only use plaintext TCP, prefer a plaintext build. If distributing one
mTLS-capable binary to mixed nodes, deploy a node-specific security JSON with
`enabled=false` and set `MCP_TCP_SECURITY=plaintext` on plaintext nodes. Never copy a single
node's private key to other nodes.

`tests/generate_tls_fixtures.cmake` creates test-only certificates under the build directory;
they must not be deployed.
