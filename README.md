# MCP Server

This repository contains the host-side MCP Server implementation.

Current scope:

- stdio JSON-RPC transport for local MCP clients.
- UDP JSON-RPC datagram transport for lightweight LAN clients, enabled at runtime.
- `initialize`, `ping`, `tools/list`, `tools/call`, `resources/list`, `prompts/list`.
- Session-level tool snapshot for stable `tools/list` behavior.
- Gateway + registry dispatch path for built-in, module, embedded, and remote routing metadata.
- Built-in tools:
  - `system.ping`
  - `system.get_time`
  - `system.get_status`
  - `system.shell_exec` disabled unless `MCP_ENABLE_SHELL_EXEC=1`
  - `gateway.status`
  - `registry.list_tools`
  - `plugin_tools.lsmod`
  - `plugin_tools.insmod` placeholder
  - `plugin_tools.rmmod` placeholder
  - `embedded.get_protocol_info`

## Repository Layout

- `external/`: locked third-party source submodules.
- `cmake/`: build helpers and third-party policy.
- `config/`: reproducible build presets.
- `src/`, `include/`, `tests/`: server code, headers, and smoke tests.
- `scripts/`: bootstrap helpers for submodule initialization.

## Bootstrap

Clone with submodules when possible:

```powershell
git clone --recursive <repo-url> mcp_server
```

If you already have the repository checkout, initialize submodules before configuring:

```powershell
git submodule update --init --recursive
```

Windows helper:

```powershell
.\scripts\bootstrap.ps1
```

POSIX helper:

```bash
./scripts/bootstrap.sh
```

## Build

The default build uses bundled source from `external/libuv` and `external/jansson`.

```powershell
cmake -S . -B build -C config/gateway_defconfig.cmake
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

For single-config generators such as Ninja:

```powershell
cmake -S . -B build-ninja -G Ninja -C config/dev_defconfig.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ninja
ctest --test-dir build-ninja --output-on-failure
```

Optional system-library mode is available for controlled environments:

```powershell
cmake -S . -B build-system -C config/gateway_defconfig.cmake `
  -DMCP_USE_BUNDLED_LIBUV=OFF `
  -DMCP_USE_SYSTEM_LIBUV=ON `
  -DMCP_USE_BUNDLED_JANSSON=OFF `
  -DMCP_USE_SYSTEM_JANSSON=ON
```

## Runtime Policy

`system.shell_exec` is intentionally visible but disabled by default because it is a high-risk host action.
Enable it only in a trusted local session:

```powershell
$env:MCP_ENABLE_SHELL_EXEC = "1"
.\build\mcp_server.exe
```

UDP transport is compiled when `MCP_TRANSPORT_UDP=ON` and is disabled at runtime unless explicitly requested:

```powershell
$env:MCP_ENABLE_UDP = "1"
$env:MCP_UDP_HOST = "127.0.0.1"
$env:MCP_UDP_PORT = "8765"
.\build\src\Debug\mcp_server.exe
```

The initial UDP mode maps one datagram to one compact JSON-RPC message and sends each response back to the datagram sender.

TCP framed transport is also disabled at runtime unless explicitly requested.
On Linux, the server can listen on a specific IP and port via `MCP_TCP_HOST` and `MCP_TCP_PORT`.
For a TCP-only listener, disable stdio explicitly:

```bash
MCP_ENABLE_STDIO=0 \
MCP_ENABLE_TCP=1 \
MCP_TCP_HOST=127.0.0.1 \
MCP_TCP_PORT=18767 \
./build-ninja/src/mcp_server
```

To bind on all interfaces, set `MCP_TCP_HOST=0.0.0.0`.
If the binary was built into a different directory, replace `./build-ninja/src/mcp_server` with the corresponding `./<build-dir>/src/mcp_server`.
If you want stdio and TCP enabled at the same time, omit `MCP_ENABLE_STDIO=0`.

TCP defaults:

- `MCP_TCP_HOST=127.0.0.1`
- `MCP_TCP_PORT=8765`

The TCP transport uses framed JSON-RPC messages: each request or response is encoded as a 4-byte big-endian frame length followed by one UTF-8 JSON message.
A minimal client-side framing example is available in `tests/tcp_framed_smoke.py`.

On Linux, the listener can be verified with:

```bash
ss -ltn | grep 18767
```

The repository policy for third-party updates and version locks is documented in `docs/dependency_policy.md` and `third_party.lock`.
