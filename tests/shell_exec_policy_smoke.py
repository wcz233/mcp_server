import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


TOKEN = "s3-token-must-not-appear-49a6f31d"
PARENT_SECRET = "s3-parent-secret-must-not-appear-867e"
DEFAULT_SECRET = "s3-default-secret-must-not-appear-302b"
REQUEST_SECRET = "s3-request-secret-must-not-appear-745c"
SHELL_EXEC_RESULT_FIELDS = {"stdout", "stderr", "exit_code"}
SHELL_EXEC_POLICY_FIELDS = {
    "sandbox_revision",
    "sandbox_enabled",
    "shell_enabled",
}
SHELL_EXEC_BOOLEAN_DIAGNOSTICS = {
    "timed_out",
    "truncated",
    "stdout_truncated",
    "stderr_truncated",
}


def assert_shell_result_contract(payload):
    assert SHELL_EXEC_POLICY_FIELDS.isdisjoint(payload), payload
    for field in SHELL_EXEC_BOOLEAN_DIAGNOSTICS:
        if field in payload:
            assert payload[field] is True, payload
    if "signal" in payload:
        assert type(payload["signal"]) is int and payload["signal"] != 0, payload


def shell_command(command_windows, command_unix):
    return command_windows if os.name == "nt" else command_unix


def base_environment(config_path):
    if os.name == "nt":
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        env = {
            "ComSpec": os.environ.get("ComSpec", system_root + r"\System32\cmd.exe"),
            "PATH": system_root + r"\System32;" + system_root,
            "SystemRoot": system_root,
            "TEMP": tempfile.gettempdir(),
            "TMP": tempfile.gettempdir(),
        }
    else:
        env = {"HOME": "/tmp", "LANG": "C", "PATH": "/usr/bin:/bin"}
    env.update(
        {
            "MCP_ENABLE_SANDBOX_CTL": "1",
            "MCP_SHELL_EXEC_CONFIG": str(config_path),
            "S3_LAYER": "parent",
            "S3_PARENT_ONLY": "startup-snapshot",
            "S3_PARENT_SECRET": PARENT_SECRET,
        }
    )
    return env


def valid_config(working_directory):
    if os.name == "nt":
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        shell_path = os.environ.get("ComSpec", system_root + r"\System32\cmd.exe")
        shell_arg = "/C"
        configured_env = {
            "PATH": system_root + r"\System32;" + system_root,
            "SystemRoot": system_root,
        }
    else:
        shell_path = "/bin/sh"
        shell_arg = "-c"
        configured_env = {"HOME": str(working_directory), "LANG": "C", "PATH": "/usr/bin:/bin"}
    configured_env.update(
        {
            "S3_DEFAULT_ONLY": "configured-default",
            "S3_DEFAULT_SECRET": DEFAULT_SECRET,
            "S3_LAYER": "default",
        }
    )
    return {
        "version": 2,
        "control": {"token": TOKEN},
        "defaults": {
            "shell_enabled": True,
            "command_length": 65536,
            "timeout_ms": 300000,
            "output_bytes": 65536,
            "once_read_stdout_err_chunk_size": 1024,
            "capture_stderr": True,
            "merge_stderr_to_stdout": False,
            "execution": {
                "mode": "shell",
                "shell_path": shell_path,
                "shell_arg": shell_arg,
                "working_directory": str(working_directory),
                "inherit_env": True,
                "request_cwd_allowed": True,
                "request_env_allowed": True,
                "kill_process_group_on_timeout": True,
                "run_as_user": "",
                "run_as_group": "",
                "env": configured_env,
            },
            "limits": {
                "cpu_seconds": 3600,
                "memory_bytes": 34359738367,
                "file_size_bytes": 17179869184,
                "open_files": 1024,
                "processes": 512,
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


class Client:
    def __init__(self, exe, config_path):
        self.stderr_file = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
        self.responses = []
        self.next_id = 1
        self.proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
            env=base_environment(config_path),
            text=True,
            encoding="utf-8",
        )
        response = self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "shell-policy-smoke", "version": "0.3"},
            },
        )
        assert response["result"]["serverInfo"]["name"] == "mcp_server", response
        self.notify("notifications/initialized", {})

    def request(self, method, params):
        request_id = self.next_id
        self.next_id += 1
        self.proc.stdin.write(
            json.dumps(
                {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
                separators=(",", ":"),
            )
            + "\n"
        )
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            self.stderr_file.seek(0)
            raise RuntimeError(f"server closed stdout; stderr={self.stderr_file.read()}")
        response = json.loads(line)
        self.responses.append(response)
        assert response["id"] == request_id, response
        return response

    def notify(self, method, params):
        self.proc.stdin.write(
            json.dumps({"jsonrpc": "2.0", "method": method, "params": params}, separators=(",", ":"))
            + "\n"
        )
        self.proc.stdin.flush()

    def call(self, name, arguments):
        response = self.request("tools/call", {"name": name, "arguments": arguments})
        assert "result" in response, response
        return response["result"]

    def json_call(self, name, arguments, expect_error=False):
        result = self.call(name, arguments)
        assert result["isError"] is expect_error, result
        assert result["content"] and result["content"][0]["type"] == "text", result
        return result, json.loads(result["content"][0]["text"])

    def control(self, arguments, expect_error=False):
        return self.json_call("system.sandbox_ctl", arguments, expect_error)[1]

    def get(self):
        return self.control({"action": "get", "token": TOKEN})

    def update(self, state, **values):
        return self.control(
            {
                "action": "update",
                "token": TOKEN,
                "expected_revision": state["revision"],
                **values,
            }
        )

    def shell(self, arguments, expect_error=False):
        payload = self.json_call("system.shell_exec", arguments, expect_error)[1]
        assert_shell_result_contract(payload)
        return payload

    def close(self):
        if self.proc.stdin:
            self.proc.stdin.close()
            self.proc.stdin = None
        self.proc.wait(timeout=5)
        self.stderr_file.seek(0)
        stderr = self.stderr_file.read()
        self.stderr_file.close()
        assert self.proc.returncode == 0, (self.proc.returncode, stderr)
        serialized = json.dumps(self.responses, ensure_ascii=False)
        for secret in (TOKEN, PARENT_SECRET, DEFAULT_SECRET, REQUEST_SECRET):
            assert secret not in serialized, secret
            assert secret not in stderr, secret


def assert_no_spawn(client, arguments, marker, text_fragment):
    marker.unlink(missing_ok=True)
    result = client.call("system.shell_exec", arguments)
    assert result["isError"] is True, result
    text = result["content"][0]["text"]
    assert text_fragment in text, text
    assert not marker.exists(), marker


def marker_command(marker):
    return shell_command(f'echo spawned>"{marker}"', f"touch '{marker}'")


def env_value_command(name):
    return shell_command(
        f"if defined {name} (echo %{name}%) else (echo unset)",
        f'printf \'%s\' "${{{name}-unset}}"',
    )


def verify_descriptor(client):
    response = client.request("tools/list", {})
    tool = next(item for item in response["result"]["tools"] if item["name"] == "system.shell_exec")
    properties = tool["inputSchema"]["properties"]
    assert "does not repeat the command" in tool["description"], tool
    assert properties["timeout_ms"]["minimum"] == 1, properties
    assert properties["cwd"]["type"] == "string", properties
    assert properties["env"]["additionalProperties"] == {"type": "string"}, properties


def verify_effective_policy(client, temp_path):
    state = client.get()
    assert state["effective"]["timeout_ms"] == 300000, state
    assert state["effective"]["limits"]["memory_bytes"] == 34359738367, state
    assert state["capabilities"]["timeout_ms"] == "enforced", state
    assert state["capabilities"]["execution"]["mode"] == "enforced", state
    if os.name != "nt":
        assert state["capabilities"]["limits"]["memory_bytes"] == "enforced", state
    payload = client.shell({"command": shell_command("echo smoke", "printf smoke")})
    assert payload["stdout"].strip() == "smoke", payload

    state = client.update(state, overrides={"command_length": 8})
    marker = temp_path / "strict-request.marker"
    assert_no_spawn(client, {"command": marker_command(marker) + "123456789"}, marker, "maximum length")
    state = client.update(state, overrides={"command_length": 65536, "timeout_ms": 50})
    for value in (None, "50", 0, 51):
        assert_no_spawn(
            client,
            {"command": marker_command(marker), "timeout_ms": value},
            marker,
            "timeout_ms",
        )

    timeout_command = shell_command("ping 127.0.0.1 -n 3 >nul", "sleep 1")
    timeout_payload = client.shell({"command": timeout_command}, expect_error=True)
    assert timeout_payload["timed_out"] is True, timeout_payload

    state = client.update(state, overrides={"command_length": 8})
    state = client.update(state, sandbox_enabled=False)
    assert state["overrides"]["command_length"] == 8, state
    assert state["effective"]["command_length"] == 65536, state
    payload = client.shell({"command": shell_command("echo 123456789", "printf 123456789")})
    assert payload["stdout"].strip() == "123456789", payload

    state = client.update(state, sandbox_enabled=True)
    assert state["overrides"]["command_length"] == 8, state
    assert_no_spawn(client, {"command": marker_command(marker) + "123456789"}, marker, "maximum length")
    state = client.update(state, overrides={"command_length": 65536, "timeout_ms": 300000})
    return state


def assert_compact_shell_result(payload, command, expected_stdout):
    assert set(payload) == SHELL_EXEC_RESULT_FIELDS, payload
    assert command not in (value for value in payload.values() if isinstance(value, str)), payload
    assert payload["stdout"].strip() == expected_stdout, payload
    assert payload["stderr"] == "", payload
    assert payload["exit_code"] == 0, payload


def verify_compact_result_contract(client, state, temp_path):
    if os.name == "nt":
        one_byte_command = "0"
        normal_command = "echo stage3-regular"
        max_command_prefix = "echo stage3-max&rem "
        once_command = "echo x>>stage3-once.marker"
        marker_content = "x\n"
        default_shell_arg = "/C"
    else:
        one_byte_command = ":"
        normal_command = "printf stage3-regular"
        max_command_prefix = "printf stage3-max #"
        once_command = "printf x >> stage3-once.marker"
        marker_content = "x"
        default_shell_arg = "-c"

    configured_max = 4096
    state = client.update(state, overrides={"command_length": configured_max})

    max_command = max_command_prefix + "x" * (configured_max - len(max_command_prefix))
    assert len(max_command.encode("utf-8")) == configured_max, len(max_command)

    if os.name == "nt":
        state = client.update(state, overrides={"execution": {"shell_arg": "/C exit"}})
    payload = client.shell({"command": one_byte_command})
    assert_compact_shell_result(payload, one_byte_command, "")
    if os.name == "nt":
        state = client.update(state, overrides={"execution": {"shell_arg": default_shell_arg}})

    for command, expected_stdout in (
        (normal_command, "stage3-regular"),
        (max_command, "stage3-max"),
    ):
        payload = client.shell({"command": command})
        assert_compact_shell_result(payload, command, expected_stdout)

    marker = temp_path / "stage3-once.marker"
    payload = client.shell({"command": once_command})
    assert_compact_shell_result(payload, once_command, "")
    assert marker.read_text(encoding="utf-8") == marker_content, marker.read_text(encoding="utf-8")

    return client.update(
        state,
        overrides={
            "command_length": 65536,
        },
    )


def verify_cwd_and_environment(client, state, temp_path):
    requested_cwd = temp_path / "requested-cwd"
    requested_cwd.mkdir()
    cwd_payload = client.shell({"command": shell_command("cd", "pwd"), "cwd": str(requested_cwd)})
    assert Path(cwd_payload["stdout"].strip()).resolve() == requested_cwd.resolve(), cwd_payload

    unicode_cwd = temp_path / "\u540c\u6b65\u76ee\u5f55"
    unicode_cwd.mkdir()
    cwd_payload = client.shell({"command": shell_command("cd", "pwd"), "cwd": str(unicode_cwd)})
    assert Path(cwd_payload["stdout"].strip()).resolve() == unicode_cwd.resolve(), cwd_payload

    state = client.update(state, overrides={"execution": {"request_cwd_allowed": False}})
    marker = temp_path / "cwd-disabled.marker"
    assert_no_spawn(
        client,
        {"command": marker_command(marker), "cwd": str(requested_cwd)},
        marker,
        "cwd",
    )
    state = client.update(state, overrides={"execution": {"request_cwd_allowed": True}})

    assert client.shell({"command": env_value_command("S3_PARENT_ONLY")})["stdout"].strip() == "startup-snapshot"
    assert client.shell({"command": env_value_command("S3_LAYER")})["stdout"].strip() == "default"
    request_env = {"S3_LAYER": "request", "S3_REQUEST_ONLY": "request", "S3_REQUEST_SECRET": REQUEST_SECRET}
    payload = client.shell({"command": env_value_command("S3_LAYER"), "env": request_env})
    assert payload["stdout"].strip() == "request", payload
    utf8_value = "\u9636\u6bb5-S3"
    payload = client.shell(
        {"command": env_value_command("S3_UTF8_VALUE"), "env": {"S3_UTF8_VALUE": utf8_value}}
    )
    assert payload["stdout"].strip() == utf8_value, payload
    count_command = shell_command("set S3_LAYER", "env | grep -c '^S3_LAYER='")
    count_payload = client.shell({"command": count_command, "env": request_env})
    if os.name == "nt":
        assert count_payload["stdout"].strip().lower() == "s3_layer=request", count_payload
    else:
        assert count_payload["stdout"].strip() == "1", count_payload

    state = client.update(state, overrides={"execution": {"inherit_env": False}})
    assert client.shell({"command": env_value_command("S3_PARENT_ONLY")})["stdout"].strip() == "unset"
    assert client.shell({"command": env_value_command("S3_DEFAULT_ONLY")})["stdout"].strip() == "configured-default"

    for item_bytes in (254, 255):
        value = "v" * (item_bytes - len("B="))
        client.shell({"command": shell_command("echo ok", "printf ok"), "env": {"B": value}})
    marker = temp_path / "env-item.marker"
    too_large = "v" * (256 - len("B="))
    assert_no_spawn(
        client,
        {"command": marker_command(marker), "env": {"B": too_large}},
        marker,
        "environment",
    )

    base_count = state["effective"]["execution"]["env"]["items"]
    exact_env = {f"S3_{index:04d}": "x" for index in range(1024 - base_count)}
    client.shell({"command": shell_command("echo ok", "printf ok"), "env": exact_env})
    overflow_env = dict(exact_env)
    overflow_env["S3_OVERFLOW"] = "x"
    marker = temp_path / "env-count.marker"
    assert_no_spawn(
        client,
        {"command": marker_command(marker), "env": overflow_env},
        marker,
        "environment",
    )

    state = client.update(state, overrides={"execution": {"request_env_allowed": False}})
    marker = temp_path / "env-disabled.marker"
    assert_no_spawn(
        client,
        {"command": marker_command(marker), "env": {"S3_REQUEST_ONLY": "blocked"}},
        marker,
        "env",
    )
    return client.update(
        state,
        overrides={"execution": {"request_env_allowed": True, "inherit_env": True}},
    )


def verify_output_and_shell_arg(client, state, temp_path):
    both_streams = shell_command("echo abcdef&echo ghijkl 1>&2", "printf abcdef; printf ghijkl >&2")
    state = client.update(
        state,
        overrides={
            "output_bytes": 0,
            "once_read_stdout_err_chunk_size": 64,
            "capture_stderr": True,
            "merge_stderr_to_stdout": False,
        },
    )
    payload = client.shell({"command": both_streams})
    assert payload["stdout"] == "" and payload["stderr"] == "", payload
    assert payload["truncated"] is True, payload
    assert payload["stdout_truncated"] is True and payload["stderr_truncated"] is True, payload

    state = client.update(state, overrides={"output_bytes": 4})
    payload = client.shell({"command": both_streams})
    assert len(payload["stdout"]) == 4 and len(payload["stderr"]) == 4, payload
    assert payload["stdout_truncated"] is True and payload["stderr_truncated"] is True, payload

    state = client.update(
        state,
        overrides={"output_bytes": 5, "capture_stderr": True, "merge_stderr_to_stdout": True},
    )
    payload = client.shell({"command": both_streams})
    assert len(payload["stdout"]) == 5 and payload["stderr"] == "", payload
    assert payload["stdout_truncated"] is True and "stderr_truncated" not in payload, payload

    state = client.update(state, overrides={"capture_stderr": False, "merge_stderr_to_stdout": True})
    assert state["effective"]["merge_stderr_to_stdout"] is False, state
    payload = client.shell({"command": shell_command("echo ignored 1>&2", "printf ignored >&2")})
    assert payload["stderr"] == "" and "stderr_truncated" not in payload, payload

    if os.name == "nt":
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        empty_arg_shell = system_root + r"\System32\where.exe"
        empty_arg_command = "cmd.exe"
    else:
        probe = temp_path / "argv_probe.py"
        probe.write_text("import sys\nprint(len(sys.argv))\n", encoding="utf-8")
        empty_arg_shell = sys.executable
        empty_arg_command = str(probe)
    state = client.update(
        state,
        overrides={
            "output_bytes": 65536,
            "capture_stderr": True,
            "merge_stderr_to_stdout": False,
            "execution": {"shell_path": empty_arg_shell, "shell_arg": ""},
        },
    )
    payload = client.shell({"command": empty_arg_command})
    if os.name == "nt":
        assert payload["stdout"].strip().lower().endswith(r"\cmd.exe"), payload
    else:
        assert payload["stdout"].strip() == "1", payload

    if os.name == "nt":
        exec_command = system_root + r"\System32\whoami.exe"
        default_shell_path = os.environ.get("ComSpec", system_root + r"\System32\cmd.exe")
        default_shell_arg = "/C"
    else:
        exec_command = "/usr/bin/true"
        default_shell_path = "/bin/sh"
        default_shell_arg = "-c"
    state = client.update(state, overrides={"execution": {"mode": "exec"}})
    payload = client.shell({"command": exec_command})
    assert payload["exit_code"] == 0, payload
    state = client.update(
        state,
        overrides={
            "execution": {
                "mode": "shell",
                "shell_path": default_shell_path,
                "shell_arg": default_shell_arg,
            }
        },
    )
    return state


def verify_64_bit_rlimit(client, state):
    if os.name == "nt":
        payload = client.shell({"command": "echo windows-limits-ignored"})
        assert payload["stdout"].strip() == "windows-limits-ignored", payload
        return state

    state = client.update(state, overrides={"limits": {"memory_bytes": 34359738367}})
    assert state["effective"]["limits"]["memory_bytes"] == 34359738367, state
    payload = client.shell({"command": "ulimit -v"})
    reported_kib = int(payload["stdout"].strip())
    assert reported_kib * 1024 <= 34359738367, payload
    assert 34359738367 - reported_kib * 1024 < 1024, payload
    return state


def verify_platform_limits_and_identity(client, state, temp_path):
    limit_names = (
        "cpu_seconds",
        "memory_bytes",
        "file_size_bytes",
        "open_files",
        "processes",
    )
    if os.name == "nt":
        for name in limit_names:
            assert state["capabilities"]["limits"][name] == "unsupported", state
        assert state["capabilities"]["execution"]["run_as_user"] == "unsupported", state
        assert state["capabilities"]["execution"]["run_as_group"] == "unsupported", state
        return state

    import grp
    import pwd
    import resource

    def within_parent_limit(resource_id, requested):
        inherited_hard = resource.getrlimit(resource_id)[1]
        if inherited_hard == resource.RLIM_INFINITY:
            return requested
        return min(requested, inherited_hard)

    limits = {
        "cpu_seconds": within_parent_limit(resource.RLIMIT_CPU, 120),
        "memory_bytes": within_parent_limit(resource.RLIMIT_AS, 1073741824),
        "file_size_bytes": within_parent_limit(resource.RLIMIT_FSIZE, 16777216),
        "open_files": within_parent_limit(resource.RLIMIT_NOFILE, 128),
        "processes": within_parent_limit(resource.RLIMIT_NPROC, 64),
    }
    for name in limit_names:
        assert state["capabilities"]["limits"][name] == "enforced", state
    assert state["capabilities"]["execution"]["run_as_user"] == "enforced", state
    assert state["capabilities"]["execution"]["run_as_group"] == "enforced", state

    expected_uid = os.geteuid()
    expected_gid = os.getegid()
    identity = {}
    if expected_uid == 0:
        target = next(entry for entry in pwd.getpwall() if entry.pw_uid != 0)
        expected_uid = target.pw_uid
        expected_gid = target.pw_gid
        temp_path.chmod(0o755)
        identity = {
            "run_as_user": target.pw_name,
            "run_as_group": grp.getgrgid(target.pw_gid).gr_name,
        }
    else:
        identity = {"run_as_group": grp.getgrgid(expected_gid).gr_name}

    probe = temp_path / "unix_policy_probe.py"
    probe.write_text(
        "import json, os, resource\n"
        "print(json.dumps({\n"
        "    'uid': os.geteuid(),\n"
        "    'gid': os.getegid(),\n"
        "    'cpu_seconds': resource.getrlimit(resource.RLIMIT_CPU)[0],\n"
        "    'memory_bytes': resource.getrlimit(resource.RLIMIT_AS)[0],\n"
        "    'file_size_bytes': resource.getrlimit(resource.RLIMIT_FSIZE)[0],\n"
        "    'open_files': resource.getrlimit(resource.RLIMIT_NOFILE)[0],\n"
        "    'processes': resource.getrlimit(resource.RLIMIT_NPROC)[0],\n"
        "}))\n",
        encoding="utf-8",
    )
    state = client.update(
        state,
        overrides={
            "execution": {
                "mode": "shell",
                "shell_path": sys.executable,
                "shell_arg": "",
                **identity,
            },
            "limits": limits,
        },
    )
    payload = client.shell({"command": str(probe)})
    observed = json.loads(payload["stdout"])
    assert observed["uid"] == expected_uid, observed
    assert observed["gid"] == expected_gid, observed
    for name, value in limits.items():
        assert observed[name] == value, observed
    return state


def main():
    exe = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="mcp-shell-policy-") as temp_dir:
        temp_path = Path(temp_dir)
        config_path = temp_path / "shell_exec.json"
        config_path.write_text(
            json.dumps(valid_config(temp_path), separators=(",", ":")),
            encoding="utf-8",
        )
        client = Client(exe, config_path)
        try:
            verify_descriptor(client)
            state = verify_effective_policy(client, temp_path)
            state = verify_compact_result_contract(client, state, temp_path)
            state = verify_cwd_and_environment(client, state, temp_path)
            state = verify_output_and_shell_arg(client, state, temp_path)
            state = verify_64_bit_rlimit(client, state)
            verify_platform_limits_and_identity(client, state, temp_path)
        finally:
            client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
