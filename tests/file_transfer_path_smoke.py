import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from file_transfer_d_smoke import (
    call,
    call_tool,
    initialize,
    json_text,
    load_plugin,
    start_server,
    wait_for_peer,
    wait_for_tcp,
)


def tool_names(sock, request_id):
    response = call(sock, request_id, "tools/list", {})
    return {tool["name"] for tool in response["result"]["tools"]}


def ensure_file_transfer_tools(sock, plugin_path, request_id):
    names = tool_names(sock, request_id)
    request_id += 1
    if "server.recv" in names:
        return request_id
    if plugin_path and plugin_path != "-":
        load_plugin(sock, plugin_path, request_id)
        request_id += 1
        names = tool_names(sock, request_id)
        request_id += 1
    assert "server.recv" in names, names
    return request_id


def assert_remote_path_error(label, result):
    assert result["isError"] is True, (label, result)
    text = result["content"][0]["text"]
    assert "Remote path" in text, (label, result)
    assert "File transfer setup failed" not in text, (label, result)


def assert_setup_error(label, result):
    assert result["isError"] is True, (label, result)
    assert "File transfer setup failed" in result["content"][0]["text"], (label, result)


def main():
    exe = sys.argv[1]
    plugin_path = sys.argv[2]
    tcp_a = int(sys.argv[3])
    tcp_b = int(sys.argv[4])
    discovery_a = int(sys.argv[5])
    discovery_b = int(sys.argv[6])

    tmp = tempfile.mkdtemp(prefix="mft-path-smoke-")
    proc_a = start_server(exe, tcp_a, discovery_a, discovery_b, tmp)
    proc_b = start_server(exe, tcp_b, discovery_b, discovery_a, tmp)
    sock_a = None
    sock_b = None

    try:
        sock_a = wait_for_tcp(tcp_a, proc_a)
        sock_b = wait_for_tcp(tcp_b, proc_b)
        initialize(sock_a, "mft-path-a")
        initialize(sock_b, "mft-path-b")

        next_a = ensure_file_transfer_tools(sock_a, plugin_path, 2)
        next_b = ensure_file_transfer_tools(sock_b, plugin_path, 2)
        peer_b, next_a = wait_for_peer(sock_a, tcp_b, next_a)
        peer_a, next_b = wait_for_peer(sock_b, tcp_a, next_b)
        assert peer_a > 0 and peer_b > 0

        pull = Path(tmp) / "pull"
        forward = call_tool(
            sock_a,
            next_a,
            "server.recv",
            {
                "server_id": peer_b,
                "remote_path": "D:/Project/2025-12-02/mcp/mcp.txt",
                "local_path": str(pull / "forward"),
                "timeout_ms": 2000,
            },
            timeout=5.0,
        )
        next_a += 1
        assert_remote_path_error("forward", forward)

        backslash = call_tool(
            sock_a,
            next_a,
            "server.recv",
            {
                "server_id": peer_b,
                "remote_path": "D:\\Project\\2025-12-02\\mcp\\mcp.txt",
                "local_path": str(pull / "backslash"),
                "timeout_ms": 2000,
            },
            timeout=5.0,
        )
        next_a += 1
        assert_remote_path_error("backslash", backslash)

        mixed = call_tool(
            sock_a,
            next_a,
            "server.recv",
            {
                "server_id": peer_b,
                "remote_path": "D:\\Project/2025-12-02\\mcp.txt",
                "local_path": str(pull / "mixed"),
                "timeout_ms": 2000,
            },
            timeout=5.0,
        )
        assert_setup_error("mixed", mixed)
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
