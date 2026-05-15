import json
import os
import subprocess
import sys
import time
from pathlib import Path


def send(proc, payload):
    proc.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
    proc.stdin.flush()


def recv(proc):
    line = proc.stdout.readline()
    if not line:
        stderr = proc.stderr.read()
        raise RuntimeError(f"server closed stdout; stderr={stderr}")
    return json.loads(line)


def call_tool(proc, request_id, name, arguments):
    send(
        proc,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
    )
    response = recv(proc)
    assert response["id"] == request_id, response
    return response["result"]


def text_content(result):
    assert result["content"][0]["type"] == "text", result
    return result["content"][0]["text"]


def json_content(result):
    return json.loads(text_content(result))


def shell_command(command_windows, command_unix):
    return command_windows if os.name == "nt" else command_unix


def main():
    exe = sys.argv[1]
    env = os.environ.copy()
    env["MCP_ENABLE_SHELL_EXEC"] = "1"
    env["MCP_SHOULD_NOT_LEAK"] = "secret"
    env["MCP_SHELL_EXEC_CONFIG"] = os.path.join(
        os.path.dirname(__file__), "shell_exec_test_config.json"
    )
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
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
                    "clientInfo": {"name": "shell-policy-smoke", "version": "0.1"},
                },
            },
        )
        recv(proc)
        send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

        ok = call_tool(
            proc,
            2,
            "system.shell_exec",
            {"command": shell_command("echo smoke", "echo smoke")},
        )
        assert ok["isError"] is False, ok
        payload = json_content(ok)
        assert payload["stdout"].strip() == "smoke", payload
        assert payload["stderr"] == "", payload
        assert payload["exit_code"] == 0, payload
        assert payload["timed_out"] is False, payload
        assert payload["truncated"] is False, payload

        shell_syntax = call_tool(
            proc,
            3,
            "system.shell_exec",
            {"command": shell_command("echo smoke && echo ok", "echo smoke && echo ok")},
        )
        assert shell_syntax["isError"] is False, shell_syntax
        syntax_payload = json_content(shell_syntax)
        assert [line.strip() for line in syntax_payload["stdout"].splitlines()] == [
            "smoke",
            "ok",
        ], syntax_payload

        if os.name == "nt":
            dir_payload = json_content(
                call_tool(
                    proc,
                    31,
                    "system.shell_exec",
                    {"command": "dir"},
                )
            )
            assert dir_payload["stdout"] != "", dir_payload
            assert dir_payload["stderr"] == "", dir_payload
            assert dir_payload["exit_code"] == 0, dir_payload

        cwd = call_tool(
            proc,
            4,
            "system.shell_exec",
            {"command": shell_command("cd", "pwd")},
        )
        assert cwd["isError"] is False, cwd
        cwd_payload = json_content(cwd)
        expected_cwd = str(Path(".").resolve())
        assert cwd_payload["stdout"].strip().lower() == expected_cwd.lower(), cwd_payload

        env_clean = call_tool(
            proc,
            5,
            "system.shell_exec",
            {
                "command": shell_command(
                    'if defined MCP_SHOULD_NOT_LEAK (echo %MCP_SHOULD_NOT_LEAK%) else (echo unset)',
                    'printf \'%s\' "${MCP_SHOULD_NOT_LEAK-unset}"',
                )
            },
        )
        assert env_clean["isError"] is False, env_clean
        env_payload = json_content(env_clean)
        assert env_payload["stdout"].strip() == "unset", env_payload

        if os.name != "nt":
            marker = f"/tmp/mcp_shell_exec_marker_{os.getpid()}"
            timed_out = call_tool(
                proc,
                6,
                "system.shell_exec",
                {
                    "command": f"sh -c 'sleep 2; touch {marker}' & wait",
                    "timeout_ms": 50,
                },
            )
            assert timed_out["isError"] is True, timed_out
            timeout_payload = json_content(timed_out)
            assert timeout_payload["timed_out"] is True, timeout_payload
            assert timeout_payload["signal"] != 0, timeout_payload
            time.sleep(2.2)
            assert not os.path.exists(marker), marker
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
