import json
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


def main():
    exe = sys.argv[1]
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
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
                    "clientInfo": {"name": "smoke", "version": "0.1"},
                },
            },
        )
        init = recv(proc)
        assert init["id"] == 1
        assert init["result"]["serverInfo"]["name"] == "mcp_server"

        send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

        send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        tools = recv(proc)
        names = {tool["name"] for tool in tools["result"]["tools"]}
        assert "system.ping" in names
        assert "gateway.status" in names
        assert "gateway.proxy_tool" in names
        assert "registry.list_tools" in names

        send(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "tools/call",
                "params": {"name": "system.ping", "arguments": {}},
            },
        )
        ping = recv(proc)
        assert ping["result"]["isError"] is False
        assert ping["result"]["content"][0]["text"] == "pong"

        send(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "tools/call",
                "params": {"name": "system.shell_exec", "arguments": {"command": "echo unsafe"}},
            },
        )
        shell = recv(proc)
        assert shell["result"]["isError"] is False
        shell_payload = json.loads(shell["result"]["content"][0]["text"])
        assert shell_payload["stdout"].strip() == "unsafe", shell_payload
        assert set(shell_payload) == {"stdout", "stderr", "exit_code"}, shell_payload

        send(
            proc,
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "tools/call",
                "params": {"name": "system.get_status", "arguments": {}},
            },
        )
        status_response = recv(proc)
        status_result = status_response["result"]
        assert status_result["isError"] is False, status_result
        status = json.loads(status_result["content"][0]["text"])
        assert status["hostname"], status
        assert "commands" in status and isinstance(status["commands"], dict), status
        assert "cwd" not in status, status
        assert "path" not in status, status
        assert "shell" not in status, status
        assert "network_interfaces" not in status, status
        if sys.platform != "win32":
            assert isinstance(status["uid"], int), status
            assert isinstance(status["gid"], int), status
            assert "sh" in status["commands"], status
    finally:
        proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
