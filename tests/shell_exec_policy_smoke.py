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


def main():
    exe = sys.argv[1]
    env = os.environ.copy()
    env["MCP_ENABLE_SHELL_EXEC"] = "1"
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

        ok = call_tool(proc, 2, "system.shell_exec", {"command": "echo smoke"})
        assert ok["isError"] is False, ok
        assert "smoke" in text_content(ok), ok

        rejected = call_tool(proc, 3, "system.shell_exec", {"command": "echo smoke && echo bad"})
        assert rejected["isError"] is True, rejected
        assert "Unsupported shell metacharacters" in text_content(rejected), rejected
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
