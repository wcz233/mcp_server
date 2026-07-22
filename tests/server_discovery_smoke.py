import json
import os
import shlex
import socket
import struct
import subprocess
import signal
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
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"tcp listener did not become ready: {last_error}")


def start_server(exe, tcp_port, discovery_port, peer_discovery_port, shell_exec="0"):
    env = os.environ.copy()
    creationflags = 0
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(tcp_port)
    env["MCP_ENABLE_DISCOVERY"] = "1"
    env["MCP_DISCOVERY_BIND_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_PORT"] = str(discovery_port)
    env["MCP_DISCOVERY_ADVERTISE_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_HOSTS"] = f"127.0.0.1:{peer_discovery_port}"
    env["MCP_STRICT_INIT"] = "0"
    env["MCP_ENABLE_SHELL_EXEC"] = shell_exec
    if os.name == "nt":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
    return subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
        creationflags=creationflags,
    )


def start_signal_server(exe, tcp_port, discovery_port, peer_discovery_port):
    return start_server(exe, tcp_port, discovery_port, peer_discovery_port)


def request_graceful_shutdown(proc):
    if os.name == "nt":
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGINT)


def initialize(sock):
    init = call(
        sock,
        1,
        "initialize",
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "server-discovery-smoke", "version": "0.1"},
        },
    )
    assert init["result"]["serverInfo"]["name"] == "mcp_server", init


def call_tool(sock, request_id, name, arguments=None):
    response = call(
        sock,
        request_id,
        "tools/call",
        {"name": name, "arguments": arguments or {}},
    )
    result = response["result"]
    assert result["content"][0]["type"] == "text", result
    return result


def parse_text_json(result):
    assert result["isError"] is False, result
    return json.loads(result["content"][0]["text"])


def slow_python_command(seconds, text):
    code = f"import time; time.sleep({seconds}); print({text!r})"
    if os.name == "nt":
        return subprocess.list2cmdline([sys.executable, "-c", code])
    return f"{shlex.quote(sys.executable)} -c {shlex.quote(code)}"


def main():
    exe = sys.argv[1]
    tcp_a = int(sys.argv[2])
    tcp_b = int(sys.argv[3])
    discovery_a = int(sys.argv[4])
    discovery_b = int(sys.argv[5])
    proc_a = start_server(exe, tcp_a, discovery_a, discovery_b)
    proc_b = None
    sock_a = None
    sock_b = None

    try:
        sock_a = wait_for_tcp(tcp_a, proc_a)
        sock_a.settimeout(3.0)
        initialize(sock_a)

        tools = call(sock_a, 2, "tools/list", {})["result"]["tools"]
        names = {tool["name"] for tool in tools}
        assert "server.list_servers" in names, names
        assert "gateway.proxy_tool" in names, names

        result = call_tool(sock_a, 3, "server.list_servers", {"wait_ms": 100})
        local_payload = parse_text_json(result)
        assert local_payload["total"] == 1, local_payload
        local = local_payload["servers"][0]
        assert local["address"] == f"127.0.0.1:{tcp_a}", local
        assert local["server_id"] == 0, local
        assert local["ip"] == "127.0.0.1", local
        assert local["port"] == tcp_a, local
        assert local["scope"] == "local", local
        assert local["state"] == "online", local
        assert local["tcp_connected"] is True, local
        assert "system_status" in local, local
        assert "hostname" in local["system_status"], local

        proc_b = start_server(exe, tcp_b, discovery_b, discovery_a, shell_exec="1")
        sock_b = wait_for_tcp(tcp_b, proc_b)
        sock_b.settimeout(3.0)
        initialize(sock_b)

        payload = None
        deadline = time.time() + 5
        while time.time() < deadline:
            result = call_tool(sock_a, 4, "server.list_servers", {"wait_ms": 500})
            payload = parse_text_json(result)
            if any(
                server["address"] == f"127.0.0.1:{tcp_b}" and server["state"] == "online"
                for server in payload["servers"]
            ):
                break
            time.sleep(0.1)

        assert payload is not None, "missing discovery response"
        assert payload["total"] >= 2, payload
        local = next(server for server in payload["servers"] if server["address"] == f"127.0.0.1:{tcp_a}")
        assert local["scope"] == "local", local
        assert local["server_id"] == 0, local
        peer = next(server for server in payload["servers"] if server["address"] == f"127.0.0.1:{tcp_b}")
        peer_server_id = peer["server_id"]
        assert peer_server_id > 0, peer
        assert peer["ip"] == "127.0.0.1", peer
        assert peer["port"] == tcp_b, peer
        assert peer["scope"] == "remote", peer
        assert peer["state"] == "online", peer
        assert "system_status" in peer, peer
        assert "hostname" in peer["system_status"], peer

        result = call_tool(
            sock_a,
            5,
            "gateway.proxy_tool",
            {"server_id": peer_server_id, "tool_name": "tools_list", "args": {}},
        )
        tools_payload = parse_text_json(result)
        remote_tool_names = {tool["name"] for tool in tools_payload["tools"]}
        assert "system.ping" in remote_tool_names, tools_payload
        assert "gateway.proxy_tool" in remote_tool_names, tools_payload

        result = call_tool(sock_a, 6, "server.list_servers", {"wait_ms": 100})
        cached_payload = parse_text_json(result)
        cached_peer = next(
            server for server in cached_payload["servers"] if server["address"] == f"127.0.0.1:{tcp_b}"
        )
        assert cached_peer["server_id"] == peer_server_id, cached_peer
        assert "tools_list" in cached_peer, cached_peer
        cached_names = {tool["name"] for tool in cached_peer["tools_list"]["tools"]}
        assert "system.ping" in cached_names, cached_peer

        result = call_tool(
            sock_a,
            7,
            "gateway.proxy_tool",
            {"server_id": peer_server_id, "tool_name": "system.ping", "args": {}},
        )
        assert result["isError"] is False, result
        assert result["content"][0]["text"] == "pong", result

        timed_out = call_tool(
            sock_a,
            71,
            "gateway.proxy_tool",
            {
                "server_id": peer_server_id,
                "tool_name": "system.shell_exec",
                "args": {"command": slow_python_command(0.3, "too-late"), "timeout_ms": 1500},
                "proxy_timeout_ms": 100,
            },
        )
        assert timed_out["isError"] is True, timed_out
        assert "gateway_proxy_timed_out" in timed_out["content"][0]["text"], timed_out

        proxy_command = slow_python_command(0.3, "proxy-ok")
        completed = call_tool(
            sock_a,
            72,
            "gateway.proxy_tool",
            {
                "server_id": peer_server_id,
                "tool_name": "system.shell_exec",
                "args": {"command": proxy_command, "timeout_ms": 1500},
                "proxy_timeout_ms": 2000,
            },
        )
        assert completed["isError"] is False, completed
        completed_payload = parse_text_json(completed)
        assert "command" not in completed_payload, completed_payload
        assert proxy_command not in (
            value for value in completed_payload.values() if isinstance(value, str)
        ), completed_payload
        assert completed_payload["stdout"].strip() == "proxy-ok", completed_payload
        assert completed_payload["timed_out"] is False, completed_payload

        request_graceful_shutdown(proc_b)
        proc_b.wait(timeout=5)
        proc_b = None

        offline_payload = None
        deadline = time.time() + 5
        while time.time() < deadline:
            result = call_tool(sock_a, 8, "server.list_servers", {"wait_ms": 100})
            offline_payload = parse_text_json(result)
            peer = next(
                (
                    server
                    for server in offline_payload["servers"]
                    if server["address"] == f"127.0.0.1:{tcp_b}"
                ),
                None,
            )
            if peer and peer["state"] == "offline":
                break
            time.sleep(0.1)

        assert offline_payload is not None, "missing offline response"
        peer = next(server for server in offline_payload["servers"] if server["address"] == f"127.0.0.1:{tcp_b}")
        assert peer["server_id"] == peer_server_id, peer
        assert peer["state"] == "offline", peer
        assert peer["tcp_connected"] is False, peer

        proc_b = start_signal_server(exe, tcp_b, discovery_b, discovery_a)
        sock_b = wait_for_tcp(tcp_b, proc_b)
        sock_b.settimeout(3.0)
        initialize(sock_b)

        restarted_payload = None
        deadline = time.time() + 5
        while time.time() < deadline:
            result = call_tool(sock_a, 9, "server.list_servers", {"wait_ms": 500})
            restarted_payload = parse_text_json(result)
            peer = next(
                (
                    server
                    for server in restarted_payload["servers"]
                    if server["address"] == f"127.0.0.1:{tcp_b}"
                ),
                None,
            )
            if peer and peer["state"] == "online" and peer["tcp_connected"] is True:
                break
            time.sleep(0.1)

        assert restarted_payload is not None, "missing restarted peer response"
        peer = next(server for server in restarted_payload["servers"] if server["address"] == f"127.0.0.1:{tcp_b}")
        assert peer["server_id"] == peer_server_id, peer
        assert peer["state"] == "online", peer
        assert peer["tcp_connected"] is True, peer
    finally:
        if sock_a:
            sock_a.close()
        if sock_b:
            sock_b.close()
        for proc in (proc_a, proc_b):
            if not proc:
                continue
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
