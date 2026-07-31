import json
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


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


def initialize(sock):
    response = call(
        sock,
        1,
        "initialize",
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "network-access-smoke", "version": "0.1"},
        },
    )
    assert response["result"]["serverInfo"]["name"] == "mcp_server", response
    sock.sendall(encode({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}}))


def write_policy(path, enabled, ips, peers):
    path.write_text(
        json.dumps(
            {
                "version": 1,
                "enabled": enabled,
                "allowlist": {"ips": ips},
                "discovery": {"peers": peers},
            }
        ),
        encoding="utf-8",
    )


def server_env(config_path, tcp_port, discovery_enabled):
    env = os.environ.copy()
    env.pop("MCP_DISCOVERY_HOSTS", None)
    env["MCP_NETWORK_ACCESS_CONFIG"] = str(config_path)
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(tcp_port)
    env["MCP_ENABLE_DISCOVERY"] = "1" if discovery_enabled else "0"
    env["MCP_STRICT_INIT"] = "0"
    env["MCP_ENABLE_SHELL_EXEC"] = "0"
    return env


def start_server(exe, env, cwd):
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    return subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        creationflags=creationflags,
    )


def stop_server(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def wait_for_tcp(port, proc):
    deadline = time.time() + 5
    last_error = None
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early: {proc.stderr.read()}")
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"tcp listener did not become ready: {last_error}")


def assert_startup_failure(exe, env, cwd, expected):
    proc = start_server(exe, env, cwd)
    try:
        _, stderr = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        stop_server(proc)
        raise AssertionError("invalid configuration did not stop startup")
    assert proc.returncode != 0, proc.returncode
    assert expected in stderr, stderr


def verify_rejected_tcp(exe, config_path, tcp_port, cwd):
    env = server_env(config_path, tcp_port, False)
    proc = start_server(exe, env, cwd)
    sock = None
    try:
        sock = wait_for_tcp(tcp_port, proc)
        sock.settimeout(2)
        try:
            sock.sendall(
                encode(
                    {
                        "jsonrpc": "2.0",
                        "id": 1,
                        "method": "initialize",
                        "params": {},
                    }
                )
            )
            assert sock.recv(1) == b"", "denied TCP source remained connected"
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            pass
        assert proc.poll() is None, proc.stderr.read()
    finally:
        if sock:
            sock.close()
        stop_server(proc)


def discovery_packet(advertise_host):
    return json.dumps(
        {
            "mcp_server_discovery": 1,
            "instance_id": "network-access-unauthorized-advertise",
            "tcp_port": 19999,
            "reply": True,
            "event": "online",
            "advertise_host": advertise_host,
            "status": {},
        },
        separators=(",", ":"),
    ).encode("utf-8")


def verify_allowed_and_unicast(
    exe,
    config_path,
    tcp_port,
    discovery_port,
    unicast_port,
    broadcast_port,
    cwd,
):
    unicast = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    broadcast = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    unicast.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    broadcast.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    unicast.bind(("127.0.0.1", unicast_port))
    broadcast.bind(("0.0.0.0", broadcast_port))
    env = server_env(config_path, tcp_port, True)
    env["MCP_DISCOVERY_BIND_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_PORT"] = str(discovery_port)
    env["MCP_UDP_BROADCAST_LISTEN_PORT"] = str(broadcast_port)
    env["MCP_DISCOVERY_ADVERTISE_HOST"] = "127.0.0.1"
    proc = start_server(exe, env, cwd)
    client = None
    try:
        client = wait_for_tcp(tcp_port, proc)
        client.settimeout(3)
        initialize(client)

        sender.sendto(discovery_packet("192.0.2.2"), ("127.0.0.1", discovery_port))
        listed = call(client, 2, "tools/call", {"name": "server.list_servers", "arguments": {}})
        text = listed["result"]["content"][0]["text"]
        assert "192.0.2.2" not in text, text

        unicast_packets = 0
        broadcast_packets = 0
        deadline = time.time() + 12
        while time.time() < deadline:
            ready, _, _ = select.select([unicast, broadcast], [], [], deadline - time.time())
            if not ready:
                break
            for ready_sock in ready:
                packet, _ = ready_sock.recvfrom(65536)
                try:
                    payload = json.loads(packet.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if payload.get("mcp_server_discovery") != 1 or payload.get("tcp_port") != tcp_port:
                    continue
                if ready_sock is unicast:
                    unicast_packets += 1
                else:
                    broadcast_packets += 1
        assert unicast_packets >= 2, unicast_packets
        assert broadcast_packets == 0, broadcast_packets
    finally:
        if client:
            client.close()
        stop_server(proc)
        sender.close()
        broadcast.close()
        unicast.close()


def main():
    exe = str(Path(sys.argv[1]).resolve())
    tcp_port = int(sys.argv[2])
    denied_tcp_port = int(sys.argv[3])
    discovery_port = int(sys.argv[4])
    unicast_port = int(sys.argv[5])
    broadcast_port = int(sys.argv[6])

    with tempfile.TemporaryDirectory(prefix="mcp-network-access-") as tmp_name:
        tmp = Path(tmp_name)
        invalid = tmp / "invalid.json"
        conflict = tmp / "conflict.json"
        denied = tmp / "denied.json"
        allowed = tmp / "allowed.json"

        write_policy(invalid, True, [], [])
        invalid_env = server_env(invalid, tcp_port, False)
        assert_startup_failure(exe, invalid_env, tmp, "allowlist.ips must not be empty")

        write_policy(conflict, True, ["127.0.0.1"], [])
        conflict_env = server_env(conflict, tcp_port, False)
        conflict_env["MCP_DISCOVERY_HOSTS"] = "127.0.0.1"
        assert_startup_failure(exe, conflict_env, tmp, "MCP_DISCOVERY_HOSTS cannot be used")

        write_policy(denied, True, ["192.0.2.1"], [])
        verify_rejected_tcp(exe, denied, denied_tcp_port, tmp)

        write_policy(
            allowed,
            True,
            ["127.0.0.1"],
            [{"ip": "127.0.0.1", "discovery_port": unicast_port}],
        )
        verify_allowed_and_unicast(
            exe,
            allowed,
            tcp_port,
            discovery_port,
            unicast_port,
            broadcast_port,
            tmp,
        )


if __name__ == "__main__":
    main()
