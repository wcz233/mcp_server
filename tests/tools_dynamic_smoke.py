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
        assert result["isError"] is True, result
        assert "disabled" in parse_text_content(result), result
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
        assert "dummy" in parse_text_content(result) or "Failed" in parse_text_content(result), result
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
        shell_exec = next(tool for tool in tools if tool["name"] == "system.shell_exec")
        assert shell_exec["annotations"]["timeout_ms"] == 5000, shell_exec

        for index, tool in enumerate(tools, start=3):
            verify_tool(proc, index, tool["name"])
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
