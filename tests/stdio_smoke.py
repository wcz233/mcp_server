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
        assert shell["result"]["isError"] is True
        assert "disabled" in shell["result"]["content"][0]["text"]
    finally:
        proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
