import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


TOKEN = "s2-token-must-not-appear-7f51b36e"


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
        "enabled": False,
        "max_command_length": 3500,
        "default_timeout_ms": 5000,
        "max_timeout_ms": 30000,
        "max_output_bytes": 16384,
        "chunk_size": 1024,
        "capture_stderr": True,
        "merge_stderr": False,
        "execution": {
            "mode": "shell",
            "working_directory": str(working_directory),
            "request_cwd_allowed": True,
            "clear_environment": True,
            "allowed_env": ["PATH"],
            "request_env_allowed": True,
            "run_as_user": "",
            "run_as_group": "",
            "kill_process_group_on_timeout": True,
        },
        "limits": {
            "max_cpu_seconds": 5,
            "max_memory_bytes": 134217728,
            "max_file_size_bytes": 10485760,
            "max_open_files": 64,
            "max_processes": 16,
        },
        "isolation": {"require_non_root": False},
    }


def write_config(path, value):
    path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")


class Client:
    def __init__(self, exe, env):
        self.stderr_file = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
        self.proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
            env=env,
            text=True,
            encoding="utf-8",
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
    max_output_options = schema["properties"]["overrides"]["properties"]["max_output_bytes"][
        "anyOf"
    ]
    max_output_integer = next(item for item in max_output_options if item["type"] == "integer")
    assert max_output_integer["minimum"] == 256, max_output_integer
    assert max_output_integer["maximum"] == 2147483648, max_output_integer

    error = client.control({"action": "get", "token": TOKEN}, expect_error=True)
    assert error == {
        "code": "unauthorized",
        "message": "sandbox control authentication failed",
    }, error
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
    shell_response = client.call("system.shell_exec", {"command": "echo must-not-run"})
    shell_result = shell_response["result"]
    assert shell_result["isError"] is True, shell_result
    assert "disabled by policy" in shell_result["content"][0]["text"], shell_result

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
        verify_invalid_config(exe, config_path)
        verify_control_contract(exe, config_path, copy.deepcopy(config))
        verify_capabilities(exe, config_path, copy.deepcopy(config))
        verify_reject_only_execution(exe, config_path, copy.deepcopy(config))
        verify_restart_clears_state(exe, config_path, copy.deepcopy(config))
        verify_base_drift_and_conflict(exe, config_path, copy.deepcopy(config))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
