import json
import os
import socket
import ssl
import struct
import subprocess
import sys
import time

from tls_test_support import add_server_tls_env, client_context, connect_tls


def frame(payload):
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(data)) + data


def initialize_payload():
    return frame(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "tls-security-smoke", "version": "0.1"},
            },
        }
    )


def wait_for_valid_tls(port, proc):
    deadline = time.time() + 5
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early: {proc.stderr.read()}")
        try:
            return connect_tls(port)
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("TLS listener did not become ready")


def assert_rejected(port, context):
    raw = socket.create_connection(("127.0.0.1", port), timeout=1)
    sock = None
    try:
        try:
            sock = context.wrap_socket(raw, server_hostname="localhost")
            sock.settimeout(1)
            sock.sendall(initialize_payload())
            assert not sock.recv(4096), "unauthenticated client received an MCP response"
        except (OSError, ssl.SSLError):
            pass
    finally:
        if sock:
            sock.close()
        else:
            raw.close()


def main():
    exe = sys.argv[1]
    port = int(sys.argv[2])
    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_ENABLE_DISCOVERY"] = "0"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(port)
    add_server_tls_env(env, "node-a")
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )

    valid = None
    try:
        valid = wait_for_valid_tls(port, proc)
        valid.sendall(initialize_payload())
        valid.settimeout(2)
        response = valid.recv(4096)
        assert b'"serverInfo"' in response, response
        valid.close()
        valid = None

        raw = socket.create_connection(("127.0.0.1", port), timeout=1)
        raw.settimeout(1)
        raw.sendall(initialize_payload())
        try:
            response = raw.recv(4096)
        except (ConnectionResetError, socket.timeout):
            response = b""
        raw.close()
        assert b'"jsonrpc"' not in response, response

        assert_rejected(port, client_context(identity=None))
        assert_rejected(port, client_context(identity="untrusted-client"))
    finally:
        if valid:
            valid.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
