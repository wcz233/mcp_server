import json
import os
import socket
import struct
import subprocess
import sys
import time


def encode(payload):
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(data)) + data


def recv_frame(sock):
    header = sock.recv(4)
    if len(header) != 4:
        raise RuntimeError("short frame header")
    (length,) = struct.unpack(">I", header)
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise RuntimeError("short frame body")
        data.extend(chunk)
    return json.loads(data.decode("utf-8"))


def call(sock, request_id, method, params=None):
    payload = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        payload["params"] = params
    sock.sendall(encode(payload))
    response = recv_frame(sock)
    assert response["id"] == request_id, response
    return response


def wait_for_tcp(port, proc):
    deadline = time.time() + 5
    last_error = None
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early; stderr={proc.stderr.read()}")
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"tcp listener did not become ready: {last_error}")


def main():
    exe = sys.argv[1]
    port = int(sys.argv[2])
    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(port)
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )

    sock = None
    try:
        sock = wait_for_tcp(port, proc)
        init = call(
            sock,
            1,
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "tcp-framed-smoke", "version": "0.1"},
            },
        )
        assert init["result"]["serverInfo"]["name"] == "mcp_server", init

        sock.sendall(encode({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}))

        tools = call(sock, 2, "tools/list", {})["result"]["tools"]
        names = {tool["name"] for tool in tools}
        assert "system.ping" in names, tools
        assert "gateway.status" in names, tools

        ping = call(
            sock,
            3,
            "tools/call",
            {"name": "system.ping", "arguments": {}},
        )
        assert ping["result"]["isError"] is False, ping
        assert ping["result"]["content"][0]["text"] == "pong", ping
    finally:
        if sock:
            sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
