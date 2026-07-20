import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


TOKEN = "s4-token-must-not-appear-2f841bd7"
SNAPSHOT_FIELDS = (
    "sandbox_revision",
    "sandbox_enabled",
    "shell_enabled",
    "timeout_ms",
    "output_bytes",
    "once_read_stdout_err_chunk_size",
)


def valid_config(working_directory):
    return {
        "version": 2,
        "control": {"token": TOKEN},
        "defaults": {
            "shell_enabled": True,
            "command_length": 65536,
            "timeout_ms": 1000,
            "output_bytes": 512,
            "once_read_stdout_err_chunk_size": 64,
            "capture_stderr": True,
            "merge_stderr_to_stdout": False,
            "execution": {
                "mode": "shell",
                "shell_path": "/bin/sh",
                "shell_arg": "-c",
                "working_directory": str(working_directory),
                "inherit_env": False,
                "request_cwd_allowed": True,
                "request_env_allowed": True,
                "kill_process_group_on_timeout": True,
                "run_as_user": "",
                "run_as_group": "",
                "env": {"HOME": str(working_directory), "LANG": "C", "PATH": "/usr/bin:/bin"},
            },
            "limits": {
                "cpu_seconds": 3600,
                "memory_bytes": 2147483648,
                "file_size_bytes": 2147483648,
                "open_files": 64,
                "processes": 16,
            },
            "isolation": {"require_non_root": False},
        },
        "bounds": {
            "command_length": {"min": 1, "max": 65536},
            "timeout_ms": {"min": 1, "max": 3600000},
            "output_bytes": {"min": 0, "max": 1048576},
            "once_read_stdout_err_chunk_size": {"min": 64, "max": 65536},
            "execution": {
                "mode": {"allowed": ["shell", "exec"]},
                "shell_path": {"min_bytes": 1, "max_bytes": 4096},
                "shell_arg": {"min_bytes": 0, "max_bytes": 65536},
                "working_directory": {"min_bytes": 1, "max_bytes": 4096},
                "allowed_env": {"max_items": 1024, "item_max_bytes": 255},
                "run_as_user": {"min_bytes": 0},
                "run_as_group": {"min_bytes": 0},
            },
            "limits": {
                "cpu_seconds": {"min": 0, "max": 3600},
                "memory_bytes": {"min": 0, "max": 34359738367},
                "file_size_bytes": {"min": 0, "max": 17179869184},
                "open_files": {"min": 0, "max": 65536},
                "processes": {"min": 0, "max": 2048},
            },
        },
    }


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


def start_server(exe, config_path):
    env = {
        "HOME": str(config_path.parent),
        "LANG": "C",
        "PATH": "/usr/bin:/bin",
        "MCP_ENABLE_SANDBOX_CTL": "1",
        "MCP_SHELL_EXEC_CONFIG": str(config_path),
    }
    return subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )


def assert_snapshot(payload, expected):
    assert {field: payload[field] for field in SNAPSHOT_FIELDS} == expected, payload


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

    temp_dir = tempfile.TemporaryDirectory(prefix="mcp-shell-job-")
    temp_path = Path(temp_dir.name)
    config_path = temp_path / "shell_exec.json"
    config_path.write_text(json.dumps(valid_config(temp_path), separators=(",", ":")), encoding="utf-8")
    async_cwd = temp_path / "async-cwd"
    async_cwd.mkdir()
    proc = start_server(exe, config_path)
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

        state = json_content(
            call_tool(proc, 4, "system.sandbox_ctl", {"action": "get", "token": TOKEN})
        )
        assert state["revision"] == 0, state

        old_started = json_content(
            call_tool(
                proc,
                101,
                "system.shell_start",
                {
                    "command": (
                        "sleep 0.6; i=0; "
                        'while [ "$i" -lt 300 ]; do printf o; i=$((i + 1)); done'
                    ),
                    "label": "old-policy-snapshot",
                },
            )
        )
        old_job_id = old_started["job_id"]
        old_snapshot = {
            "sandbox_revision": state["revision"],
            "sandbox_enabled": True,
            "shell_enabled": True,
            "timeout_ms": 1000,
            "output_bytes": 512,
            "once_read_stdout_err_chunk_size": 64,
        }
        assert_snapshot(old_started, old_snapshot)

        state = json_content(
            call_tool(
                proc,
                102,
                "system.sandbox_ctl",
                {
                    "action": "update",
                    "token": TOKEN,
                    "expected_revision": state["revision"],
                    "overrides": {"timeout_ms": 100, "output_bytes": 256},
                },
            )
        )
        new_snapshot = {
            "sandbox_revision": state["revision"],
            "sandbox_enabled": True,
            "shell_enabled": True,
            "timeout_ms": 100,
            "output_bytes": 256,
            "once_read_stdout_err_chunk_size": 64,
        }

        old_poll = json_content(
            call_tool(proc, 103, "system.shell_poll", {"job_id": old_job_id})
        )
        assert old_poll["state"] == "running", old_poll
        assert_snapshot(old_poll, old_snapshot)
        old_tail = json_content(
            call_tool(proc, 104, "system.shell_tail", {"job_id": old_job_id})
        )
        assert_snapshot(old_tail, old_snapshot)
        jobs = json_content(call_tool(proc, 105, "system.shell_list", {}))["jobs"]
        assert_snapshot(next(job for job in jobs if job["job_id"] == old_job_id), old_snapshot)

        new_started = json_content(
            call_tool(
                proc,
                106,
                "system.shell_start",
                {
                    "command": (
                        'i=0; while [ "$i" -lt 300 ]; do printf n; i=$((i + 1)); done; '
                        "sleep 1"
                    ),
                    "label": "new-policy-snapshot",
                },
            )
        )
        new_job_id = new_started["job_id"]
        assert_snapshot(new_started, new_snapshot)
        new_polled = wait_for_state(proc, new_job_id, "timed_out")
        assert_snapshot(new_polled, new_snapshot)
        new_wait = json_content(
            call_tool(proc, 107, "system.shell_wait", {"job_id": new_job_id, "timeout_ms": 3000})
        )
        assert new_wait["wait_result"] == "finished", new_wait
        assert_snapshot(new_wait, new_snapshot)
        new_tail = json_content(
            call_tool(proc, 108, "system.shell_tail", {"job_id": new_job_id})
        )
        assert new_tail["stdout"] == "n" * 256, new_tail
        assert new_tail["stdout_truncated"] is True, new_tail
        assert_snapshot(new_tail, new_snapshot)

        state = json_content(
            call_tool(
                proc,
                109,
                "system.sandbox_ctl",
                {
                    "action": "update",
                    "token": TOKEN,
                    "expected_revision": state["revision"],
                    "overrides": {"shell_enabled": False},
                },
            )
        )
        blocked_marker = temp_path / "disabled-start-marker"
        blocked = call_tool(
            proc,
            110,
            "system.shell_start",
            {"command": f"touch {blocked_marker}"},
        )
        assert blocked["isError"] is True, blocked
        assert "disabled" in blocked["content"][0]["text"], blocked
        time.sleep(0.1)
        assert not blocked_marker.exists(), blocked_marker

        old_poll = json_content(
            call_tool(proc, 111, "system.shell_poll", {"job_id": old_job_id})
        )
        assert_snapshot(old_poll, old_snapshot)
        old_wait = json_content(
            call_tool(proc, 112, "system.shell_wait", {"job_id": old_job_id, "timeout_ms": 3000})
        )
        assert old_wait["state"] == "exited" and old_wait["exit_code"] == 0, old_wait
        assert_snapshot(old_wait, old_snapshot)
        old_tail = json_content(
            call_tool(proc, 113, "system.shell_tail", {"job_id": old_job_id})
        )
        assert old_tail["stdout"] == "o" * 300, old_tail
        assert old_tail["stdout_truncated"] is False, old_tail
        assert_snapshot(old_tail, old_snapshot)
        jobs = json_content(call_tool(proc, 114, "system.shell_list", {}))["jobs"]
        assert_snapshot(next(job for job in jobs if job["job_id"] == old_job_id), old_snapshot)
        assert_snapshot(next(job for job in jobs if job["job_id"] == new_job_id), new_snapshot)

        state = json_content(
            call_tool(
                proc,
                115,
                "system.sandbox_ctl",
                {
                    "action": "update",
                    "token": TOKEN,
                    "expected_revision": state["revision"],
                    "overrides": {
                        "shell_enabled": None,
                        "timeout_ms": None,
                        "output_bytes": None,
                    },
                },
            )
        )

        started = call_tool(
            proc,
            5,
            "system.shell_start",
            {
                "command": "printf '%s|%s|' \"$PWD\" \"$S4_ENV\"; sleep 0.2; printf done",
                "cwd": str(async_cwd),
                "env": {"S4_ENV": "request"},
                "timeout_ms": 800,
                "output_limit_bytes": 256,
            },
        )
        assert started["isError"] is False, started
        start_payload = json_content(started)
        job_id = start_payload["job_id"]
        expected_snapshot = {
            "sandbox_revision": state["revision"],
            "sandbox_enabled": True,
            "shell_enabled": True,
            "timeout_ms": 800,
            "output_bytes": 256,
            "once_read_stdout_err_chunk_size": 64,
        }
        assert start_payload["state"] == "running", start_payload
        assert start_payload["pid"] > 0, start_payload
        assert start_payload["process_group_id"] == start_payload["pid"], start_payload
        assert start_payload["rollback"]["tool_name"] == "system.shell_kill", start_payload
        assert_snapshot(start_payload, expected_snapshot)

        poll_payload = json_content(
            call_tool(proc, 6, "system.shell_poll", {"job_id": job_id})
        )
        assert_snapshot(poll_payload, expected_snapshot)

        wait_payload = json_content(
            call_tool(proc, 7, "system.shell_wait", {"job_id": job_id, "timeout_ms": 3000})
        )
        assert wait_payload["wait_result"] == "finished", wait_payload
        assert wait_payload["state"] == "exited", wait_payload
        assert wait_payload["exit_code"] == 0, wait_payload
        assert_snapshot(wait_payload, expected_snapshot)

        tail_payload = json_content(call_tool(proc, 8, "system.shell_tail", {"job_id": job_id}))
        expected_stdout = f"{async_cwd}|request|done"
        assert tail_payload["stdout"] == expected_stdout, tail_payload
        assert tail_payload["next_stdout_offset"] == len(expected_stdout), tail_payload
        assert_snapshot(tail_payload, expected_snapshot)

        listed_payload = json_content(call_tool(proc, 9, "system.shell_list", {}))
        listed = next(job for job in listed_payload["jobs"] if job["job_id"] == job_id)
        assert_snapshot(listed, expected_snapshot)

        truncated_started = json_content(
            call_tool(
                proc,
                10,
                "system.shell_start",
                {
                    "command": "i=0; while [ \"$i\" -lt 300 ]; do printf x; i=$((i + 1)); done",
                    "output_limit_bytes": 256,
                },
            )
        )
        truncated_job_id = truncated_started["job_id"]
        truncated_wait = json_content(
            call_tool(proc, 11, "system.shell_wait", {"job_id": truncated_job_id, "timeout_ms": 3000})
        )
        assert truncated_wait["state"] == "exited", truncated_wait
        truncated_tail = json_content(call_tool(proc, 12, "system.shell_tail", {"job_id": truncated_job_id}))
        assert truncated_tail["stdout"] == "x" * 256, truncated_tail
        assert truncated_tail["stdout_truncated"] is True, truncated_tail

        bad_timeout = call_tool(proc, 13, "system.shell_start", {"command": "true", "timeout_ms": 1001})
        assert bad_timeout["isError"] is True, bad_timeout
        assert "timeout_ms" in bad_timeout["content"][0]["text"], bad_timeout
        bad_output_limit = call_tool(proc, 14, "system.shell_start", {"command": "true", "output_limit_bytes": 513})
        assert bad_output_limit["isError"] is True, bad_output_limit
        assert "output_limit_bytes" in bad_output_limit["content"][0]["text"], bad_output_limit
        bad_args = call_tool(proc, 15, "system.shell_start", {"command": "true", "args": {}})
        assert bad_args["isError"] is True, bad_args
        assert "args" in bad_args["content"][0]["text"], bad_args
        bad_env = call_tool(proc, 16, "system.shell_start", {"command": "true", "env": {"BAD=NAME": "x"}})
        assert bad_env["isError"] is True, bad_env
        assert "valid names" in bad_env["content"][0]["text"], bad_env
        bad_offset = call_tool(proc, 17, "system.shell_tail", {"job_id": job_id, "offset": 0})
        assert bad_offset["isError"] is True, bad_offset
        assert "offset" in bad_offset["content"][0]["text"], bad_offset

        state = json_content(
            call_tool(
                proc,
                18,
                "system.sandbox_ctl",
                {
                    "action": "update",
                    "token": TOKEN,
                    "expected_revision": state["revision"],
                    "overrides": {
                        "execution": {
                            "request_cwd_allowed": False,
                            "request_env_allowed": False,
                        }
                    },
                },
            )
        )
        blocked_cwd = call_tool(
            proc,
            19,
            "system.shell_start",
            {"command": "pwd", "cwd": str(async_cwd)},
        )
        assert blocked_cwd["isError"] is True and "cwd is disabled" in blocked_cwd["content"][0]["text"], blocked_cwd
        blocked_env = call_tool(
            proc,
            20,
            "system.shell_start",
            {"command": "true", "env": {"S4_ENV": "blocked"}},
        )
        assert blocked_env["isError"] is True and "env is disabled" in blocked_env["content"][0]["text"], blocked_env

        state = json_content(
            call_tool(
                proc,
                21,
                "system.sandbox_ctl",
                {
                    "action": "update",
                    "token": TOKEN,
                    "expected_revision": state["revision"],
                    "overrides": {
                        "output_bytes": 0,
                        "execution": {
                            "request_cwd_allowed": None,
                            "request_env_allowed": None,
                        },
                    },
                },
            )
        )
        zero_started = json_content(
            call_tool(proc, 22, "system.shell_start", {"command": "printf discarded"})
        )
        zero_expected = {
            "sandbox_revision": state["revision"],
            "sandbox_enabled": True,
            "shell_enabled": True,
            "timeout_ms": 1000,
            "output_bytes": 0,
            "once_read_stdout_err_chunk_size": 64,
        }
        assert_snapshot(zero_started, zero_expected)
        zero_job_id = zero_started["job_id"]
        zero_wait = json_content(
            call_tool(proc, 23, "system.shell_wait", {"job_id": zero_job_id, "timeout_ms": 3000})
        )
        assert_snapshot(zero_wait, zero_expected)
        zero_tail = json_content(call_tool(proc, 24, "system.shell_tail", {"job_id": zero_job_id}))
        assert zero_tail["stdout"] == "" and zero_tail["stdout_truncated"] is True, zero_tail
        assert_snapshot(zero_tail, zero_expected)

        marker = temp_path / "kill-marker"
        try:
            marker.unlink()
        except FileNotFoundError:
            pass
        kill_started = json_content(
            call_tool(
                proc,
                25,
                "system.shell_start",
                {
                    "command": f"sh -c 'sleep 2; touch {marker}'",
                    "timeout_ms": 800,
                    "label": "kill-check",
                },
            )
        )
        kill_job_id = kill_started["job_id"]
        killed = json_content(call_tool(proc, 26, "system.shell_kill", {"job_id": kill_job_id, "signal": signal.SIGTERM}))
        assert killed["state"] == "killed", killed
        assert killed["signal"] in (signal.SIGTERM, signal.SIGKILL), killed
        time.sleep(2.2)
        assert not marker.exists(), marker

        timeout_marker = temp_path / "timeout-group-marker"
        timeout_marker.unlink(missing_ok=True)
        timed_started = json_content(
            call_tool(
                proc,
                27,
                "system.shell_start",
                {
                    "command": f"sh -c 'sleep 2; touch {timeout_marker}'",
                    "timeout_ms": 100,
                },
            )
        )
        timed = wait_for_state(proc, timed_started["job_id"], "timed_out")
        assert timed["signal"] != 0, timed
        time.sleep(2.2)
        assert not timeout_marker.exists(), timeout_marker

        jobs = json_content(call_tool(proc, 28, "system.shell_list", {}))["jobs"]
        ids = {job["job_id"] for job in jobs}
        assert job_id in ids and kill_job_id in ids and timed_started["job_id"] in ids, jobs
        assert zero_job_id in ids and truncated_job_id in ids, jobs
        listed = next(job for job in jobs if job["job_id"] == job_id)
        assert_snapshot(listed, expected_snapshot)
    finally:
        if proc.stdin:
            proc.stdin.close()
        proc.wait(timeout=5)
        temp_dir.cleanup()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
