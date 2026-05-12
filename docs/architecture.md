# Architecture

The initial implementation follows the repository design documents with a small executable core:

```text
stdio transport
  -> JSON-RPC protocol handler
  -> session state and tool snapshot
  -> gateway dispatcher
  -> tool registry
  -> built-in tool handlers
```

The gateway is the only `tools/call` execution path. Even local built-in tools are resolved through the registry and gateway so later embedded, remote, and module tools can use the same policy and audit location.

Dynamic plugins are not loaded in this first implementation. The ABI header is present in `include/mcp/plugin/plugin_abi.h`, and `plugin_tools.*` management tools return explicit placeholder results.

Embedded endpoint transport is not active in this first implementation. `include/mcp/embedded/mep.h` captures the MEP v1 constants used by `embedded.get_protocol_info`.
