import json
import os
import subprocess
import sys


def send(proc, payload):
    proc.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
    proc.stdin.flush()


def recv(proc):
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("server closed stdout")
    return json.loads(line)


def control_env(gate=None, token=None):
    env = os.environ.copy()
    env.pop("MCP_ENABLE_SANDBOX_CTL", None)
    env.pop("MCP_SANDBOX_CTL_TOKEN", None)
    if gate is not None:
        env["MCP_ENABLE_SANDBOX_CTL"] = gate
    if token is not None:
        env["MCP_SANDBOX_CTL_TOKEN"] = token
    return env


def assert_startup_failure(exe, gate, token, expected_error):
    proc = subprocess.run(
        [exe],
        input="",
        capture_output=True,
        env=control_env(gate, token),
        text=True,
        encoding="utf-8",
        timeout=5,
    )

    assert proc.returncode == 1, (proc.returncode, proc.stdout, proc.stderr)
    assert expected_error in proc.stderr, proc.stderr
    assert "uv_loop_close" not in proc.stderr, proc.stderr
    if token:
        assert token not in proc.stdout, proc.stdout
        assert token not in proc.stderr, proc.stderr


def assert_starts_without_registered_tool(exe, gate=None, token=None):
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=control_env(gate, token),
        text=True,
        encoding="utf-8",
    )

    try:
        send(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "sandbox-ctl-smoke", "version": "0.1"},
                },
            },
        )
        init = recv(proc)
        assert init["id"] == 1, init
        assert init["result"]["serverInfo"]["name"] == "mcp_server", init

        send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
        send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        tools = recv(proc)
        names = {tool["name"] for tool in tools["result"]["tools"]}
        assert "system.sandbox_ctl" not in names, names
    finally:
        if proc.stdin:
            proc.stdin.close()
            proc.stdin = None
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()
            raise AssertionError(("server did not exit", stdout, stderr))

    assert proc.returncode == 0, (proc.returncode, stdout, stderr)
    assert "uv_loop_close" not in stderr, stderr


def main():
    exe = sys.argv[1]
    assert_starts_without_registered_tool(exe)
    assert_starts_without_registered_tool(exe, gate="")
    assert_starts_without_registered_tool(exe, gate="0")
    assert_starts_without_registered_tool(exe, gate="1", token="x")

    secret = "s1-token-must-not-appear-7f51b36e"
    assert_startup_failure(
        exe,
        gate="off",
        token=secret,
        expected_error="MCP_ENABLE_SANDBOX_CTL must be unset, empty, 0, or 1",
    )
    assert_startup_failure(
        exe,
        gate="1",
        token=None,
        expected_error="MCP_SANDBOX_CTL_TOKEN must be non-empty when MCP_ENABLE_SANDBOX_CTL=1",
    )
    assert_startup_failure(
        exe,
        gate="1",
        token="",
        expected_error="MCP_SANDBOX_CTL_TOKEN must be non-empty when MCP_ENABLE_SANDBOX_CTL=1",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
