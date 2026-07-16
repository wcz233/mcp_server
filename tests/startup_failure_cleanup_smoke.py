import os
import subprocess
import sys


def control_env(**updates):
    env = os.environ.copy()
    env.pop("MCP_ENABLE_SANDBOX_CTL", None)
    env.pop("MCP_SANDBOX_CTL_TOKEN", None)
    env.update(updates)
    return env


def assert_startup_failure(exe, env, expected_error, secret=None):
    proc = subprocess.run(
        [exe],
        input="",
        capture_output=True,
        env=env,
        text=True,
        encoding="utf-8",
        timeout=5,
    )

    assert proc.returncode == 1, (proc.returncode, proc.stdout, proc.stderr)
    assert expected_error in proc.stderr, proc.stderr
    assert "uv_loop_close" not in proc.stderr, proc.stderr
    if secret:
        assert secret not in proc.stdout, proc.stdout
        assert secret not in proc.stderr, proc.stderr


def main():
    exe = sys.argv[1]

    assert_startup_failure(
        exe,
        control_env(
            MCP_ENABLE_TCP="1",
            MCP_TCP_HOST="not-a-valid-host",
            MCP_TCP_PORT="18770",
        ),
        "mcp_server_start_tcp failed",
    )

    secret = "s1-token-must-not-appear-7f51b36e"
    assert_startup_failure(
        exe,
        control_env(MCP_ENABLE_SANDBOX_CTL="off", MCP_SANDBOX_CTL_TOKEN=secret),
        "MCP_ENABLE_SANDBOX_CTL must be unset, empty, 0, or 1",
        secret,
    )

    assert_startup_failure(
        exe,
        control_env(MCP_ENABLE_SANDBOX_CTL="1"),
        "MCP_SANDBOX_CTL_TOKEN must be non-empty when MCP_ENABLE_SANDBOX_CTL=1",
    )
    assert_startup_failure(
        exe,
        control_env(MCP_ENABLE_SANDBOX_CTL="1", MCP_SANDBOX_CTL_TOKEN=""),
        "MCP_SANDBOX_CTL_TOKEN must be non-empty when MCP_ENABLE_SANDBOX_CTL=1",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
