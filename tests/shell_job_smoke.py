import json
import os
import signal
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


def call_tool(proc, request_id, name, arguments=None):
    send(
        proc,
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}},
        },
    )
    response = recv(proc)
    assert response["id"] == request_id, response
    result = response["result"]
    assert result["content"][0]["type"] == "text", result
    return result


def json_content(result):
    return json.loads(result["content"][0]["text"])


def initialize(proc):
    send(
        proc,
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "shell-job-smoke", "version": "0.1"},
            },
        },
    )
    recv(proc)
    send(proc, {"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})


def start_server(exe):
    env = os.environ.copy()
    env["MCP_ENABLE_SHELL_EXEC"] = "1"
    env["MCP_SHELL_EXEC_CONFIG"] = os.path.join(
        os.path.dirname(__file__), "shell_exec_test_config.json"
    )
    return subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )


def wait_for_state(proc, job_id, wanted, deadline_seconds=5.0):
    deadline = time.time() + deadline_seconds
    last = None
    while time.time() < deadline:
        result = call_tool(proc, 1000 + int(time.time() * 1000) % 100000, "system.shell_poll", {"job_id": job_id})
        assert result["isError"] is False, result
        last = json_content(result)
        if last["state"] == wanted:
            return last
        time.sleep(0.05)
    raise AssertionError(f"job did not reach {wanted}: {last}")


def main():
    exe = sys.argv[1]
    if os.name == "nt":
        return 0

    proc = start_server(exe)
    try:
        initialize(proc)

        tools = call_tool(proc, 2, "system.shell_list", {})
        assert tools["isError"] is False, tools
        assert "jobs" in json_content(tools), tools

        started = call_tool(
            proc,
            3,
            "system.shell_start",
            {"command": "sh -c 'printf start; sleep 0.2; printf done'", "timeout_ms": 2000},
        )
        assert started["isError"] is False, started
        start_payload = json_content(started)
        job_id = start_payload["job_id"]
        assert start_payload["state"] == "running", start_payload
        assert start_payload["pid"] > 0, start_payload
        assert start_payload["process_group_id"] == start_payload["pid"], start_payload
        assert start_payload["rollback"]["tool_name"] == "system.shell_kill", start_payload

        wait_payload = json_content(
            call_tool(proc, 4, "system.shell_wait", {"job_id": job_id, "timeout_ms": 3000})
        )
        assert wait_payload["wait_result"] == "finished", wait_payload
        assert wait_payload["state"] == "exited", wait_payload
        assert wait_payload["exit_code"] == 0, wait_payload

        tail_payload = json_content(call_tool(proc, 5, "system.shell_tail", {"job_id": job_id}))
        assert tail_payload["stdout"] == "startdone", tail_payload
        assert tail_payload["next_stdout_offset"] == len("startdone"), tail_payload

        marker = Path(f"/tmp/mcp_shell_job_marker_{os.getpid()}")
        try:
            marker.unlink()
        except FileNotFoundError:
            pass
        kill_started = json_content(
            call_tool(
                proc,
                6,
                "system.shell_start",
                {
                    "command": f"sh -c 'sleep 2; touch {marker}'",
                    "timeout_ms": 5000,
                    "label": "kill-check",
                },
            )
        )
        kill_job_id = kill_started["job_id"]
        killed = json_content(call_tool(proc, 7, "system.shell_kill", {"job_id": kill_job_id, "signal": signal.SIGTERM}))
        assert killed["state"] == "killed", killed
        killed = wait_for_state(proc, kill_job_id, "killed")
        assert killed["signal"] in (signal.SIGTERM, signal.SIGKILL), killed
        time.sleep(2.2)
        assert not marker.exists(), marker

        timed_started = json_content(
            call_tool(
                proc,
                8,
                "system.shell_start",
                {"command": "sh -c 'sleep 2'", "timeout_ms": 100},
            )
        )
        timed = wait_for_state(proc, timed_started["job_id"], "timed_out")
        assert timed["signal"] != 0, timed

        jobs = json_content(call_tool(proc, 9, "system.shell_list", {}))["jobs"]
        ids = {job["job_id"] for job in jobs}
        assert job_id in ids and kill_job_id in ids and timed_started["job_id"] in ids, jobs
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
