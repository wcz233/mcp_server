import os
import subprocess
import sys


def main():
    exe = sys.argv[1]
    env = os.environ.copy()
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "not-a-valid-host"
    env["MCP_TCP_PORT"] = "18770"

    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )

    if proc.stdin:
        proc.stdin.close()

    stdout, stderr = proc.communicate(timeout=5)
    assert proc.returncode == 1, (proc.returncode, stdout, stderr)
    assert "mcp_server_start_tcp failed" in stderr, stderr
    assert "uv_loop_close" not in stderr, stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
