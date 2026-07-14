import subprocess
import sys

from tools_dynamic_smoke import assert_json_text, call_tool, parse_text_content, recv, send


def main():
    exe = sys.argv[1]
    plugin_path = sys.argv[2]
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
                    "clientInfo": {"name": "busy-unload-smoke", "version": "0.1"},
                },
            },
        )
        init = recv(proc)
        assert init["id"] == 1, init
        send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})

        loaded = call_tool(proc, 2, "plugin_tools.insmod", {"package_path": plugin_path})
        assert loaded["isError"] is False, loaded
        plugin_id = assert_json_text(loaded)["plugin_id"]

        busy = call_tool(proc, 3, "plugin_tools.rmmod", {"plugin_id": plugin_id})
        assert busy["isError"] is True, busy
        assert "busy" in parse_text_content(busy).lower(), busy

        modules = assert_json_text(call_tool(proc, 4, "plugin_tools.lsmod", {}))["plugins"]
        retained = next(item for item in modules if item["plugin_id"] == plugin_id)
        assert retained["state"] == "ACTIVE", retained

        unloaded = call_tool(proc, 5, "plugin_tools.rmmod", {"plugin_id": plugin_id})
        assert unloaded["isError"] is False, unloaded
        assert assert_json_text(unloaded)["unloaded"] is True, unloaded

        modules = assert_json_text(call_tool(proc, 6, "plugin_tools.lsmod", {}))["plugins"]
        assert all(item["plugin_id"] != plugin_id for item in modules), modules
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
