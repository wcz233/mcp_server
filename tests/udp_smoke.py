import json
import os
import socket
import subprocess
import sys
import time


def encode(payload):
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def recv_json(sock):
    data, _ = sock.recvfrom(1024 * 1024)
    return json.loads(data.decode("utf-8").strip())


def send(sock, port, payload):
    sock.sendto(encode(payload), ("127.0.0.1", port))


def start_server(exe, port):
    env = os.environ.copy()
    env["MCP_ENABLE_UDP"] = "1"
    env["MCP_UDP_HOST"] = "127.0.0.1"
    env["MCP_UDP_PORT"] = str(port)
    return subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )


def wait_for_udp(sock, port, proc):
    deadline = time.time() + 5
    request_id = 1000

    while time.time() < deadline:
        request_id += 1
        send(sock, port, {"jsonrpc": "2.0", "id": request_id, "method": "ping", "params": {}})
        try:
            response = recv_json(sock)
        except (socket.timeout, ConnectionResetError):
            if proc.poll() is not None:
                raise RuntimeError(f"server exited early; stderr={proc.stderr.read()}")
            continue
        if response.get("id") == request_id:
            return

    raise RuntimeError("UDP transport did not become ready")


def call(sock, port, request_id, method, params=None):
    payload = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        payload["params"] = params
    send(sock, port, payload)
    response = recv_json(sock)
    assert response["id"] == request_id, response
    return response


def call_tool(sock, port, request_id, name, arguments):
    response = call(
        sock,
        port,
        request_id,
        "tools/call",
        {"name": name, "arguments": arguments},
    )
    return response["result"]


def parse_text(result):
    assert result["content"][0]["type"] == "text", result
    return result["content"][0]["text"]


def expect_error(response, code, message):
    assert "error" in response, response
    assert response["error"]["code"] == code, response
    assert response["error"]["message"] == message, response


def main():
    exe = sys.argv[1]
    port = int(sys.argv[2])
    proc = start_server(exe, port)
    sock_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_a.settimeout(0.5)
    sock_b.settimeout(0.5)

    try:
        wait_for_udp(sock_a, port, proc)

        expect_error(
            call(sock_b, port, 90, "tools/list", {}),
            -32600,
            "Session not initialized",
        )

        init = call(
            sock_a,
            port,
            1,
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "udp-smoke", "version": "0.1"},
            },
        )
        assert init["result"]["serverInfo"]["name"] == "mcp_server", init

        send(sock_a, port, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

        expect_error(
            call(sock_b, port, 91, "tools/call", {"name": "system.ping", "arguments": {}}),
            -32600,
            "Session not initialized",
        )

        tools = call(sock_a, port, 2, "tools/list", {})["result"]["tools"]
        names = {tool["name"] for tool in tools}
        assert "system.ping" in names, tools
        assert "gateway.status" in names, tools

        ping = call_tool(sock_a, port, 3, "system.ping", {})
        assert ping["isError"] is False, ping
        assert parse_text(ping) == "pong", ping

        status = call_tool(sock_a, port, 4, "gateway.status", {})
        assert status["isError"] is False, status
        payload = json.loads(parse_text(status))
        assert payload["stdio_transport"] == "enabled", payload
        assert payload["udp_transport"] == "enabled", payload

        init_b = call(
            sock_b,
            port,
            5,
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "udp-smoke-b", "version": "0.1"},
            },
        )
        assert init_b["result"]["serverInfo"]["name"] == "mcp_server", init_b
        send(sock_b, port, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

        ping_b = call_tool(sock_b, port, 6, "system.ping", {})
        assert ping_b["isError"] is False, ping_b
        assert parse_text(ping_b) == "pong", ping_b
    finally:
        sock_a.close()
        sock_b.close()
        if proc.stdin:
            proc.stdin.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
