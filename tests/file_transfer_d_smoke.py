import hashlib
import json
import os
import shutil
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


def recv_frame(sock, timeout=10.0):
    sock.settimeout(timeout)
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


def call(sock, request_id, method, params=None, timeout=10.0):
    payload = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        payload["params"] = params
    sock.sendall(encode(payload))
    response = recv_frame(sock, timeout=timeout)
    assert response["id"] == request_id, response
    return response


def call_tool(sock, request_id, name, arguments=None, timeout=10.0):
    response = call(
        sock,
        request_id,
        "tools/call",
        {"name": name, "arguments": arguments or {}},
        timeout=timeout,
    )
    result = response["result"]
    assert result["content"][0]["type"] == "text", result
    return result


def json_text(result):
    assert result["isError"] is False, result
    return json.loads(result["content"][0]["text"])


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


def start_server(exe, tcp_port, discovery_port, peer_discovery_port, cwd):
    env = os.environ.copy()
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
    env["MCP_ENABLE_SHELL_EXEC"] = "0"
    return subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        cwd=cwd,
        text=True,
        encoding="utf-8",
    )


def initialize(sock, name):
    init = call(
        sock,
        1,
        "initialize",
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": name, "version": "0.1"},
        },
    )
    assert init["result"]["serverInfo"]["name"] == "mcp_server", init


def load_plugin(sock, plugin_path, request_id):
    result = call_tool(sock, request_id, "plugin_tools.insmod", {"package_path": plugin_path})
    payload = json_text(result)
    assert payload["tools"] and "server.send" in payload["tools"], payload
    return payload["plugin_id"]


def wait_for_peer(sock, tcp_port, start_id):
    request_id = start_id
    deadline = time.time() + 8
    last_payload = None
    while time.time() < deadline:
        payload = json_text(call_tool(sock, request_id, "server.list_servers", {"wait_ms": 500}))
        request_id += 1
        last_payload = payload
        for server in payload["servers"]:
            if server["address"] == f"127.0.0.1:{tcp_port}" and server["tcp_connected"]:
                return server["server_id"], request_id
        time.sleep(0.1)
    raise AssertionError(f"peer not connected: {last_payload}")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def assert_tree_equal(src, dst):
    for root, _, files in os.walk(src):
        rel_root = Path(root).relative_to(src)
        for name in files:
            src_file = Path(root) / name
            dst_file = Path(dst) / rel_root / name
            assert dst_file.exists(), dst_file
            assert sha256(src_file) == sha256(dst_file), (src_file, dst_file)


def main():
    exe = sys.argv[1]
    plugin_path = sys.argv[2]
    tcp_a = int(sys.argv[3])
    tcp_b = int(sys.argv[4])
    discovery_a = int(sys.argv[5])
    discovery_b = int(sys.argv[6])

    tmp = tempfile.mkdtemp(prefix="mft-d-smoke-")
    proc_a = start_server(exe, tcp_a, discovery_a, discovery_b, tmp)
    proc_b = start_server(exe, tcp_b, discovery_b, discovery_a, tmp)
    sock_a = None
    sock_b = None

    try:
        sock_a = wait_for_tcp(tcp_a, proc_a)
        sock_b = wait_for_tcp(tcp_b, proc_b)
        initialize(sock_a, "mft-a")
        initialize(sock_b, "mft-b")
        call(sock_a, 2, "tools/list", {})
        call(sock_b, 2, "tools/list", {})

        load_plugin(sock_a, plugin_path, 3)
        load_plugin(sock_b, plugin_path, 3)

        peer_b, _ = wait_for_peer(sock_a, tcp_b, 4)
        peer_a, _ = wait_for_peer(sock_b, tcp_a, 4)
        assert peer_a > 0 and peer_b > 0

        src = Path(tmp) / "src"
        dst = Path(tmp) / "dst"
        pull = Path(tmp) / "pull"
        file_src_dir = Path(tmp) / "file-src"
        remote_fetch_dir = Path(tmp) / "remote-fetch"
        src.mkdir()
        file_src_dir.mkdir()
        remote_fetch_dir.mkdir()
        (src / "sub").mkdir()
        (src / "sub" / "file1.txt").write_text("alpha\n", encoding="utf-8")
        (src / "sub" / "file2.bin").write_bytes(b"b" * 70000)
        (src / "root.txt").write_text("root\n", encoding="utf-8")
        (file_src_dir / "single.txt").write_text("single\n", encoding="utf-8")
        (remote_fetch_dir / "fetched.txt").write_text("fetched\n", encoding="utf-8")

        send_result = json_text(
            call_tool(
                sock_a,
                10,
                "server.send",
                {"server_id": peer_b, "local_path": str(src), "remote_path": str(dst)},
                timeout=15.0,
            )
        )
        assert send_result["files_transferred"] >= 3, send_result
        assert send_result["files_skipped"] == 0, send_result
        assert_tree_equal(src, dst / "src")

        send_skip = json_text(
            call_tool(
                sock_a,
                11,
                "server.send",
                {"server_id": peer_b, "local_path": str(src), "remote_path": str(dst)},
                timeout=15.0,
            )
        )
        assert send_skip["files_skipped"] >= 3, send_skip

        # Resume: create a partial file at the receiver and remove final file.
        target = dst / "src" / "sub" / "file2.bin"
        partial = Path(str(target) + ".part")
        target.unlink()
        partial.write_bytes((src / "sub" / "file2.bin").read_bytes()[:20000])
        resume_result = json_text(
            call_tool(
                sock_a,
                12,
                "server.send",
                {"server_id": peer_b, "local_path": str(src), "remote_path": str(dst)},
                timeout=15.0,
            )
        )
        assert resume_result["bytes_transferred"] < send_result["bytes_transferred"], resume_result
        assert_tree_equal(src, dst / "src")

        recv_result = json_text(
            call_tool(
                sock_a,
                13,
                "server.recv",
                {"server_id": peer_b, "remote_path": str(dst), "local_path": str(pull)},
                timeout=15.0,
            )
        )
        assert recv_result["files_transferred"] >= 3, recv_result
        assert_tree_equal(dst, pull / "dst")

        send_file_result = json_text(
            call_tool(
                sock_a,
                14,
                "server.send",
                {"server_id": peer_b, "local_path": str(file_src_dir / "single.txt"), "remote_path": "."},
                timeout=15.0,
            )
        )
        assert send_file_result["files_transferred"] == 1, send_file_result
        assert (Path(tmp) / "single.txt").read_text(encoding="utf-8") == "single\n"

        recv_file_result = json_text(
            call_tool(
                sock_a,
                15,
                "server.recv",
                {"server_id": peer_b, "remote_path": str(remote_fetch_dir / "fetched.txt"), "local_path": "."},
                timeout=15.0,
            )
        )
        assert recv_file_result["files_transferred"] == 1, recv_file_result
        assert (Path(tmp) / "fetched.txt").read_text(encoding="utf-8") == "fetched\n"

        missing_recv = call_tool(
            sock_a,
            16,
            "server.recv",
            {
                "server_id": peer_b,
                "remote_path": str(Path(tmp) / "does-not-exist"),
                "local_path": str(pull),
                "timeout_ms": 2000,
            },
            timeout=5.0,
        )
        assert missing_recv["isError"] is True, missing_recv
        assert "Remote path" in missing_recv["content"][0]["text"], missing_recv
    finally:
        for sock in (sock_a, sock_b):
            if sock:
                sock.close()
        for proc in (proc_a, proc_b):
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
