import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from tls_test_support import add_server_tls_env, connect_tls
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
            return connect_tls(port, timeout=0.5)
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
    add_server_tls_env(env, "node-a" if tcp_port % 2 == 0 else "node-b")
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


def stop_server(proc):
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


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


def send_discovery_event(discovery_port, instance_id, tcp_port, event):
    packet = json.dumps(
        {
            "mcp_server_discovery": 1,
            "instance_id": instance_id,
            "tcp_port": tcp_port,
            "reply": True,
            "event": event,
            "advertise_host": "127.0.0.1",
            "status": {"hostname": instance_id, "os": "test"},
        },
        separators=(",", ":"),
    ).encode("utf-8")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.sendto(packet, ("127.0.0.1", discovery_port))


def close_plain_client_sessions(proc, tcp_port, name):
    for index in range(3):
        client = wait_for_tcp(tcp_port, proc)
        try:
            initialize(client, f"{name}-{index}")
        finally:
            client.close()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def assert_directory_integrity(result, files_verified):
    assert result["integrity"] == {
        "algorithm": "sha256",
        "status": "verified",
        "files_verified": files_verified,
    }, result


def assert_file_integrity(result, path):
    integrity = result["integrity"]
    assert integrity == {
        "algorithm": "sha256",
        "status": "verified",
        "files_verified": 1,
        "digest": sha256(path),
    }, result


def assert_tree_equal(src, dst):
    for root, _, files in os.walk(src):
        rel_root = Path(root).relative_to(src)
        for name in files:
            src_file = Path(root) / name
            dst_file = Path(dst) / rel_root / name
            assert dst_file.exists(), dst_file
            assert sha256(src_file) == sha256(dst_file), (src_file, dst_file)


def write_repeated_file(path, size, value):
    chunk = bytes([value]) * (1024 * 1024)
    remaining = size
    with open(path, "wb") as f:
        while remaining:
            write_size = min(remaining, len(chunk))
            f.write(chunk[:write_size])
            remaining -= write_size


def copy_prefix(source, target, size):
    remaining = size
    with open(source, "rb") as src, open(target, "wb") as dst:
        while remaining:
            chunk = src.read(min(remaining, 1024 * 1024))
            if not chunk:
                raise RuntimeError("source ended before resume prefix")
            dst.write(chunk)
            remaining -= len(chunk)


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
        proc_a_pid = proc_a.pid
        proc_b_pid = proc_b.pid
        sock_a = wait_for_tcp(tcp_a, proc_a)
        sock_b = wait_for_tcp(tcp_b, proc_b)
        initialize(sock_a, "mft-a")
        initialize(sock_b, "mft-b")
        call(sock_a, 2, "tools/list", {})
        call(sock_b, 2, "tools/list", {})

        load_plugin(sock_a, plugin_path, 3)
        load_plugin(sock_b, plugin_path, 3)

        tools = call(sock_a, 4, "tools/list", {})["result"]["tools"]
        descriptions = {tool["name"]: tool["description"] for tool in tools}
        for name in ("server.send", "server.recv"):
            assert "whole-file SHA-256 verification" in descriptions[name], descriptions[name]

        peer_b, _ = wait_for_peer(sock_a, tcp_b, 5)
        peer_a, _ = wait_for_peer(sock_b, tcp_a, 5)
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
        fetched_bytes = b"fetched\nbinary\x00tail\n"
        (remote_fetch_dir / "fetched.txt").write_bytes(fetched_bytes)

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
        assert_directory_integrity(send_result, 3)
        assert_tree_equal(src, dst / "src")

        close_plain_client_sessions(proc_a, tcp_a, "adapter-a")
        close_plain_client_sessions(proc_b, tcp_b, "adapter-b")
        time.sleep(0.2)

        for _ in range(3):
            send_discovery_event(discovery_a, "mft-b-control-reconnect", tcp_b, "offline")
            time.sleep(0.05)
        time.sleep(0.5)
        send_discovery_event(discovery_a, "mft-b-control-reconnect", tcp_b, "online")
        peer_b, _ = wait_for_peer(sock_a, tcp_b, 40)
        assert proc_a.pid == proc_a_pid and proc_a.poll() is None
        assert proc_b.pid == proc_b_pid and proc_b.poll() is None

        reconnect_src = file_src_dir / "reconnect-10.bin"
        reconnect_target = Path(tmp) / "reconnect-target.bin"
        reconnect_pull = Path(tmp) / "reconnect-pull"
        reconnect_src.write_bytes(bytes(range(10)))
        reconnect_pull.mkdir()
        reconnect_send = json_text(
            call_tool(
                sock_a,
                40,
                "server.send",
                {
                    "server_id": peer_b,
                    "local_path": str(reconnect_src),
                    "remote_path": str(reconnect_target),
                    "timeout_ms": 10000,
                },
                timeout=15.0,
            )
        )
        assert reconnect_send["files_transferred"] == 1, reconnect_send
        assert_file_integrity(reconnect_send, reconnect_src)
        assert reconnect_target.stat().st_size == reconnect_src.stat().st_size
        assert sha256(reconnect_target) == sha256(reconnect_src)
        assert not Path(str(reconnect_target) + ".part").exists()

        reconnect_recv = json_text(
            call_tool(
                sock_a,
                41,
                "server.recv",
                {
                    "server_id": peer_b,
                    "remote_path": str(reconnect_target),
                    "local_path": str(reconnect_pull),
                    "timeout_ms": 10000,
                },
                timeout=15.0,
            )
        )
        reconnect_pulled = reconnect_pull / reconnect_target.name
        assert reconnect_recv["files_transferred"] == 1, reconnect_recv
        assert_file_integrity(reconnect_recv, reconnect_target)
        assert reconnect_pulled.stat().st_size == reconnect_target.stat().st_size
        assert sha256(reconnect_pulled) == sha256(reconnect_target)
        assert not Path(str(reconnect_pulled) + ".part").exists()

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
        assert_directory_integrity(send_skip, 3)

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
        assert_directory_integrity(resume_result, 3)
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
        assert_directory_integrity(recv_result, 3)
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
        assert_file_integrity(send_file_result, file_src_dir / "single.txt")
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
        assert_file_integrity(recv_file_result, remote_fetch_dir / "fetched.txt")
        assert (Path(tmp) / "fetched.txt").read_bytes() == fetched_bytes

        empty_src = file_src_dir / "empty.bin"
        empty_target = Path(tmp) / "empty-target.bin"
        empty_src.write_bytes(b"")
        empty_result = json_text(
            call_tool(
                sock_a,
                21,
                "server.send",
                {
                    "server_id": peer_b,
                    "local_path": str(empty_src),
                    "remote_path": str(empty_target),
                },
                timeout=15.0,
            )
        )
        assert empty_result["files_transferred"] == 1, empty_result
        assert_file_integrity(empty_result, empty_src)
        assert empty_target.read_bytes() == b""

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
        assert "sha256_mismatch" not in missing_recv["content"][0]["text"], missing_recv

        window_size = 16 * 1024 * 1024
        window_src = file_src_dir / "window-src.bin"
        window_target = Path(tmp) / "window-target.bin"
        window_pull = Path(tmp) / "window-pull"
        window_pull.mkdir()
        write_repeated_file(window_src, window_size, ord("w"))
        window_send = json_text(
            call_tool(
                sock_a,
                19,
                "server.send",
                {
                    "server_id": peer_b,
                    "local_path": str(window_src),
                    "remote_path": str(window_target),
                    "timeout_ms": 60000,
                },
                timeout=60.0,
            )
        )
        assert window_send["bytes_transferred"] == window_size, window_send
        assert sha256(window_src) == sha256(window_target)
        window_recv = json_text(
            call_tool(
                sock_a,
                20,
                "server.recv",
                {
                    "server_id": peer_b,
                    "remote_path": str(window_target),
                    "local_path": str(window_pull),
                    "timeout_ms": 60000,
                },
                timeout=60.0,
            )
        )
        window_pulled = window_pull / window_target.name
        assert window_recv["bytes_transferred"] == window_size, window_recv
        assert sha256(window_target) == sha256(window_pulled)

        logical_block_size = 64 * 1024 * 1024
        resume_tail_size = 1024 * 1024
        resume_size = logical_block_size + resume_tail_size
        resume_src = file_src_dir / "resume-large.bin"
        resume_target = Path(tmp) / "resume-large-target.bin"
        resume_part = Path(str(resume_target) + ".part")
        write_repeated_file(resume_src, resume_size, ord("r"))
        copy_prefix(resume_src, resume_part, logical_block_size)
        with resume_part.open("ab") as f:
            f.write(b"invalid partial block")
        resume_large = json_text(
            call_tool(
                sock_a,
                17,
                "server.send",
                {
                    "server_id": peer_b,
                    "local_path": str(resume_src),
                    "remote_path": str(resume_target),
                    "timeout_ms": 120000,
                },
                timeout=120.0,
            )
        )
        assert resume_large["files_transferred"] == 1, resume_large
        assert resume_large["bytes_total"] == resume_size, resume_large
        assert resume_large["bytes_transferred"] == resume_tail_size, resume_large
        assert_file_integrity(resume_large, resume_src)
        assert sha256(resume_src) == sha256(resume_target)
        assert not resume_part.exists(), resume_part

        corrupt_src = file_src_dir / "corrupt.bin"
        corrupt_target = Path(tmp) / "corrupt-target.bin"
        corrupt_part = Path(str(corrupt_target) + ".part")
        corrupt_src.write_bytes(b"c" * (8 * 1024 * 1024))
        transfer = {}

        def send_corruptible_file():
            try:
                transfer["result"] = call_tool(
                    sock_a,
                    18,
                    "server.send",
                    {
                        "server_id": peer_b,
                        "local_path": str(corrupt_src),
                        "remote_path": str(corrupt_target),
                    },
                    timeout=30.0,
                )
            except Exception as exc:
                transfer["error"] = exc

        worker = threading.Thread(target=send_corruptible_file)
        worker.start()
        deadline = time.time() + 10
        corrupted = False
        while time.time() < deadline:
            try:
                if corrupt_part.stat().st_size > 0:
                    with corrupt_part.open("r+b", buffering=0) as f:
                        f.seek(0)
                        f.write(b"x")
                    corrupted = True
                    break
            except FileNotFoundError:
                pass
            if not worker.is_alive():
                break
            time.sleep(0.005)

        worker.join(timeout=35)
        assert not worker.is_alive(), "corruption transfer did not finish"
        if "error" in transfer:
            raise transfer["error"]
        assert corrupted, "partial file was not corrupted during transfer"
        corrupt_result = transfer["result"]
        assert corrupt_result["isError"] is True, corrupt_result
        assert not corrupt_target.exists(), corrupt_target
        assert corrupt_part.exists(), corrupt_part
        error_text = corrupt_result["content"][0]["text"]
        assert len(error_text.encode("utf-8")) <= 1024, corrupt_result
        assert json.loads(error_text) == {
            "code": "sha256_mismatch",
            "message": "Received file failed whole-file SHA-256 verification.",
            "path": ".",
            "expected_sha256": sha256(corrupt_src),
            "actual_sha256": sha256(corrupt_part),
        }, corrupt_result
    finally:
        for sock in (sock_a, sock_b):
            if sock:
                sock.close()
        for proc in (proc_a, proc_b):
            stop_server(proc)
        shutil.rmtree(tmp, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
