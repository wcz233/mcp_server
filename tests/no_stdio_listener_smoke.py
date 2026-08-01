import os
import socket
import subprocess
import sys
import time

from tls_test_support import add_server_tls_env


def main():
    exe = sys.argv[1]
    port = int(sys.argv[2])
    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(port)
    add_server_tls_env(env)
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )

    try:
        deadline = time.time() + 2
        connected = False
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"server exited early; stderr={proc.stderr.read()}")
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                sock.close()
                connected = True
                break
            except OSError:
                time.sleep(0.05)
        assert connected, "tcp listener did not become ready"
        assert proc.poll() is None, "server should remain alive without stdio"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
