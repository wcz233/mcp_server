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


def list_tools(proc, request_id):
    send(proc, {"jsonrpc": "2.0", "id": request_id, "method": "tools/list", "params": {}})
    response = recv(proc)
    assert response["id"] == request_id, response
    return response["result"]["tools"]


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

        tool_schemas = {tool["name"]: tool["inputSchema"] for tool in list_tools(proc, 2)}
        start_properties = tool_schemas["system.shell_start"]["properties"]
        assert "command" in start_properties, start_properties
        assert "label" in start_properties, start_properties
        assert "env" in start_properties, start_properties
        assert "args" not in start_properties, start_properties
        assert start_properties["timeout_ms"]["minimum"] == 1, start_properties
        assert start_properties["timeout_ms"]["maximum"] == 300000, start_properties
        assert start_properties["output_limit_bytes"]["minimum"] == 256, start_properties
        assert start_properties["output_limit_bytes"]["maximum"] == 2147483648, start_properties
        assert start_properties["env"]["additionalProperties"]["type"] == "string", start_properties
        tail_properties = tool_schemas["system.shell_tail"]["properties"]
        assert "offset" not in tail_properties, tail_properties
        assert "stdout_offset" in tail_properties, tail_properties
        assert "stderr_offset" in tail_properties, tail_properties

        tools = call_tool(proc, 3, "system.shell_list", {})
        assert tools["isError"] is False, tools
        assert "jobs" in json_content(tools), tools

        started = call_tool(
            proc,
            4,
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
        assert start_payload["sandbox_revision"] == 0, start_payload
        assert start_payload["sandbox_enabled"] is True, start_payload
        assert start_payload["shell_enabled"] is True, start_payload

        wait_payload = json_content(
            call_tool(proc, 5, "system.shell_wait", {"job_id": job_id, "timeout_ms": 3000})
        )
        assert wait_payload["wait_result"] == "finished", wait_payload
        assert wait_payload["state"] == "exited", wait_payload
        assert wait_payload["exit_code"] == 0, wait_payload
        assert wait_payload["sandbox_revision"] == start_payload["sandbox_revision"], wait_payload

        tail_payload = json_content(call_tool(proc, 6, "system.shell_tail", {"job_id": job_id}))
        assert tail_payload["stdout"] == "startdone", tail_payload
        assert tail_payload["next_stdout_offset"] == len("startdone"), tail_payload
        assert tail_payload["sandbox_revision"] == start_payload["sandbox_revision"], tail_payload

        env_started = json_content(
            call_tool(
                proc,
                7,
                "system.shell_start",
                {
                    "command": "printf '%s:%s' \"$MCP_TEST_ENV\" \"$LANG\"",
                    "env": {"MCP_TEST_ENV": "from-env"},
                    "label": "env-check",
                },
            )
        )
        assert env_started["label"] == "env-check", env_started
        env_job_id = env_started["job_id"]
        env_wait = json_content(call_tool(proc, 8, "system.shell_wait", {"job_id": env_job_id, "timeout_ms": 3000}))
        assert env_wait["state"] == "exited", env_wait
        env_tail = json_content(call_tool(proc, 9, "system.shell_tail", {"job_id": env_job_id}))
        assert env_tail["stdout"] == "from-env:C", env_tail

        truncated_started = json_content(
            call_tool(
                proc,
                10,
                "system.shell_start",
                {"command": "printf abcdef", "output_limit_bytes": 256},
            )
        )
        truncated_job_id = truncated_started["job_id"]
        truncated_wait = json_content(
            call_tool(proc, 11, "system.shell_wait", {"job_id": truncated_job_id, "timeout_ms": 3000})
        )
        assert truncated_wait["state"] == "exited", truncated_wait
        truncated_tail = json_content(call_tool(proc, 12, "system.shell_tail", {"job_id": truncated_job_id}))
        assert truncated_tail["stdout"] == "abcdef", truncated_tail
        assert truncated_tail["stdout_truncated"] is False, truncated_tail

        bad_timeout = call_tool(proc, 13, "system.shell_start", {"command": "true", "timeout_ms": 5001})
        assert bad_timeout["isError"] is True, bad_timeout
        assert "timeout_ms" in bad_timeout["content"][0]["text"], bad_timeout
        bad_output_limit = call_tool(proc, 14, "system.shell_start", {"command": "true", "output_limit_bytes": 255})
        assert bad_output_limit["isError"] is True, bad_output_limit
        assert "output_limit_bytes" in bad_output_limit["content"][0]["text"], bad_output_limit
        bad_args = call_tool(proc, 15, "system.shell_start", {"command": "true", "args": {}})
        assert bad_args["isError"] is True, bad_args
        assert "args" in bad_args["content"][0]["text"], bad_args
        bad_env = call_tool(proc, 16, "system.shell_start", {"command": "true", "env": {"BAD=NAME": "x"}})
        assert bad_env["isError"] is True, bad_env
        assert "env names" in bad_env["content"][0]["text"], bad_env
        bad_offset = call_tool(proc, 17, "system.shell_tail", {"job_id": job_id, "offset": 0})
        assert bad_offset["isError"] is True, bad_offset
        assert "offset" in bad_offset["content"][0]["text"], bad_offset

        marker = Path(f"/tmp/mcp_shell_job_marker_{os.getpid()}")
        try:
            marker.unlink()
        except FileNotFoundError:
            pass
        kill_started = json_content(
            call_tool(
                proc,
                18,
                "system.shell_start",
                {
                    "command": f"sh -c 'sleep 2; touch {marker}'",
                    "timeout_ms": 5000,
                    "label": "kill-check",
                },
            )
        )
        kill_job_id = kill_started["job_id"]
        killed = json_content(call_tool(proc, 19, "system.shell_kill", {"job_id": kill_job_id, "signal": signal.SIGTERM}))
        assert killed["state"] == "killed", killed
        assert killed["signal"] in (signal.SIGTERM, signal.SIGKILL), killed
        time.sleep(2.2)
        assert not marker.exists(), marker

        timed_started = json_content(
            call_tool(
                proc,
                20,
                "system.shell_start",
                {"command": "sh -c 'sleep 2'", "timeout_ms": 100},
            )
        )
        timed = wait_for_state(proc, timed_started["job_id"], "timed_out")
        assert timed["signal"] != 0, timed

        jobs = json_content(call_tool(proc, 21, "system.shell_list", {}))["jobs"]
        ids = {job["job_id"] for job in jobs}
        assert job_id in ids and kill_job_id in ids and timed_started["job_id"] in ids, jobs
        assert env_job_id in ids and truncated_job_id in ids, jobs
        listed = next(job for job in jobs if job["job_id"] == job_id)
        assert listed["sandbox_revision"] == start_payload["sandbox_revision"], listed
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
