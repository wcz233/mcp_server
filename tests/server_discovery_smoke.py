import copy
import json
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import signal
import sys
import time


SERVER_ENTRY_KEYS = {
    "server_id",
    "address",
    "port",
    "scope",
    "state",
    "tcp_connected",
    "last_seen_ms",
    "system_status",
}
SYSTEM_STATUS_KEYS = {
    "hostname",
    "os",
    "machine",
    "memory_total_bytes",
    "memory_available_bytes",
    "commands",
}
REQUIRED_SYSTEM_STATUS_KEYS = {
    "hostname",
    "os",
    "memory_total_bytes",
    "memory_available_bytes",
    "commands",
}


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


def proxy_tools_list(sock, request_id, server_id):
    response = call(
        sock,
        request_id,
        "tools/call",
        {
            "name": "gateway.proxy_tool",
            "arguments": {"server_id": server_id, "tool_name": "tools_list", "args": {}},
        },
    )
    result = response["result"]
    if result.get("isError") is False and isinstance(result.get("content"), list):
        payload = parse_text_json(result)
    else:
        payload = result
    assert isinstance(payload.get("tools"), list), payload
    return result, payload


def assert_server_entry_contract(server):
    assert set(server) == SERVER_ENTRY_KEYS, server
    status = server["system_status"]
    assert set(status).issubset(SYSTEM_STATUS_KEYS), status
    assert REQUIRED_SYSTEM_STATUS_KEYS.issubset(status), status
    assert status["memory_total_bytes"] > 0, status
    assert 0 <= status["memory_available_bytes"] <= status["memory_total_bytes"], status
    assert isinstance(status["commands"], dict), status
    assert all(
        isinstance(name, str) and isinstance(path, str)
        for name, path in status["commands"].items()
    ), status


def assert_full_status_available(summary, full_status):
    for key in ("hostname", "os", "machine", "memory_total_bytes", "commands"):
        if key in summary:
            assert summary[key] == full_status[key], (summary, full_status)
    if full_status["os"] != "windows":
        assert {"kernel", "uid", "gid"}.issubset(full_status), full_status
        assert not {"kernel", "uid", "gid"}.intersection(summary), summary


def normalized_server_list_size(payload):
    normalized = copy.deepcopy(payload)
    for server in normalized["servers"]:
        server["last_seen_ms"] = 0
        server["system_status"]["memory_available_bytes"] = 0
    return len(json.dumps(normalized, separators=(",", ":"), ensure_ascii=True))


def assert_equivalent_scale_reduction(local, peer, tools_payload):
    compact_servers = [copy.deepcopy(local)]
    compact_servers.extend(copy.deepcopy(peer) for _ in range(4))
    compact_payload = {"total": 5, "servers": compact_servers}
    legacy_payload = copy.deepcopy(compact_payload)
    for server in legacy_payload["servers"][1:4]:
        server["tools_list"] = tools_payload

    compact_chars = len(json.dumps(compact_payload, separators=(",", ":"), ensure_ascii=True))
    legacy_chars = len(json.dumps(legacy_payload, separators=(",", ":"), ensure_ascii=True))
    assert compact_chars * 10 <= legacy_chars, (compact_chars, legacy_chars)


def file_transfer_plugin_path(exe):
    directory = Path(exe).resolve().parent
    for name in (
        "mcp_file_transfer_plugin.dll",
        "mcp_file_transfer_plugin.so",
        "mcp_file_transfer_plugin.dylib",
    ):
        candidate = directory / name
        if candidate.is_file():
            return str(candidate)
    return None


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
    plugin_path = file_transfer_plugin_path(exe)
    proc_a = start_server(exe, tcp_a, discovery_a, discovery_b)
    proc_b = None
    sock_a = None
    sock_b = None
    contract_failures = []

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
        assert local["port"] == tcp_a, local
        assert local["scope"] == "local", local
        assert local["state"] == "online", local
        assert local["tcp_connected"] is True, local
        assert_server_entry_contract(local)
        local_full_status = parse_text_json(call_tool(sock_a, 101, "system.get_status", {}))
        assert_full_status_available(local["system_status"], local_full_status)

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
        for server in payload["servers"]:
            assert_server_entry_contract(server)
        local = next(server for server in payload["servers"] if server["address"] == f"127.0.0.1:{tcp_a}")
        assert local["scope"] == "local", local
        assert local["server_id"] == 0, local
        peer = next(server for server in payload["servers"] if server["address"] == f"127.0.0.1:{tcp_b}")
        peer_server_id = peer["server_id"]
        assert peer_server_id > 0, peer
        assert peer["port"] == tcp_b, peer
        assert peer["scope"] == "remote", peer
        assert peer["state"] == "online", peer
        compact_size = normalized_server_list_size(payload)

        direct_tools_payload = call(sock_b, 2, "tools/list", {})["result"]
        management_payload = parse_text_json(call_tool(sock_b, 3, "registry.list_tools", {}))
        assert (
            management_payload["registryVersion"] == direct_tools_payload["registryVersion"]
        ), management_payload
        assert {tool["name"] for tool in management_payload["tools"]} == {
            tool["name"] for tool in direct_tools_payload["tools"]
        }, management_payload
        assert all(
            "enabled" in tool and "version" in tool for tool in management_payload["tools"]
        ), management_payload
        assert all(
            "enabled" not in tool and "version" not in tool
            for tool in direct_tools_payload["tools"]
        ), direct_tools_payload
        assert_equivalent_scale_reduction(local, peer, management_payload)

        result = call_tool(
            sock_a,
            5,
            "gateway.proxy_tool",
            {"server_id": peer_server_id, "tool_name": "system.ping", "args": {}},
        )
        if result["isError"] is not False or result["content"][0]["text"] != "pong":
            contract_failures.append(f"automatic proxy call failed: {result}")

        tools_result, tools_payload = proxy_tools_list(sock_a, 6, peer_server_id)
        if tools_result.get("isError") is not False or not isinstance(
            tools_result.get("content"), list
        ):
            contract_failures.append(
                "proxied tools/list is not a CallToolResult: "
                f"keys={sorted(tools_result)}"
            )
        assert tools_payload == direct_tools_payload, tools_payload
        remote_tool_names = {tool["name"] for tool in tools_payload["tools"]}
        assert "system.ping" in remote_tool_names, tools_payload
        assert "gateway.proxy_tool" in remote_tool_names, tools_payload

        result = call_tool(sock_a, 7, "server.list_servers", {"wait_ms": 100})
        cached_payload = parse_text_json(result)
        cached_peer = next(
            server for server in cached_payload["servers"] if server["address"] == f"127.0.0.1:{tcp_b}"
        )
        assert cached_peer["server_id"] == peer_server_id, cached_peer
        assert_server_entry_contract(cached_peer)
        assert normalized_server_list_size(cached_payload) == compact_size, cached_payload

        remote_full_status = parse_text_json(
            call_tool(
                sock_a,
                102,
                "gateway.proxy_tool",
                {"server_id": peer_server_id, "tool_name": "system.get_status", "args": {}},
            )
        )
        assert_full_status_available(cached_peer["system_status"], remote_full_status)

        missing = call_tool(
            sock_a,
            105,
            "gateway.proxy_tool",
            {
                "server_id": peer_server_id,
                "tool_name": "diagnostic.nonexistent",
                "args": {},
            },
        )
        assert missing["isError"] is True, missing
        missing_text = missing["content"][0]["text"].lower()
        if "not advertised" not in missing_text or "not cached" in missing_text:
            contract_failures.append(f"missing tool error is imprecise: {missing}")

        if plugin_path:
            loaded = parse_text_json(
                call_tool(sock_b, 4, "plugin_tools.insmod", {"package_path": plugin_path})
            )
            plugin_id = loaded["plugin_id"]
            assert {"server.send", "server.recv"}.issubset(loaded["tools"]), loaded

            direct_tools_payload = call(sock_b, 5, "tools/list", {})["result"]
            direct_names = {tool["name"] for tool in direct_tools_payload["tools"]}
            assert {"server.send", "server.recv"}.issubset(direct_names), direct_tools_payload

            _, tools_payload = proxy_tools_list(sock_a, 8, peer_server_id)
            assert tools_payload == direct_tools_payload, tools_payload
            plugin_payload = parse_text_json(
                call_tool(sock_a, 103, "server.list_servers", {"wait_ms": 100})
            )
            assert normalized_server_list_size(plugin_payload) == compact_size, plugin_payload
            proxied = call_tool(
                sock_a,
                9,
                "gateway.proxy_tool",
                {"server_id": peer_server_id, "tool_name": "server.send", "args": {}},
            )
            assert proxied["isError"] is True, proxied
            assert "session snapshot" not in proxied["content"][0]["text"], proxied

            unloaded = parse_text_json(
                call_tool(sock_b, 6, "plugin_tools.rmmod", {"plugin_id": plugin_id})
            )
            assert unloaded["unloaded"] is True, unloaded
            direct_tools_payload = call(sock_b, 7, "tools/list", {})["result"]
            assert "server.send" not in {
                tool["name"] for tool in direct_tools_payload["tools"]
            }, direct_tools_payload
            assert proxy_tools_list(sock_a, 10, peer_server_id)[1] == direct_tools_payload
            unloaded_payload = parse_text_json(
                call_tool(sock_a, 104, "server.list_servers", {"wait_ms": 100})
            )
            assert normalized_server_list_size(unloaded_payload) == compact_size, unloaded_payload

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
        assert_server_entry_contract(peer)
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
        assert_server_entry_contract(peer)
        assert peer["server_id"] == peer_server_id, peer
        assert peer["state"] == "online", peer
        assert peer["tcp_connected"] is True, peer
        assert not contract_failures, "\n".join(contract_failures)
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
