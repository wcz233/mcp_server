import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


TOKEN = "startup-token-must-not-appear-4e91a7c2"


def clean_env():
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("MCP_SHELL_") or key in {
            "MCP_ENABLE_SANDBOX_CTL",
            "MCP_ENABLE_SHELL_EXEC",
            "MCP_SANDBOX_CTL_TOKEN",
        }:
            env.pop(key, None)
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
    if isinstance(value, str):
        path.write_text(value, encoding="utf-8")
    else:
        path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")


def run_server(exe, env, expected_returncode, secrets=()):
    proc = subprocess.Popen(
        [exe],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )
    stdout, stderr = proc.communicate("", timeout=5)
    assert proc.returncode == expected_returncode, (proc.returncode, stdout, stderr)
    assert "uv_loop_close" not in stderr, stderr
    for secret in secrets:
        assert secret not in stdout, stdout
        assert secret not in stderr, stderr
    return stdout, stderr


def startup_env(config_path=None, gate=None, legacy_token=None):
    env = clean_env()
    if config_path is not None:
        env["MCP_SHELL_EXEC_CONFIG"] = str(config_path)
    if gate is not None:
        env["MCP_ENABLE_SANDBOX_CTL"] = gate
    if legacy_token is not None:
        env["MCP_SANDBOX_CTL_TOKEN"] = legacy_token
    return env


def verify_policy_startup_matrix(exe, temp_path):
    config_path = temp_path / "shell_exec-v2.json"
    missing_path = temp_path / "missing.json"
    config = valid_config(temp_path)

    write_config(config_path, config)
    run_server(exe, startup_env(config_path, gate="1"), 0, (TOKEN,))
    run_server(exe, startup_env(config_path, gate="0"), 0, (TOKEN,))
    run_server(exe, startup_env(), 0)
    run_server(exe, startup_env(config_path, gate="invalid"), 1, (TOKEN,))
    run_server(exe, startup_env(missing_path, gate="1"), 1)

    invalid_configs = []
    invalid_configs.append("{invalid")

    value = copy.deepcopy(config)
    value["version"] = 3
    invalid_configs.append(value)

    value = copy.deepcopy(config)
    value["unknown"] = True
    invalid_configs.append(value)

    value = copy.deepcopy(config)
    value["defaults"]["execution"]["unknown"] = True
    invalid_configs.append(value)

    value = copy.deepcopy(config)
    del value["control"]["token"]
    invalid_configs.append(value)

    value = copy.deepcopy(config)
    value["control"]["token"] = ""
    invalid_configs.append(value)

    value = copy.deepcopy(config)
    value["control"]["token"] = "embedded\x00token"
    invalid_configs.append(value)

    value = copy.deepcopy(config)
    value["control"]["token"] = "x" * 4097
    invalid_configs.append(value)

    value = copy.deepcopy(config)
    value["defaults"]["timeout_ms"] = "300000"
    invalid_configs.append(value)

    serialized = json.dumps(config, separators=(",", ":"))
    invalid_configs.append(serialized.replace('{"version":2,', '{"version":2,"version":2,', 1))

    for invalid in invalid_configs:
        write_config(config_path, invalid)
        run_server(exe, startup_env(config_path, gate="1"), 1, (TOKEN,))

    value = copy.deepcopy(config)
    del value["control"]["token"]
    write_config(config_path, value)
    run_server(
        exe,
        startup_env(config_path, gate="1", legacy_token=TOKEN),
        1,
        (TOKEN,),
    )

    write_config(config_path, "{invalid")
    run_server(exe, startup_env(config_path, gate="0"), 1)

    value = copy.deepcopy(config)
    value["bounds"]["limits"]["memory_bytes"]["max"] = 34359738368
    write_config(config_path, value)
    run_server(exe, startup_env(config_path, gate="1"), 0, (TOKEN,))

    value = copy.deepcopy(config)
    value["defaults"]["command_length"] = 0
    value["defaults"]["limits"]["memory_bytes"] = 34359738367
    write_config(config_path, value)
    run_server(exe, startup_env(config_path, gate="1"), 0, (TOKEN,))


def verify_transport_failure_cleanup(exe):
    env = clean_env()
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "not-a-valid-host"
    env["MCP_TCP_PORT"] = "18770"

    stdout, stderr = run_server(exe, env, 1)
    assert "mcp_server_start_tcp failed" in stderr, (stdout, stderr)


def main():
    exe = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="mcp-sandbox-startup-") as temp_dir:
        verify_policy_startup_matrix(exe, Path(temp_dir))
    verify_transport_failure_cleanup(exe)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
