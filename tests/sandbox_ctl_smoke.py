import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


TOKEN = "s2-token-must-not-appear-7f51b36e"

V2_DEFAULT_PATHS = {
    ("shell_enabled",),
    ("command_length",),
    ("timeout_ms",),
    ("output_bytes",),
    ("once_read_stdout_err_chunk_size",),
    ("capture_stderr",),
    ("merge_stderr_to_stdout",),
    ("execution", "mode"),
    ("execution", "shell_path"),
    ("execution", "shell_arg"),
    ("execution", "working_directory"),
    ("execution", "inherit_env"),
    ("execution", "request_cwd_allowed"),
    ("execution", "request_env_allowed"),
    ("execution", "kill_process_group_on_timeout"),
    ("execution", "run_as_user"),
    ("execution", "run_as_group"),
    ("execution", "env"),
    ("limits", "cpu_seconds"),
    ("limits", "memory_bytes"),
    ("limits", "file_size_bytes"),
    ("limits", "open_files"),
    ("limits", "processes"),
    ("isolation", "require_non_root"),
}


def control_env(config_path, gate=None, token=None):
    env = os.environ.copy()
    env["MCP_SHELL_EXEC_CONFIG"] = str(config_path)
    env.pop("MCP_ENABLE_SANDBOX_CTL", None)
    env.pop("MCP_SANDBOX_CTL_TOKEN", None)
    if gate is not None:
        env["MCP_ENABLE_SANDBOX_CTL"] = gate
    if token is not None:
        env["MCP_SANDBOX_CTL_TOKEN"] = token
    return env


def valid_config(working_directory):
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
                "shell_path": "/bin/sh",
                "shell_arg": "-c",
                "working_directory": str(working_directory),
                "inherit_env": False,
                "request_cwd_allowed": True,
                "request_env_allowed": True,
                "kill_process_group_on_timeout": True,
                "run_as_user": "",
                "run_as_group": "",
                "env": {"PATH": "/usr/bin:/bin", "HOME": str(working_directory), "LANG": "C"},
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


def write_config(path, value):
    path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")


class Client:
    def __init__(self, exe, env, preexec_fn=None):
        self.stderr_file = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
        self.proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
            env=env,
            text=True,
            encoding="utf-8",
            preexec_fn=preexec_fn,
        )
        self.next_id = 1
        self.responses = []
        self.stderr = ""
        init = self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "sandbox-ctl-smoke", "version": "0.2"},
            },
        )
        assert init["result"]["serverInfo"]["name"] == "mcp_server", init
        self.notify("notifications/initialized", {})

    def send(self, payload):
        self.proc.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()

    def receive(self):
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout")
        response = json.loads(line)
        self.responses.append(response)
        return response

    def request(self, method, params):
        request_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        response = self.receive()
        if response.get("id") != request_id:
            assert response.get("id") is None, response
            assert response.get("error", {}).get("code") == -32700, response
        return response

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def tools(self):
        return self.request("tools/list", {})["result"]["tools"]

    def call(self, name, arguments):
        return self.request("tools/call", {"name": name, "arguments": arguments})

    def control_response(self, arguments):
        return self.call("system.sandbox_ctl", arguments)

    def control(self, arguments, expect_error=False):
        response = self.control_response(arguments)
        assert "result" in response, response
        result = response["result"]
        assert result["isError"] is expect_error, result
        content = result["content"]
        assert content and content[0]["type"] == "text", result
        return json.loads(content[0]["text"])

    def get(self):
        return self.control({"action": "get", "token": TOKEN})

    def close(self):
        if self.proc.stdin:
            self.proc.stdin.close()
            self.proc.stdin = None
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
            raise AssertionError("server did not exit")
        self.stderr_file.seek(0)
        self.stderr = self.stderr_file.read()
        self.stderr_file.close()
        assert self.proc.returncode == 0, (self.proc.returncode, self.stderr)
        assert "uv_loop_close" not in self.stderr, self.stderr
        serialized_responses = json.dumps(self.responses, ensure_ascii=False)
        assert TOKEN not in serialized_responses, serialized_responses
        assert TOKEN not in self.stderr, self.stderr
        return self.stderr


def descriptor_by_name(tools, name):
    return next(tool for tool in tools if tool["name"] == name)


def nested_get(value, path):
    for key in path:
        value = value[key]
    return value


def nested_has(value, path):
    for key in path[:-1]:
        if key not in value:
            return False
        value = value[key]
    return path[-1] in value


def schema_leaf_paths(schema, prefix=()):
    if schema.get("type") == "object" and "properties" in schema:
        paths = set()
        for key, child in schema["properties"].items():
            paths.update(schema_leaf_paths(child, prefix + (key,)))
        return paths
    return {prefix}


def schema_at_path(schema, path):
    for key in path:
        schema = schema["properties"][key]
    return schema


def override_patch(path, value):
    patch = value
    for key in reversed(path):
        patch = {key: patch}
    return patch


def update_arguments(state, **values):
    return {
        "action": "update",
        "token": TOKEN,
        "expected_revision": state["revision"],
        **values,
    }


def shell_command(command_windows, command_unix):
    return command_windows if os.name == "nt" else command_unix


def shell_result(client, arguments):
    response = client.call("system.shell_exec", arguments)
    assert "result" in response, response
    result = response["result"]
    content = result["content"]
    assert content and content[0]["type"] == "text", result
    return result, content[0]["text"]


def successful_shell_payload(client, arguments):
    result, text = shell_result(client, arguments)
    assert result["isError"] is False, result
    return json.loads(text)


def successful_job_payload(client, name, arguments):
    response = client.call(name, arguments)
    assert "result" in response, response
    result = response["result"]
    assert result["isError"] is False, result
    return json.loads(result["content"][0]["text"])


def assert_job_error(client, name, arguments, expected_text):
    response = client.call(name, arguments)
    result = response["result"]
    assert result["isError"] is True, result
    assert expected_text in result["content"][0]["text"], result


def assert_error_unchanged(client, arguments, code="invalid_params", field=None):
    before = client.get()
    response = client.control_response(arguments)
    if "result" in response:
        result = response["result"]
        assert result["isError"] is True, result
        payload = json.loads(result["content"][0]["text"])
        assert payload["code"] == code, payload
        if field is not None:
            assert payload.get("field") == field, payload
    else:
        assert "error" in response, response
    after = client.get()
    assert after == before, (before, response, after)
    return response


def assert_successful_update(client, state, path, value, extra_overrides=None):
    patch = override_patch(path, value)
    if extra_overrides:
        for key, extra_value in extra_overrides.items():
            patch[key] = extra_value
    updated = client.control(update_arguments(state, overrides=patch))
    assert updated["revision"] == state["revision"] + 1, updated
    assert nested_get(updated["overrides"], path) == value, updated
    assert nested_get(updated["effective"], path) == value, updated
    return updated


def assert_valid_or_unsupported(client, state, path, value, unsupported, extra_overrides=None):
    patch = override_patch(path, value)
    if extra_overrides:
        for key, extra_value in extra_overrides.items():
            patch[key] = extra_value
    arguments = update_arguments(state, overrides=patch)
    if unsupported:
        assert_error_unchanged(
            client, arguments, code="unsupported_on_platform", field=".".join(path)
        )
        return state
    return assert_successful_update(client, state, path, value, extra_overrides)


def numeric_patch(path, value):
    patch = override_patch(path, value)
    if path == ("default_timeout_ms",):
        patch["max_timeout_ms"] = 300000
    elif path == ("max_timeout_ms",):
        patch["default_timeout_ms"] = 1
    return patch


def exercise_numeric_boundaries(client):
    windows = sys.platform == "win32"
    cases = [
        (("max_command_length",), 64, 65535, [128, 4096, 64000], False),
        (("default_timeout_ms",), 1, 300000, [100, 150000, 299000], False),
        (("max_timeout_ms",), 1, 300000, [100, 150000, 299000], False),
        (("max_output_bytes",), 256, 2147483648, [1024, 1048576, 1073741824], False),
        (("limits", "max_cpu_seconds"), 0, 3600, [1, 1800, 3500], windows),
        (("limits", "max_memory_bytes"), 0, 2147483647, [1, 1048576, 1073741824], windows),
        (("limits", "max_file_size_bytes"), 0, 2147483647, [1, 1048576, 1073741824], windows),
        (("limits", "max_open_files"), 0, 1048576, [1, 4096, 1000000], windows),
        (("limits", "max_processes"), 0, 1048576, [1, 4096, 1000000], windows),
    ]
    for path, minimum, maximum, typical, unsupported in cases:
        state = client.get()
        if state["overrides"]:
            state = client.control(
                {"action": "reset", "token": TOKEN, "expected_revision": state["revision"]}
            )
        assert not nested_has(state["overrides"], path), (path, state)
        for value in [*typical, minimum, maximum]:
            patch = numeric_patch(path, value)
            if unsupported:
                assert_error_unchanged(
                    client,
                    update_arguments(state, overrides=patch),
                    code="unsupported_on_platform",
                    field=".".join(path),
                )
            else:
                state = client.control(update_arguments(state, overrides=patch))
                assert nested_get(state["effective"], path) == value, (path, value, state)

        invalid_values = [minimum - 1, maximum + 1, minimum - 100000, maximum + 1000000, ""]
        for value in invalid_values:
            assert_error_unchanged(
                client,
                update_arguments(state, overrides=numeric_patch(path, value)),
                field=".".join(path),
            )
        for value in [-(2**63) - 1, 2**63]:
            assert_error_unchanged(
                client,
                update_arguments(state, overrides=numeric_patch(path, value)),
                field=None,
            )

        state = client.control(update_arguments(state, overrides=override_patch(path, None)))
        assert not nested_has(state["overrides"], path), (path, state)


def exercise_working_directory_boundaries(client):
    path = ("execution", "working_directory")
    state = client.get()
    for length in [2, 17, 1024, 1, 4096]:
        value = "x" * length
        state = assert_successful_update(client, state, path, value)
    for value in ["", "x" * 4097, "x" * 5000, [], {}, "a\x00b"]:
        assert_error_unchanged(
            client,
            update_arguments(state, overrides=override_patch(path, value)),
            field="execution.working_directory",
        )
    state = client.control(update_arguments(state, overrides=override_patch(path, None)))
    assert not nested_has(state["overrides"], path), state


def exercise_allowed_env_boundaries(client):
    path = ("execution", "allowed_env")
    unsupported = sys.platform == "win32"
    state = client.get()
    for length in [2, 31, 200, 1, 255]:
        value = ["A" * length]
        state = assert_valid_or_unsupported(client, state, path, value, unsupported)
    for count in [0, 3, 64, 127, 128]:
        value = [f"V{index}" for index in range(count)]
        state = assert_valid_or_unsupported(client, state, path, value, unsupported)

    invalid_values = [
        [""],
        ["A" * 256],
        ["A" * 300],
        ["A=B"],
        ["A\x00B"],
        [1],
        "",
        {},
        [f"V{index}" for index in range(129)],
        [f"V{index}" for index in range(256)],
    ]
    for value in invalid_values:
        assert_error_unchanged(
            client,
            update_arguments(state, overrides=override_patch(path, value)),
            field="execution.allowed_env",
        )
    state = client.control(update_arguments(state, overrides=override_patch(path, None)))
    assert not nested_has(state["overrides"], path), state


def exercise_boolean_and_identity_fields(client):
    windows = sys.platform == "win32"
    boolean_cases = [
        (("capture_stderr",), False),
        (("merge_stderr",), False),
        (("execution", "request_cwd_allowed"), windows),
        (("execution", "clear_environment"), False),
        (("execution", "request_env_allowed"), windows),
        (("execution", "kill_process_group_on_timeout"), windows),
        (("isolation", "require_non_root"), False),
    ]
    for path, unsupported in boolean_cases:
        state = client.get()
        for value in [True, False]:
            extra = {"merge_stderr": False} if path == ("capture_stderr",) and not value else None
            state = assert_valid_or_unsupported(client, state, path, value, unsupported, extra)
        for value in ["", [], {}, 0]:
            assert_error_unchanged(
                client,
                update_arguments(state, overrides=override_patch(path, value)),
                field=".".join(path),
            )
        state = client.control(update_arguments(state, overrides=override_patch(path, None)))
        assert not nested_has(state["overrides"], path), state

    for path in [("execution", "run_as_user"), ("execution", "run_as_group")]:
        state = client.get()
        state = assert_successful_update(client, state, path, "")
        if windows:
            assert_error_unchanged(
                client,
                update_arguments(state, overrides=override_patch(path, "sandbox-user")),
                code="unsupported_on_platform",
                field=".".join(path),
            )
        else:
            state = assert_successful_update(client, state, path, "sandbox-user")
        for value in [1, [], {}, "a\x00b"]:
            assert_error_unchanged(
                client,
                update_arguments(state, overrides=override_patch(path, value)),
                field=".".join(path),
            )
        state = client.control(update_arguments(state, overrides=override_patch(path, None)))
        assert not nested_has(state["overrides"], path), state


def verify_descriptor_and_auth(exe, config_path):
    client = Client(exe, control_env(config_path))
    tools = client.tools()
    descriptor = descriptor_by_name(tools, "system.sandbox_ctl")
    annotations = descriptor["annotations"]
    assert annotations == {
        "source": "builtin",
        "route": "local_builtin",
        "risk_level": "L4",
        "permission": "system.sandbox.control",
        "idempotent": False,
        "retryable": False,
        "cancelable": False,
        "timeout_ms": 1000,
    }, annotations
    schema = descriptor["inputSchema"]
    assert schema["additionalProperties"] is False, schema
    assert schema["required"] == ["action", "token"], schema
    assert set(schema["properties"]) == {
        "action",
        "token",
        "expected_revision",
        "sandbox_enabled",
        "overrides",
    }, schema
    assert schema["properties"]["overrides"]["additionalProperties"] is False, schema
    assert schema["properties"]["overrides"]["properties"]["execution"][
        "additionalProperties"
    ] is False, schema
    assert schema["properties"]["overrides"]["properties"]["limits"][
        "additionalProperties"
    ] is False, schema
    assert schema["properties"]["overrides"]["properties"]["isolation"][
        "additionalProperties"
    ] is False, schema
    override_schema = schema["properties"]["overrides"]
    assert schema_leaf_paths(override_schema) == V2_DEFAULT_PATHS, override_schema
    numeric_bounds = {
        ("command_length",): (1, 65536),
        ("timeout_ms",): (1, 3600000),
        ("output_bytes",): (0, 1048576),
        ("once_read_stdout_err_chunk_size",): (64, 65536),
        ("limits", "cpu_seconds"): (0, 3600),
        ("limits", "memory_bytes"): (0, 34359738367),
        ("limits", "file_size_bytes"): (0, 17179869184),
        ("limits", "open_files"): (0, 65536),
        ("limits", "processes"): (0, 2048),
    }
    for path, (minimum, maximum) in numeric_bounds.items():
        options = schema_at_path(override_schema, path)["anyOf"]
        integer = next(item for item in options if item["type"] == "integer")
        assert integer["minimum"] == minimum, (path, integer)
        assert integer["maximum"] == maximum, (path, integer)

    mode_options = schema_at_path(override_schema, ("execution", "mode"))["anyOf"]
    mode = next(item for item in mode_options if item["type"] == "string")
    assert mode["enum"] == ["shell", "exec"], mode

    error = client.control({"action": "get", "token": TOKEN}, expect_error=True)
    assert error == {
        "code": "unauthorized",
        "message": "sandbox control authentication failed",
    }, error
    client.close()


def field_status(state, path):
    return state["field_status"][".".join(path)]


def assert_redacted_environment(state, expected_items):
    for section in ("base", "effective"):
        summary = nested_get(state[section], ("execution", "env"))
        assert summary == {"items": expected_items, "valid": True}, (section, summary)


def verify_v2_control_state(exe, config_path, config):
    environment_secret = "environment-value-must-not-appear-32d53d"
    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1"))
    state = client.get()

    assert state["revision"] == 0, state
    assert state["persistence"] == "process", state
    assert state["applies_to"] == "new_executions", state
    assert state["config_loaded"] is True and state["config_version"] == 2, state
    assert state["policy_valid"] is True and state["sandbox_enabled"] is True, state
    assert state["shell_enabled"] is True and state["shell_enabled_override"] is None, state
    assert state["overrides"] == {}, state
    assert set(state["field_status"]) == {".".join(path) for path in V2_DEFAULT_PATHS}, state
    assert_redacted_environment(state, 3)
    for path in V2_DEFAULT_PATHS:
        status = field_status(state, path)
        assert status["source"] == "json", (path, status)
        assert status["diagnostic"] is None, (path, status)
        assert status["capability"] in {
            "enforced",
            "unsupported",
            "reject_only",
            "ignored",
        }, (path, status)

    update_values = {
        ("shell_enabled",): False,
        ("command_length",): 2048,
        ("timeout_ms",): 2000,
        ("output_bytes",): 0,
        ("once_read_stdout_err_chunk_size",): 256,
        ("capture_stderr",): True,
        ("merge_stderr_to_stdout",): True,
        ("execution", "mode"): "exec",
        ("execution", "shell_path"): "cmd.exe" if os.name == "nt" else "/bin/sh",
        ("execution", "shell_arg"): "",
        ("execution", "working_directory"): str(config_path.parent),
        ("execution", "inherit_env"): True,
        ("execution", "request_cwd_allowed"): False,
        ("execution", "request_env_allowed"): False,
        ("execution", "kill_process_group_on_timeout"): False,
        ("execution", "run_as_user"): "sandbox-user",
        ("execution", "run_as_group"): "sandbox-group",
        ("execution", "env"): {"S2_REDACTED": environment_secret},
        ("limits", "cpu_seconds"): 1,
        ("limits", "memory_bytes"): 34359738367,
        ("limits", "file_size_bytes"): 17179869184,
        ("limits", "open_files"): 32,
        ("limits", "processes"): 8,
        ("isolation", "require_non_root"): True,
    }

    for path in sorted(V2_DEFAULT_PATHS):
        value = update_values[path]
        capability = field_status(state, path)["capability"]
        arguments = update_arguments(state, overrides=override_patch(path, value))
        if capability == "unsupported":
            assert_error_unchanged(
                client,
                arguments,
                code="unsupported_on_platform",
                field=".".join(path),
            )
            continue

        previous_revision = state["revision"]
        state = client.control(arguments)
        assert state["revision"] == previous_revision + 1, (path, state)
        assert field_status(state, path)["source"] == "runtime", (path, state)
        if path == ("execution", "env"):
            assert nested_get(state["overrides"], path) == {"items": 1, "valid": True}, state
            assert nested_get(state["effective"], path) == {"items": 1, "valid": True}, state
        else:
            assert nested_get(state["overrides"], path) == value, (path, state)
            assert nested_get(state["effective"], path) == value, (path, state)
        assert client.get() == state, (path, state)

    assert environment_secret not in json.dumps(state, ensure_ascii=False), state
    command_status = field_status(state, ("command_length",))
    assert command_status["bounds"] == {"min": 1, "max": 65536}, command_status
    memory_status = field_status(state, ("limits", "memory_bytes"))
    assert memory_status["bounds"] == {"min": 0, "max": 34359738367}, memory_status

    before_invalid = copy.deepcopy(state)
    assert_error_unchanged(
        client,
        update_arguments(
            state,
            overrides={"command_length": 4096, "unknown": "must-not-be-logged"},
        ),
        field="overrides",
    )
    assert client.get() == before_invalid, state
    assert_error_unchanged(
        client,
        {
            "action": "update",
            "token": TOKEN,
            "expected_revision": state["revision"] - 1,
            "overrides": {"command_length": 4096},
        },
        code="revision_conflict",
    )

    state = client.control(
        update_arguments(state, overrides=override_patch(("command_length",), None))
    )
    assert not nested_has(state["overrides"], ("command_length",)), state
    assert nested_get(state["effective"], ("command_length",)) == 65536, state
    assert field_status(state, ("command_length",))["source"] == "json", state

    state = client.control(update_arguments(state, sandbox_enabled=False))
    assert state["sandbox_enabled"] is False and state["overrides"], state
    state = client.control(update_arguments(state, sandbox_enabled=True))
    assert state["sandbox_enabled"] is True and state["overrides"], state

    marker = config_path.parent / "s2-must-not-reach-spawn.marker"
    marker.unlink(missing_ok=True)
    command = shell_command(f'type nul > "{marker}"', f"touch '{marker}'")
    result, _ = shell_result(client, {"command": command})
    assert result["isError"] is True, result
    assert not marker.exists(), marker

    reset = client.control(
        {"action": "reset", "token": TOKEN, "expected_revision": state["revision"]}
    )
    assert reset["revision"] == state["revision"] + 1, reset
    assert reset["overrides"] == {}, reset
    assert reset["sandbox_enabled"] is True, reset
    assert reset["shell_enabled_override"] is None, reset
    stderr = client.close()
    assert environment_secret not in stderr, stderr
    assert "must-not-be-logged" not in stderr, stderr


def verify_field_fallback_diagnostics(exe, config_path, config):
    config["defaults"]["command_length"] = 1024
    config["bounds"]["command_length"]["max"] = 65537
    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1"))
    state = client.get()

    status = field_status(state, ("command_length",))
    assert nested_get(state["base"], ("command_length",)) == 65536, state
    assert status["source"] == "hard_fallback", status
    assert status["bounds"] == {"min": 1, "max": 65536}, status
    assert status["diagnostic"] == "bound_above_hard_max", status
    assert state["diagnostics"] == [
        {
            "field": "command_length",
            "source": "hard_fallback",
            "reason": "bound_above_hard_max",
        }
    ], state
    assert field_status(state, ("timeout_ms",))["source"] == "json", state

    state = client.control(
        update_arguments(state, overrides={"command_length": 65536})
    )
    assert nested_get(state["effective"], ("command_length",)) == 65536, state
    assert field_status(state, ("command_length",))["source"] == "runtime", state
    assert field_status(state, ("command_length",))["diagnostic"] == (
        "bound_above_hard_max"
    ), state
    assert_error_unchanged(
        client,
        update_arguments(state, overrides={"command_length": 65537}),
        field="command_length",
    )
    client.close()


def verify_snapshot_token_auth(exe, config_path, config):
    write_config(config_path, config)
    env = control_env(config_path, gate="1", token="legacy-token-must-not-authenticate")
    client = Client(exe, env)
    error = client.control(
        {"action": "get", "token": "legacy-token-must-not-authenticate"},
        expect_error=True,
    )
    assert error == {
        "code": "unauthorized",
        "message": "sandbox control authentication failed",
    }, error
    state = client.get()
    assert state["revision"] == 0, state
    client.close()


def verify_invalid_config(exe, config_path):
    config_path.write_text("{invalid", encoding="utf-8")
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    state = client.get()
    assert state["revision"] == 0, state
    assert state["config_loaded"] is False, state
    assert state["policy_valid"] is False, state
    assert state["base"] is None and state["effective"] is None, state
    assert state["shell_enabled"] is None, state
    assert state["overrides"] == {}, state
    assert state["config_error"], state

    assert_error_unchanged(
        client,
        {"action": "update", "token": TOKEN, "expected_revision": 1, "shell_enabled": True},
        code="revision_conflict",
    )
    assert_error_unchanged(
        client,
        {"action": "update", "token": TOKEN, "expected_revision": 0, "shell_enabled": True},
        code="config_unavailable",
    )
    assert_error_unchanged(
        client,
        {"action": "reset", "token": TOKEN, "expected_revision": 0},
        code="config_unavailable",
    )
    client.close()


def verify_control_contract(exe, config_path, config):
    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    descriptor_by_name(client.tools(), "system.sandbox_ctl")

    state = client.get()
    assert state["revision"] == 0, state
    assert state["persistence"] == "process", state
    assert state["applies_to"] == "new_executions", state
    assert state["shell_enabled"] is False, state
    assert state["shell_enabled_override"] is None, state
    assert state["sandbox_enabled"] is True, state
    assert state["config_loaded"] is True and state["policy_valid"] is True, state
    assert state["overrides"] == {}, state
    assert "sandbox_enabled" not in state["base"], state
    assert state["effective"]["sandbox_enabled"] is True, state

    for arguments in [
        {},
        {"action": "get"},
        {"action": "get", "token": ""},
        {"action": "get", "token": "wrong"},
        {"action": "get", "token": 1},
        {"action": 1, "token": "wrong"},
    ]:
        error = client.control(arguments, expect_error=True)
        assert error["code"] == "unauthorized", (arguments, error)

    invalid_requests = [
        ({"action": "unknown", "token": TOKEN}, "action"),
        ({"action": "get", "token": TOKEN, "extra": True}, "extra"),
        ({"action": "update", "token": TOKEN, "shell_enabled": True}, "expected_revision"),
        (
            {"action": "update", "token": TOKEN, "expected_revision": -1, "shell_enabled": True},
            "expected_revision",
        ),
        (
            {"action": "update", "token": TOKEN, "expected_revision": "0", "shell_enabled": True},
            "expected_revision",
        ),
        (
            {"action": "update", "token": TOKEN, "expected_revision": 0, "overrides": {}},
            "overrides",
        ),
        (
            {
                "action": "update",
                "token": TOKEN,
                "expected_revision": 0,
                "overrides": {"unknown": 1},
            },
            "overrides",
        ),
        (
            {
                "action": "update",
                "token": TOKEN,
                "expected_revision": 0,
                "overrides": {"execution": {"unknown": 1}},
            },
            "overrides.execution",
        ),
        (
            {"action": "reset", "token": TOKEN, "expected_revision": 0, "extra": True},
            "extra",
        ),
    ]
    for arguments, field in invalid_requests:
        assert_error_unchanged(client, arguments, field=field)

    for value in ["", 0, [], {}]:
        assert_error_unchanged(
            client,
            update_arguments(state, shell_enabled=value),
            field="shell_enabled",
        )
    for value in [None, "", 0, [], {}]:
        assert_error_unchanged(
            client,
            update_arguments(state, sandbox_enabled=value),
            field="sandbox_enabled",
        )
    for value in [-(2**63) - 1, 2**63]:
        assert_error_unchanged(
            client,
            {
                "action": "update",
                "token": TOKEN,
                "expected_revision": value,
                "shell_enabled": True,
            },
            field=None,
        )

    assert_error_unchanged(
        client,
        {"action": "update", "token": TOKEN, "expected_revision": 1, "shell_enabled": True},
        code="revision_conflict",
    )
    assert_error_unchanged(
        client,
        update_arguments(
            state,
            overrides={"default_timeout_ms": 2000, "max_timeout_ms": 1000},
        ),
        code="invalid_params",
    )
    assert_error_unchanged(
        client,
        update_arguments(state, overrides={"capture_stderr": False, "merge_stderr": True}),
        code="invalid_params",
    )

    state = client.control(update_arguments(state, shell_enabled=False))
    assert state["revision"] == 1 and state["overrides"]["shell_enabled"] is False, state
    state = client.control(update_arguments(state, shell_enabled=False))
    assert state["revision"] == 2 and state["overrides"]["shell_enabled"] is False, state
    state = client.control(update_arguments(state, sandbox_enabled=False))
    assert state["sandbox_enabled"] is False and state["revision"] == 3, state
    state = client.control(update_arguments(state, sandbox_enabled=True))
    assert state["sandbox_enabled"] is True and state["revision"] == 4, state
    state = client.control(update_arguments(state, shell_enabled=None))
    assert state["shell_enabled_override"] is None, state
    assert "shell_enabled" not in state["overrides"], state

    exercise_numeric_boundaries(client)
    exercise_working_directory_boundaries(client)
    exercise_allowed_env_boundaries(client)
    exercise_boolean_and_identity_fields(client)

    state = client.get()
    state = client.control(update_arguments(state, shell_enabled=True))
    assert state["shell_enabled"] is True, state
    result, text = shell_result(client, {"command": "echo runtime-enabled"})
    assert result["isError"] is False, result
    payload = json.loads(text)
    assert payload["stdout"].strip() == "runtime-enabled", payload
    assert payload["sandbox_revision"] == state["revision"], payload
    assert payload["sandbox_enabled"] is True, payload
    assert payload["shell_enabled"] is True, payload

    state = client.control(
        update_arguments(
            state,
            overrides={"default_timeout_ms": 100, "max_timeout_ms": 200},
        )
    )
    for timeout_value in (1, 50, 100, 199, 200):
        result, text = shell_result(
            client,
            {"command": "echo timeout-valid", "timeout_ms": timeout_value},
        )
        payload = json.loads(text)
        if timeout_value == 1 and result["isError"] is True:
            assert payload["timed_out"] is True, payload
        else:
            assert result["isError"] is False, (timeout_value, result)
            assert payload["stdout"].strip() == "timeout-valid", payload

    for index, timeout_value in enumerate((None, "100", 0, 201, 300001), start=1):
        marker = config_path.parent / f"invalid-timeout-{index}.marker"
        marker.unlink(missing_ok=True)
        command = shell_command(
            f'echo invalid>"{marker}"',
            f"touch '{marker}'",
        )
        result, text = shell_result(
            client,
            {"command": command, "timeout_ms": timeout_value},
        )
        assert result["isError"] is True, (timeout_value, result)
        assert "timeout_ms" in text, (timeout_value, text)
        assert not marker.exists(), marker

    state = client.control(
        update_arguments(
            state,
            overrides={"default_timeout_ms": 5000, "max_timeout_ms": 300000},
        )
    )
    result, text = shell_result(
        client,
        {"command": "echo timeout-hard-max", "timeout_ms": 300000},
    )
    assert result["isError"] is False, result
    assert json.loads(text)["stdout"].strip() == "timeout-hard-max", text

    state = client.control(update_arguments(state, shell_enabled=False))
    marker = config_path.parent / "shell-gate.marker"
    marker.unlink(missing_ok=True)
    command = shell_command(f'echo blocked>"{marker}"', f"touch '{marker}'")
    result, text = shell_result(client, {"command": command})
    assert result["isError"] is True, result
    assert "disabled by policy" in text, text
    assert not marker.exists(), marker

    reset = client.control(
        {"action": "reset", "token": TOKEN, "expected_revision": state["revision"]}
    )
    assert reset["revision"] == state["revision"] + 1, reset
    assert reset["overrides"] == {}, reset
    assert reset["shell_enabled_override"] is None, reset
    assert reset["sandbox_enabled"] is True, reset
    assert reset["shell_enabled"] is False, reset
    stderr = client.close()
    assert "action=update" in stderr and "action=reset" in stderr, stderr
    assert "changed=" in stderr, stderr


def reset_state(client, state):
    return client.control(
        {"action": "reset", "token": TOKEN, "expected_revision": state["revision"]}
    )


def capability_values(value):
    values = []
    if isinstance(value, dict):
        for item in value.values():
            values.extend(capability_values(item))
    elif isinstance(value, str):
        values.append(value)
    return values


def capability_patch(path, value):
    patch = override_patch(path, value)
    if path == ("default_timeout_ms",):
        patch["max_timeout_ms"] = 300000
    elif path == ("max_timeout_ms",):
        patch["default_timeout_ms"] = 1
    elif path == ("capture_stderr",) and value is False:
        patch["merge_stderr"] = False
    return patch


def verify_capabilities(exe, config_path, config):
    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    state = client.get()
    assert state["capabilities"]["shell_enabled"] == "enforced", state
    assert state["capabilities"]["sandbox_enabled"] == "enforced", state

    cases = [
        (("max_command_length",), 4096),
        (("default_timeout_ms",), 10000),
        (("max_timeout_ms",), 10000),
        (("max_output_bytes",), 32768),
        (("capture_stderr",), False),
        (("merge_stderr",), False),
        (("execution", "working_directory"), "capability-cwd"),
        (("execution", "request_cwd_allowed"), False),
        (("execution", "clear_environment"), False),
        (("execution", "allowed_env"), ["PATH"]),
        (("execution", "request_env_allowed"), False),
        (("execution", "kill_process_group_on_timeout"), False),
        (("limits", "max_cpu_seconds"), 10),
        (("limits", "max_memory_bytes"), 1048576),
        (("limits", "max_file_size_bytes"), 1048576),
        (("limits", "max_open_files"), 32),
        (("limits", "max_processes"), 4),
        (("isolation", "require_non_root"), True),
    ]
    for path, value in cases:
        state = client.get()
        status = nested_get(state["capabilities"], path)
        assert status in {"enforced", "unsupported", "ignored", "reject_only"}, (path, state)
        arguments = update_arguments(state, overrides=capability_patch(path, value))
        if status == "unsupported":
            assert_error_unchanged(
                client,
                arguments,
                code="unsupported_on_platform",
                field=".".join(path),
            )
            state = client.get()
        else:
            state = client.control(arguments)
            assert nested_get(state["effective"], path) == value, (path, status, state)

        state = client.control(
            update_arguments(state, overrides=override_patch(path, None))
        )
        assert not nested_has(state["overrides"], path), (path, state)
        state = reset_state(client, state)
        assert state["overrides"] == {}, state

    for path in [("execution", "run_as_user"), ("execution", "run_as_group")]:
        state = client.get()
        status = nested_get(state["capabilities"], path)
        nonempty = update_arguments(state, overrides=override_patch(path, "sandbox-user"))
        if status == "unsupported":
            assert_error_unchanged(
                client,
                nonempty,
                code="unsupported_on_platform",
                field=".".join(path),
            )
            state = client.get()
        else:
            state = client.control(nonempty)
            assert nested_get(state["effective"], path) == "sandbox-user", state
        state = client.control(update_arguments(state, overrides=override_patch(path, "")))
        assert nested_get(state["overrides"], path) == "", state
        state = client.control(update_arguments(state, overrides=override_patch(path, None)))
        assert not nested_has(state["overrides"], path), state
        state = reset_state(client, state)

    state = client.get()
    state = client.control(update_arguments(state, shell_enabled=True))
    assert state["capabilities"]["shell_enabled"] == "enforced", state
    state = client.control(update_arguments(state, sandbox_enabled=False))
    assert state["capabilities"]["sandbox_enabled"] == "enforced", state
    assert state["capabilities"]["max_command_length"] == "ignored", state
    assert nested_get(state["capabilities"], ("isolation", "require_non_root")) in {
        "ignored",
        "reject_only",
    }, state
    statuses = set(capability_values(state["capabilities"]))
    assert "enforced" in statuses and "ignored" in statuses, statuses
    if "reject_only" in statuses:
        assert "unsupported" in statuses, statuses
        assert statuses == {"enforced", "unsupported", "ignored", "reject_only"}, statuses
    state = client.control(update_arguments(state, sandbox_enabled=True))
    assert state["sandbox_enabled"] is True, state

    allowed_env_status = nested_get(
        state["capabilities"], ("execution", "allowed_env")
    )
    if allowed_env_status != "unsupported":
        state = reset_state(client, state)
        state = client.control(
            update_arguments(
                state,
                overrides={"execution": {"clear_environment": False, "allowed_env": []}},
            )
        )
        assert state["effective"]["execution"]["parent_environment_mode"] == "none", state
        state = client.control(
            update_arguments(
                state,
                overrides={"execution": {"allowed_env": ["PATH", "PATH"]}},
            )
        )
        assert state["overrides"]["execution"]["allowed_env"] == ["PATH", "PATH"], state
        assert state["effective"]["execution"]["parent_environment_mode"] == "allowlist", state
    reset_state(client, state)
    client.close()


def verify_reject_only_execution(exe, config_path, config):
    reject_config = copy.deepcopy(config)
    reject_config["enabled"] = True
    reject_config["isolation"]["require_non_root"] = True
    write_config(config_path, reject_config)
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    state = client.get()
    status = nested_get(state["capabilities"], ("isolation", "require_non_root"))
    if status == "reject_only":
        response = client.call("system.shell_exec", {"command": "echo must-not-run"})
        result = response["result"]
        assert result["isError"] is True, result
        assert "low-privilege token" in result["content"][0]["text"], result
    client.close()


def verify_windows_async_unsupported(exe, config_path):
    if os.name != "nt":
        return

    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    marker = config_path.parent / "windows-async-unsupported.marker"
    marker.unlink(missing_ok=True)
    result, text = shell_result(
        client,
        {"command": f'echo must-not-run>"{marker}"'},
    )
    assert result["isError"] is True, result
    assert "not implemented" in text, text
    assert not marker.exists(), marker
    client.close()


def verify_isolated_memory_failure(exe, config_path, config):
    if os.name == "nt":
        return

    try:
        import resource
    except ImportError:
        print("SKIP isolated memory failure: resource module unavailable")
        return
    if not hasattr(resource, "RLIMIT_AS"):
        print("SKIP isolated memory failure: RLIMIT_AS unavailable")
        return

    write_config(config_path, config)
    server_limit = 24 * 1024 * 1024

    def limit_server_memory():
        resource.setrlimit(resource.RLIMIT_AS, (server_limit, server_limit))

    client = Client(
        exe,
        control_env(config_path, gate="1", token=TOKEN),
        preexec_fn=limit_server_memory,
    )
    try:
        state = client.get()
        state = client.control(
            update_arguments(
                state,
                shell_enabled=True,
                overrides={"max_output_bytes": 16 * 1024 * 1024},
            )
        )
        revision = state["revision"]
        result, text = shell_result(
            client,
            {"command": "head -c 16777216 /dev/zero", "timeout_ms": 5000},
        )
        assert result["isError"] is True, result
        assert text, result
        after = client.get()
        assert after["revision"] == revision, after
        assert after["overrides"] == state["overrides"], after
    finally:
        client.close()


def verify_synchronous_bypass(exe, config_path, config):
    write_config(config_path, config)
    env = control_env(config_path, gate="1", token=TOKEN)
    env["MCP_SANDBOX_BYPASS_SENTINEL"] = "bypass-parent-value"
    client = Client(exe, env)

    state = client.get()
    state = client.control(
        update_arguments(
            state,
            shell_enabled=True,
            overrides={"max_command_length": 64},
        )
    )
    restrictive_overrides = copy.deepcopy(state["overrides"])
    long_command = "echo " + "b" * 80
    result, text = shell_result(client, {"command": long_command})
    assert result["isError"] is True and "maximum length" in text, result

    state = client.control(update_arguments(state, sandbox_enabled=False))
    assert state["overrides"] == restrictive_overrides, state
    assert state["shell_enabled"] is True, state
    effective = state["effective"]
    assert effective["shell_enabled"] is True, effective
    assert effective["sandbox_enabled"] is False, effective
    assert effective["max_command_length"] == 65535, effective
    assert effective["default_timeout_ms"] == 300000, effective
    assert effective["max_timeout_ms"] == 300000, effective
    assert effective["max_output_bytes"] == 2147483648, effective
    assert effective["capture_stderr"] is True, effective
    execution = effective["execution"]
    assert Path(execution["working_directory"]).resolve() == Path.cwd().resolve(), execution
    assert execution["request_cwd_allowed"] is True, execution
    assert execution["clear_environment"] is False, execution
    assert execution["allowed_env"] == [], execution
    assert execution["request_env_allowed"] is True, execution
    assert execution["kill_process_group_on_timeout"] is True, execution
    assert execution["run_as_user"] is None and execution["run_as_group"] is None, execution
    if os.name != "nt":
        assert execution["parent_environment_mode"] == "all", execution
    assert effective["limits"] == {
        "max_cpu_seconds": 0,
        "max_memory_bytes": 0,
        "max_file_size_bytes": 0,
        "max_open_files": 0,
        "max_processes": 0,
    }, effective
    assert effective["isolation"]["require_non_root"] is False, effective

    payload = successful_shell_payload(client, {"command": long_command})
    assert payload["sandbox_revision"] == state["revision"], payload
    assert payload["sandbox_enabled"] is False, payload
    assert payload["shell_enabled"] is True, payload

    hard_marker = config_path.parent / "hard-command.marker"
    hard_marker.unlink(missing_ok=True)
    prefix = shell_command(
        f'echo hard>"{hard_marker}"&',
        f"touch '{hard_marker}';",
    )
    oversized_command = prefix + "x" * (65536 - len(prefix))
    assert len(oversized_command.encode("utf-8")) == 65536, len(oversized_command)
    result, text = shell_result(client, {"command": oversized_command})
    assert result["isError"] is True and "maximum length" in text, result
    assert not hard_marker.exists(), hard_marker

    timeout_marker = config_path.parent / "hard-timeout.marker"
    timeout_marker.unlink(missing_ok=True)
    command = shell_command(
        f'echo hard>"{timeout_marker}"',
        f"touch '{timeout_marker}'",
    )
    result, text = shell_result(client, {"command": command, "timeout_ms": 300001})
    assert result["isError"] is True and "timeout_ms" in text, result
    assert not timeout_marker.exists(), timeout_marker

    lifecycle_marker = config_path.parent / "timeout-lifecycle.marker"
    lifecycle_marker.unlink(missing_ok=True)
    command = shell_command(
        f'ping 127.0.0.1 -n 3 >nul & echo late>"{lifecycle_marker}"',
        f"sh -c 'sleep 1; touch \"{lifecycle_marker}\"' & wait",
    )
    result, text = shell_result(client, {"command": command, "timeout_ms": 50})
    assert result["isError"] is True, result
    assert json.loads(text)["timed_out"] is True, text
    time.sleep(2.2 if os.name == "nt" else 1.2)
    marker_created = lifecycle_marker.exists()
    lifecycle_marker.unlink(missing_ok=True)
    assert not marker_created, lifecycle_marker

    assert_error_unchanged(
        client,
        update_arguments(state, overrides={"max_output_bytes": 2147483649}),
        field="max_output_bytes",
    )

    state = client.get()
    state = client.control(update_arguments(state, sandbox_enabled=True))
    assert state["overrides"] == restrictive_overrides, state
    result, text = shell_result(client, {"command": long_command})
    assert result["isError"] is True and "maximum length" in text, result

    execution_overrides = {
        "working_directory": str(config_path.parent),
        "clear_environment": os.name == "nt",
    }
    if os.name != "nt":
        execution_overrides["allowed_env"] = []
    state = client.control(
        update_arguments(
            state,
            overrides={
                "max_command_length": 65535,
                "max_output_bytes": 256,
                "capture_stderr": False,
                "merge_stderr": False,
                "execution": execution_overrides,
            },
        )
    )
    output_command = shell_command(
        "echo " + "x" * 300,
        "printf '%s' '" + "x" * 300 + "'",
    )
    cwd_command = shell_command("cd", "pwd")
    env_command = shell_command(
        "if defined MCP_SANDBOX_BYPASS_SENTINEL (echo %MCP_SANDBOX_BYPASS_SENTINEL%) else (echo unset)",
        "printf '%s' \"${MCP_SANDBOX_BYPASS_SENTINEL-unset}\"",
    )
    stderr_command = shell_command(
        "echo bypass-stderr 1>&2",
        "printf '%s' bypass-stderr >&2",
    )

    normal_output = successful_shell_payload(client, {"command": output_command})
    assert len(normal_output["stdout"]) == 256 and normal_output["truncated"] is True, normal_output
    normal_cwd = successful_shell_payload(client, {"command": cwd_command})
    assert Path(normal_cwd["stdout"].strip()).resolve() == config_path.parent.resolve(), normal_cwd
    normal_env = successful_shell_payload(client, {"command": env_command})
    assert normal_env["stdout"].strip() == "unset", normal_env
    normal_stderr = successful_shell_payload(client, {"command": stderr_command})
    assert normal_stderr["stderr"] == "", normal_stderr

    saved_overrides = copy.deepcopy(state["overrides"])
    state = client.control(update_arguments(state, sandbox_enabled=False))
    assert state["overrides"] == saved_overrides, state
    bypass_output = successful_shell_payload(client, {"command": output_command})
    assert len(bypass_output["stdout"].strip()) == 300 and bypass_output["truncated"] is False, bypass_output
    bypass_cwd = successful_shell_payload(client, {"command": cwd_command})
    assert Path(bypass_cwd["stdout"].strip()).resolve() == Path.cwd().resolve(), bypass_cwd
    bypass_env = successful_shell_payload(client, {"command": env_command})
    assert bypass_env["stdout"].strip() == "bypass-parent-value", bypass_env
    bypass_stderr = successful_shell_payload(client, {"command": stderr_command})
    assert bypass_stderr["stderr"].strip() == "bypass-stderr", bypass_stderr

    state = client.control(update_arguments(state, sandbox_enabled=True))
    assert state["overrides"] == saved_overrides, state
    restored_output = successful_shell_payload(client, {"command": output_command})
    assert len(restored_output["stdout"]) == 256 and restored_output["truncated"] is True, restored_output
    restored_env = successful_shell_payload(client, {"command": env_command})
    assert restored_env["stdout"].strip() == "unset", restored_env
    restored_stderr = successful_shell_payload(client, {"command": stderr_command})
    assert restored_stderr["stderr"] == "", restored_stderr

    state = client.control(
        update_arguments(
            state,
            overrides={"capture_stderr": True, "merge_stderr": True},
        )
    )
    merged = successful_shell_payload(client, {"command": stderr_command})
    assert merged["stdout"].strip() == "bypass-stderr" and merged["stderr"] == "", merged

    require_non_root = nested_get(
        state["capabilities"], ("isolation", "require_non_root")
    )
    if require_non_root == "reject_only":
        state = client.control(
            update_arguments(state, overrides={"isolation": {"require_non_root": True}})
        )
        result, text = shell_result(client, {"command": "echo reject-only"})
        assert result["isError"] is True and "low-privilege token" in text, result
        state = client.control(update_arguments(state, sandbox_enabled=False))
        assert nested_get(state["capabilities"], ("isolation", "require_non_root")) == "reject_only", state
        payload = successful_shell_payload(client, {"command": "echo reject-only-bypassed"})
        assert payload["stdout"].strip() == "reject-only-bypassed", payload
        state = client.control(update_arguments(state, sandbox_enabled=True))
        result, text = shell_result(client, {"command": "echo reject-only-restored"})
        assert result["isError"] is True and "low-privilege token" in text, result

    reset = reset_state(client, state)
    assert reset["overrides"] == {}, reset
    assert reset["sandbox_enabled"] is True, reset
    result, text = shell_result(client, {"command": "echo reset-must-use-base"})
    assert result["isError"] is True and "disabled by policy" in text, result
    client.close()


def verify_async_effective_policy(exe, config_path, config):
    if os.name == "nt":
        return

    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    state = client.get()
    state = client.control(
        update_arguments(
            state,
            shell_enabled=True,
            overrides={
                "default_timeout_ms": 100,
                "max_timeout_ms": 100,
                "max_output_bytes": 1024,
                "execution": {
                    "request_cwd_allowed": False,
                    "request_env_allowed": False,
                },
            },
        )
    )

    assert_job_error(
        client,
        "system.shell_start",
        {"command": "pwd", "cwd": str(config_path.parent)},
        "cwd is disabled",
    )
    assert_job_error(
        client,
        "system.shell_start",
        {"command": "true", "env": {"MCP_ASYNC_VALUE": "blocked"}},
        "env is disabled",
    )

    started = successful_job_payload(
        client,
        "system.shell_start",
        {"command": "printf 'x%.0s' $(seq 1 300)", "output_limit_bytes": 256},
    )
    assert started["sandbox_revision"] == state["revision"], started
    assert started["sandbox_enabled"] is True and started["shell_enabled"] is True, started
    waited = successful_job_payload(
        client, "system.shell_wait", {"job_id": started["job_id"], "timeout_ms": 3000}
    )
    tailed = successful_job_payload(client, "system.shell_tail", {"job_id": started["job_id"]})
    listed = successful_job_payload(client, "system.shell_list", {})
    for payload in (waited, tailed, next(job for job in listed["jobs"] if job["job_id"] == started["job_id"])):
        assert payload["sandbox_revision"] == state["revision"], payload
        assert payload["sandbox_enabled"] is True and payload["shell_enabled"] is True, payload
    assert tailed["stdout"] == "x" * 256 and tailed["stdout_truncated"] is True, tailed

    marker = config_path.parent / "async-timeout.marker"
    marker.unlink(missing_ok=True)
    timed = successful_job_payload(
        client,
        "system.shell_start",
        {"command": f"sh -c 'sleep 0.3; touch {marker}'"},
    )
    timed_wait = successful_job_payload(
        client, "system.shell_wait", {"job_id": timed["job_id"], "timeout_ms": 3000}
    )
    assert timed_wait["state"] == "timed_out", timed_wait
    time.sleep(0.4)
    assert not marker.exists(), marker
    marker.unlink(missing_ok=True)

    reset_state(client, state)
    client.close()


def verify_async_job_snapshot_boundary(exe, config_path, config):
    if os.name == "nt":
        return

    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    state = client.get()
    state = client.control(
        update_arguments(
            state,
            shell_enabled=True,
            overrides={
                "default_timeout_ms": 1000,
                "max_timeout_ms": 1000,
                "max_output_bytes": 1024,
            },
        )
    )
    revision_a = state["revision"]
    started_a = successful_job_payload(
        client,
        "system.shell_start",
        {"command": "sh -c 'printf first; sleep 0.4; printf second'"},
    )
    assert started_a["sandbox_revision"] == revision_a, started_a

    state = client.control(
        update_arguments(
            state,
            overrides={
                "default_timeout_ms": 50,
                "max_timeout_ms": 50,
                "max_output_bytes": 256,
            },
        )
    )
    revision_b = state["revision"]
    assert revision_b == revision_a + 1, state

    poll_a = successful_job_payload(client, "system.shell_poll", {"job_id": started_a["job_id"]})
    tail_a = successful_job_payload(client, "system.shell_tail", {"job_id": started_a["job_id"]})
    wait_a = successful_job_payload(
        client, "system.shell_wait", {"job_id": started_a["job_id"], "timeout_ms": 3000}
    )
    listed_a = successful_job_payload(client, "system.shell_list", {})
    for payload in (poll_a, tail_a, wait_a, next(job for job in listed_a["jobs"] if job["job_id"] == started_a["job_id"])):
        assert payload["sandbox_revision"] == revision_a, payload
        assert payload["sandbox_enabled"] is True and payload["shell_enabled"] is True, payload
    final_a = successful_job_payload(client, "system.shell_tail", {"job_id": started_a["job_id"]})
    assert final_a["state"] == "exited" and final_a["stdout"] == "firstsecond", final_a
    assert final_a["stdout_truncated"] is False, final_a

    marker_b = config_path.parent / "async-job-b.marker"
    marker_b.unlink(missing_ok=True)
    started_b = successful_job_payload(
        client,
        "system.shell_start",
        {"command": f"sh -c 'sleep 0.2; touch {marker_b}'"},
    )
    assert started_b["sandbox_revision"] == revision_b, started_b
    wait_b = successful_job_payload(
        client, "system.shell_wait", {"job_id": started_b["job_id"], "timeout_ms": 3000}
    )
    assert wait_b["state"] == "timed_out", wait_b
    time.sleep(0.3)
    assert not marker_b.exists(), marker_b
    marker_b.unlink(missing_ok=True)

    state = client.control(update_arguments(state, shell_enabled=False))
    marker_c = config_path.parent / "async-job-c.marker"
    marker_c.unlink(missing_ok=True)
    assert_job_error(
        client,
        "system.shell_start",
        {"command": f"touch {marker_c}"},
        "disabled by policy",
    )
    assert not marker_c.exists(), marker_c
    marker_c.unlink(missing_ok=True)
    for job_id, revision in ((started_a["job_id"], revision_a), (started_b["job_id"], revision_b)):
        payload = successful_job_payload(client, "system.shell_poll", {"job_id": job_id})
        assert payload["sandbox_revision"] == revision, payload

    reset_state(client, state)
    client.close()


def verify_restart_clears_state(exe, config_path, config):
    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    state = client.get()
    state = client.control(
        update_arguments(
            state,
            shell_enabled=True,
            overrides={"max_command_length": 4096},
        )
    )
    assert state["revision"] == 1 and state["overrides"], state
    client.close()

    restarted = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    state = restarted.get()
    assert state["revision"] == 0, state
    assert state["overrides"] == {}, state
    assert state["shell_enabled_override"] is None, state
    assert state["sandbox_enabled"] is True, state
    restarted.close()


def verify_base_drift_and_conflict(exe, config_path, config):
    write_config(config_path, config)
    client = Client(exe, control_env(config_path, gate="1", token=TOKEN))
    initial = client.get()

    drifted = copy.deepcopy(config)
    drifted["max_command_length"] = 4096
    write_config(config_path, drifted)
    after_drift = client.get()
    assert after_drift["revision"] == initial["revision"], (initial, after_drift)
    assert after_drift["base"]["max_command_length"] == 4096, after_drift
    assert after_drift["effective"]["max_command_length"] == 4096, after_drift

    state = client.control(
        update_arguments(after_drift, overrides={"default_timeout_ms": 20000})
    )
    assert state["policy_valid"] is True, state
    saved_revision = state["revision"]
    saved_overrides = copy.deepcopy(state["overrides"])

    conflicting = copy.deepcopy(drifted)
    conflicting["default_timeout_ms"] = 500
    conflicting["max_timeout_ms"] = 1000
    write_config(config_path, conflicting)
    conflicted = client.get()
    assert conflicted["revision"] == saved_revision, conflicted
    assert conflicted["base"]["max_timeout_ms"] == 1000, conflicted
    assert conflicted["overrides"] == saved_overrides, conflicted
    assert conflicted["policy_valid"] is False, conflicted
    assert conflicted["effective"] is None, conflicted

    assert_error_unchanged(
        client,
        update_arguments(conflicted, shell_enabled=True),
        code="config_unavailable",
    )
    assert_error_unchanged(
        client,
        {"action": "reset", "token": TOKEN, "expected_revision": conflicted["revision"]},
        code="config_unavailable",
    )

    restored = copy.deepcopy(drifted)
    restored["default_timeout_ms"] = 500
    restored["max_timeout_ms"] = 30000
    write_config(config_path, restored)
    recovered = client.get()
    assert recovered["revision"] == saved_revision, recovered
    assert recovered["policy_valid"] is True, recovered
    reset = reset_state(client, recovered)
    assert reset["overrides"] == {}, reset
    client.close()


def main():
    exe = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="mcp-sandbox-ctl-") as temp_dir:
        temp_path = Path(temp_dir)
        config_path = temp_path / "shell_exec.json"
        config = valid_config(temp_path)
        write_config(config_path, config)
        verify_descriptor_and_auth(exe, config_path)
        verify_snapshot_token_auth(exe, config_path, copy.deepcopy(config))
        verify_v2_control_state(exe, config_path, copy.deepcopy(config))
        verify_field_fallback_diagnostics(exe, config_path, copy.deepcopy(config))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
