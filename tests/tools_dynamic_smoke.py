import json
import subprocess
import sys


def send(proc, payload):
    proc.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
    proc.stdin.flush()


def recv(proc):
    line = proc.stdout.readline()
    if not line:
        stderr = proc.stderr.read()
        raise RuntimeError(f"server closed stdout; stderr={stderr}")
    return json.loads(line)


def call_tool(proc, request_id, name, arguments):
    send(
        proc,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
    )
    response = recv(proc)
    assert response["id"] == request_id, response
    return response["result"]


def parse_text_content(result):
    assert "content" in result and result["content"], result
    first = result["content"][0]
    assert first["type"] == "text", result
    return first["text"]


def assert_json_text(result):
    text = parse_text_content(result)
    return json.loads(text)


def verify_tool(proc, request_id, tool_name):
    if tool_name == "system.ping":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        assert parse_text_content(result) == "pong", result
        return

    if tool_name == "system.get_time":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        text = parse_text_content(result)
        assert "T" in text and text.endswith("Z"), result
        return

    if tool_name == "system.get_status":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        payload = assert_json_text(result)
        assert "hostname" in payload, payload
        assert "os" in payload, payload
        return

    if tool_name == "system.shell_exec":
        result = call_tool(proc, request_id, tool_name, {"command": "echo unsafe"})
        assert result["isError"] is False, result
        payload = assert_json_text(result)
        assert payload["stdout"].strip() == "unsafe", payload
        assert payload["sandbox_enabled"] is False, payload
        assert payload["shell_enabled"] is True, payload
        return

    if tool_name == "system.sandbox_ctl":
        result = call_tool(proc, request_id, tool_name, {"action": "get", "token": "disabled"})
        assert result["isError"] is True, result
        payload = assert_json_text(result)
        assert payload["code"] == "unauthorized", payload
        return

    if tool_name == "system.shell_start":
        result = call_tool(proc, request_id, tool_name, {"command": "echo unsafe"})
        assert result["isError"] is True, result
        assert "disabled" in parse_text_content(result), result
        return

    if tool_name in ("system.shell_poll", "system.shell_tail", "system.shell_wait", "system.shell_kill"):
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is True, result
        assert "job_id" in parse_text_content(result), result
        return

    if tool_name == "system.shell_list":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        payload = assert_json_text(result)
        assert "jobs" in payload and isinstance(payload["jobs"], list), payload
        return

    if tool_name == "gateway.status":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        payload = assert_json_text(result)
        assert payload["stdio_transport"] == "enabled", payload
        assert "remote_calls" in payload, payload
        assert "pipe_transport" in payload, payload
        assert "tcp_transport" in payload, payload
        return

    if tool_name == "gateway.proxy_tool":
        result = call_tool(proc, request_id, tool_name, {"server_id": 1, "tool_name": "tools_list"})
        assert result["isError"] is True, result
        assert "not available" in parse_text_content(result), result
        return

    if tool_name == "server.list_servers":
        result = call_tool(proc, request_id, tool_name, {"wait_ms": 1})
        assert result["isError"] is True, result
        assert "not enabled" in parse_text_content(result), result
        return

    if tool_name == "registry.list_tools":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        payload = assert_json_text(result)
        assert "tools" in payload and isinstance(payload["tools"], list), payload
        return

    if tool_name == "plugin_tools.lsmod":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        payload = assert_json_text(result)
        assert "plugins" in payload and isinstance(payload["plugins"], list), payload
        return

    if tool_name == "plugin_tools.insmod":
        result = call_tool(proc, request_id, tool_name, {"package_path": "dummy"})
        assert result["isError"] is True, result
        text = parse_text_content(result)
        assert "dummy" in text or "failed" in text.lower() or "could not be found" in text.lower(), result
        return

    if tool_name == "plugin_tools.rmmod":
        result = call_tool(proc, request_id, tool_name, {"plugin_id": "dummy"})
        assert result["isError"] is True, result
        assert "not active" in parse_text_content(result), result
        return

    if tool_name == "embedded.get_protocol_info":
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is False, result
        payload = assert_json_text(result)
        assert "version" in payload, payload
        assert "frame_types" in payload, payload
        assert "commands" in payload, payload
        return

    if tool_name in ("server.send", "server.recv"):
        result = call_tool(proc, request_id, tool_name, {})
        assert result["isError"] is True, result
        assert "server_id" in parse_text_content(result), result
        return

    raise AssertionError(f"Unhandled tool for smoke verification: {tool_name}")


def main():
    exe = sys.argv[1]
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )

    try:
        send(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "dynamic-smoke", "version": "0.1"},
                },
            },
        )
        init = recv(proc)
        assert init["id"] == 1, init
        assert init["result"]["serverInfo"]["name"] == "mcp_server", init

        send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

        send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        tools_response = recv(proc)
        assert tools_response["id"] == 2, tools_response
        tools = tools_response["result"]["tools"]
        assert tools, tools_response
        tool_names = {tool["name"] for tool in tools}
        assert "system.sandbox_ctl" in tool_names, tool_names
        shell_exec = next(tool for tool in tools if tool["name"] == "system.shell_exec")
        assert shell_exec["annotations"]["timeout_ms"] == 300000, shell_exec
        exec_timeout = shell_exec["inputSchema"]["properties"]["timeout_ms"]
        assert exec_timeout["minimum"] == 1, exec_timeout
        assert exec_timeout["maximum"] == 300000, exec_timeout
        exec_cwd = shell_exec["inputSchema"]["properties"]["cwd"]
        assert exec_cwd["type"] == "string", exec_cwd
        exec_env = shell_exec["inputSchema"]["properties"]["env"]
        assert exec_env["type"] == "object", exec_env
        assert exec_env["additionalProperties"] == {"type": "string"}, exec_env
        shell_start = next(tool for tool in tools if tool["name"] == "system.shell_start")
        start_properties = shell_start["inputSchema"]["properties"]
        assert start_properties["timeout_ms"]["minimum"] == 1, start_properties
        assert start_properties["timeout_ms"]["maximum"] == 300000, start_properties
        assert start_properties["output_limit_bytes"]["minimum"] == 256, start_properties
        assert start_properties["output_limit_bytes"]["maximum"] == 2147483648, start_properties

        if "server.send" in tool_names or "server.recv" in tool_names:
            assert {"server.send", "server.recv"}.issubset(tool_names), tool_names
            result = call_tool(proc, 1000, "plugin_tools.lsmod", {})
            payload = assert_json_text(result)
            plugin = next(
                item
                for item in payload["plugins"]
                if item["plugin_id"] == "mcp_file_transfer_plugin"
            )
            assert plugin["builtin"] is True, plugin
            assert sorted(plugin["tools"]) == ["server.recv", "server.send"], plugin
            result = call_tool(proc, 1001, "plugin_tools.rmmod", {"plugin_id": "mcp_file_transfer_plugin"})
            assert result["isError"] is True, result
            assert "built into" in parse_text_content(result), result

        for index, tool in enumerate(tools, start=3):
            verify_tool(proc, index, tool["name"])
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
