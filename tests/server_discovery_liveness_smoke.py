import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def encode(payload):
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(data)) + data


def recv_payload(conn, timeout=5.0):
    conn.settimeout(timeout)
    header = conn.recv(4)
    if len(header) != 4:
        raise RuntimeError("short frame header")
    (length,) = struct.unpack(">I", header)
    data = bytearray()
    while len(data) < length:
        chunk = conn.recv(length - len(data))
        if not chunk:
            raise RuntimeError("short frame body")
        data.extend(chunk)
    return bytes(data)


def recv_frame(conn, timeout=5.0):
    return json.loads(recv_payload(conn, timeout).decode("utf-8"))


def send_frame(conn, payload):
    conn.sendall(encode(payload))


def send_payload(conn, payload):
    conn.sendall(struct.pack(">I", len(payload)) + payload)


def mft_json_frame(frame_type, payload):
    encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return b"MFT1" + bytes((1, frame_type, 0, 0)) + encoded


def call(sock, request_id, method, params=None):
    payload = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        payload["params"] = params
    sock.sendall(encode(payload))
    response = recv_frame(sock)
    assert response["id"] == request_id, response
    return response


def call_tool_result(sock, request_id, name, arguments=None):
    response = call(
        sock,
        request_id,
        "tools/call",
        {"name": name, "arguments": arguments or {}},
    )
    result = response["result"]
    assert result["content"][0]["type"] == "text", result
    return result


def call_tool(sock, request_id, name, arguments=None):
    result = call_tool_result(sock, request_id, name, arguments)
    assert result["isError"] is False, result
    return json.loads(result["content"][0]["text"])


def wait_for_tcp(port, proc):
    deadline = time.time() + 5
    last_error = None
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early; stderr={proc.stderr.read()}")
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"tcp listener did not become ready: {last_error}")


def start_server(exe, tcp_port, discovery_port, peer_discovery_port):
    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(tcp_port)
    env["MCP_ENABLE_DISCOVERY"] = "1"
    env["MCP_DISCOVERY_BIND_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_PORT"] = str(discovery_port)
    env["MCP_DISCOVERY_ADVERTISE_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_HOSTS"] = f"127.0.0.1:{peer_discovery_port}"
    env["MCP_DISCOVERY_PROXY_TIMEOUT_MS"] = "3000"
    env["MCP_STRICT_INIT"] = "0"
    env["MCP_ENABLE_SHELL_EXEC"] = "1"
    env["MCP_SHELL_EXEC_CONFIG"] = os.path.join(
        os.path.dirname(__file__), "shell_exec_test_config.json"
    )
    return subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )


def start_broadcast_port_server(exe, tcp_port, discovery_port, broadcast_port):
    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(tcp_port)
    env["MCP_ENABLE_DISCOVERY"] = "1"
    env["MCP_DISCOVERY_BIND_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_PORT"] = str(discovery_port)
    env["MCP_UDP_BROADCAST_LISTEN_PORT"] = str(broadcast_port)
    env["MCP_DISCOVERY_ADVERTISE_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_HOSTS"] = "127.0.0.1"
    env["MCP_STRICT_INIT"] = "0"
    env["MCP_ENABLE_SHELL_EXEC"] = "0"
    return subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )


def start_default_broadcast_port_server(exe, tcp_port, discovery_port):
    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_LISTEN_PORT"] = str(tcp_port)
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_ENABLE_DISCOVERY"] = "1"
    env["MCP_DISCOVERY_BIND_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_PORT"] = str(discovery_port)
    env["MCP_DISCOVERY_ADVERTISE_HOST"] = "127.0.0.1"
    env["MCP_DISCOVERY_HOSTS"] = "127.0.0.1"
    env["MCP_STRICT_INIT"] = "0"
    env["MCP_ENABLE_SHELL_EXEC"] = "0"
    return subprocess.Popen(
        [exe],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )


def initialize(sock):
    init = call(
        sock,
        1,
        "initialize",
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "server-discovery-liveness-smoke", "version": "0.1"},
        },
    )
    assert init["result"]["serverInfo"]["name"] == "mcp_server", init


def udp_packet(instance_id, tcp_port, event="online"):
    return json.dumps(
        {
            "mcp_server_discovery": 1,
            "instance_id": instance_id,
            "tcp_port": tcp_port,
            "reply": False,
            "event": event,
            "advertise_host": "127.0.0.1",
            "status": {"hostname": instance_id, "os": "fake"},
        },
        separators=(",", ":"),
    ).encode("utf-8")


def wait_for_udp(sock, deadline):
    while time.time() < deadline:
        try:
            data, _ = sock.recvfrom(65536)
        except socket.timeout:
            continue
        payload = json.loads(data.decode("utf-8"))
        if payload.get("mcp_server_discovery") == 1:
            return payload
    raise AssertionError("missing discovery UDP packet")


class FakePeer:
    def __init__(
        self,
        tcp_port,
        respond_to_ping,
        support_data_channel=False,
        duplicate_initialize_response=False,
        delay_tools_list=False,
        malformed_tools_list=False,
    ):
        self.tcp_port = tcp_port
        self.respond_to_ping = respond_to_ping
        self.support_data_channel = support_data_channel
        self.duplicate_initialize_response = duplicate_initialize_response
        self.delay_tools_list = delay_tools_list
        self.malformed_tools_list = malformed_tools_list
        self.heartbeats = []
        self.tools_list_requests = 0
        self.tools_call_requests = 0
        self.data_connections = 0
        self.control_data_frames = 0
        self.hello_frames = 0
        self.protocol_events = []
        self.hello_seen = threading.Event()
        self.tools_list_seen = threading.Event()
        self.data_link_failed = threading.Event()
        self._lock = threading.Lock()
        self._send_lock = threading.Lock()
        self._pending_tools_list = []
        self._stop = threading.Event()
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", tcp_port))
        self._listener.listen(8)
        self._listener.settimeout(0.2)
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()

    def close(self):
        self._stop.set()
        try:
            with socket.create_connection(("127.0.0.1", self.tcp_port), timeout=0.2):
                pass
        except OSError:
            pass
        self._thread.join(timeout=2)
        self._listener.close()

    def assert_initialized_before_hello(self):
        with self._lock:
            events = list(self.protocol_events)
            hello_frames = self.hello_frames
        assert events[:3] == ["initialize", "initialized", "hello"], events
        assert hello_frames == 1, (hello_frames, events)

    def complete_tools_list(self):
        with self._lock:
            pending = list(self._pending_tools_list)
            self._pending_tools_list.clear()
        for conn, request_id in pending:
            self._send_tools_list(conn, request_id)
        return len(pending)

    def set_tools_list_behavior(self, *, delay, malformed):
        with self._lock:
            self.delay_tools_list = delay
            self.malformed_tools_list = malformed

    def set_respond_to_ping(self, respond):
        with self._lock:
            self.respond_to_ping = respond

    def _send_frame(self, conn, payload):
        with self._send_lock:
            send_frame(conn, payload)

    def _send_tools_list(self, conn, request_id):
        with self._lock:
            malformed = self.malformed_tools_list
        result = {"unexpected": []} if malformed else {
            "tools": [
                {
                    "name": "system.ping",
                    "description": "Return pong.",
                    "inputSchema": {"type": "object", "additionalProperties": False},
                }
            ]
        }
        self._send_frame(
            conn,
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "result": result,
            },
        )

    def _run(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        data_channel = False
        with conn:
            while not self._stop.is_set():
                try:
                    payload = recv_payload(conn, timeout=2.0)
                except socket.timeout:
                    continue
                except (OSError, RuntimeError):
                    return
                if payload.startswith(b"MFT1"):
                    if len(payload) < 8:
                        continue
                    frame_type = payload[5]
                    if frame_type == 1:
                        hello = json.loads(payload[8:].decode("utf-8"))
                        with self._lock:
                            self.hello_frames += 1
                            self.protocol_events.append("hello")
                        if self.support_data_channel:
                            assert "mft.v1.data_channel" in hello.get("capabilities", []), hello
                            self.hello_seen.set()
                            send_payload(
                                conn,
                                mft_json_frame(
                                    1,
                                    {
                                        "version": 1,
                                        "capabilities": [
                                            "mft.v1.block_ack",
                                            "mft.v1.crc32",
                                            "mft.v1.data_channel",
                                        ],
                                    },
                                ),
                            )
                    elif frame_type == 2 and self.support_data_channel:
                        offer = json.loads(payload[8:].decode("utf-8"))
                        accept = [
                            {
                                "decision": (
                                    "create_directory"
                                    if entry["type"] == "directory"
                                    else "receive"
                                ),
                                "resume_offset": 0,
                            }
                            for entry in offer["manifest"]["entries"]
                        ]
                        send_payload(
                            conn,
                            mft_json_frame(
                                4,
                                {"transfer_id": offer["transfer_id"], "accept": accept},
                            ),
                        )
                    elif frame_type == 5:
                        if not data_channel:
                            self.control_data_frames += 1
                        else:
                            try:
                                conn.shutdown(socket.SHUT_RDWR)
                            except OSError:
                                pass
                            self.data_link_failed.set()
                            return
                    continue
                if not payload.startswith(b"{"):
                    continue
                request = json.loads(payload.decode("utf-8"))
                if request.get("method") == "initialize":
                    identity = request.get("params", {}).get("mcp_peer_identity", {})
                    data_channel = identity.get("data_channel") is True
                    if not data_channel:
                        with self._lock:
                            self.protocol_events.append("initialize")
                    if data_channel:
                        with self._lock:
                            self.data_connections += 1
                            data_index = self.data_connections
                        if data_index > 1:
                            continue
                    response = {
                        "jsonrpc": "2.0",
                        "id": request["id"],
                        "result": {},
                    }
                    self._send_frame(conn, response)
                    if not data_channel:
                        with self._lock:
                            self.protocol_events.append("initialized")
                        if self.duplicate_initialize_response:
                            self._send_frame(conn, response)
                    continue
                if request.get("method") == "tools/list":
                    with self._lock:
                        self.tools_list_requests += 1
                        self.tools_list_seen.set()
                        delay_tools_list = self.delay_tools_list
                        if delay_tools_list:
                            self._pending_tools_list.append((conn, request["id"]))
                    if not delay_tools_list:
                        self._send_tools_list(conn, request["id"])
                    continue
                if request.get("method") == "tools/call":
                    params = request.get("params", {})
                    if params.get("name") != "system.ping":
                        continue
                    with self._lock:
                        self.tools_call_requests += 1
                    self._send_frame(
                        conn,
                        {
                            "jsonrpc": "2.0",
                            "id": request["id"],
                            "result": {
                                "content": [{"type": "text", "text": "pong"}],
                                "isError": False,
                            },
                        },
                    )
                    continue
                if request.get("method") != "ping":
                    continue
                with self._lock:
                    self.heartbeats.append(time.time())
                    respond_to_ping = self.respond_to_ping
                if respond_to_ping:
                    self._send_frame(
                        conn,
                        {
                            "jsonrpc": "2.0",
                            "id": request["id"],
                            "result": {},
                        },
                    )


def peer_entry(payload, port):
    return next(
        (server for server in payload["servers"] if server["address"] == f"127.0.0.1:{port}"),
        None,
    )


def wait_for_online_peer(sock, tcp_port, start_id):
    request_id = start_id
    deadline = time.time() + 8
    last_payload = None
    while time.time() < deadline:
        last_payload = call_tool(sock, request_id, "server.list_servers", {"wait_ms": 100})
        request_id += 1
        peer = peer_entry(last_payload, tcp_port)
        if peer and peer["state"] == "online" and peer["tcp_connected"]:
            return peer, request_id
        time.sleep(0.1)
    raise AssertionError(f"peer did not become online: {last_payload}")


def proxy_tool_result(sock, request_id, server_id, tool_name, proxy_timeout_ms=None):
    arguments = {"server_id": server_id, "tool_name": tool_name, "args": {}}
    if proxy_timeout_ms is not None:
        arguments["proxy_timeout_ms"] = proxy_timeout_ms
    return call_tool_result(
        sock,
        request_id,
        "gateway.proxy_tool",
        arguments,
    )


def proxy_ping(sock, request_id, server_id):
    result = proxy_tool_result(sock, request_id, server_id, "system.ping")
    assert result["isError"] is False, result
    assert result["content"][0]["text"] == "pong", result


def verify_automatic_tool_discovery(tcp_server, fake_peer, server_id):
    callers = []
    threads = []
    outcomes = []
    failures = []
    try:
        for _ in range(2):
            caller = socket.create_connection(("127.0.0.1", tcp_server), timeout=1.0)
            caller.settimeout(5.0)
            initialize(caller)
            callers.append(caller)

        def invoke(caller):
            try:
                proxy_ping(caller, 2, server_id)
                outcomes.append(None)
            except Exception as exc:
                outcomes.append(exc)

        for caller in callers:
            thread = threading.Thread(target=invoke, args=(caller,), daemon=True)
            thread.start()
            threads.append(thread)

        time.sleep(0.2)
        if not all(thread.is_alive() for thread in threads):
            failures.append(f"proxy calls were not queued during refresh: {outcomes}")
        if fake_peer.tools_list_requests != 1:
            failures.append(
                f"expected one tools/list request, got {fake_peer.tools_list_requests}"
            )
        completed_refreshes = fake_peer.complete_tools_list()
        if completed_refreshes != 1:
            failures.append(f"expected one pending refresh, got {completed_refreshes}")

        for thread in threads:
            thread.join(timeout=5)
        if not all(not thread.is_alive() for thread in threads):
            failures.append(f"queued proxy calls did not complete: {outcomes}")
        if outcomes != [None, None]:
            failures.append(f"queued proxy calls failed: {outcomes}")
        if fake_peer.tools_call_requests != 2:
            failures.append(
                f"expected two remote tools/call requests, got {fake_peer.tools_call_requests}"
            )
        if fake_peer.tools_list_requests != 1:
            failures.append(
                f"tools/list request was not coalesced: {fake_peer.tools_list_requests}"
            )
    finally:
        for caller in callers:
            caller.close()
    return failures


def blocking_command():
    if os.name == "nt":
        ping = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "ping.exe")
        return f"{ping} -n 5 127.0.0.1 > NUL"
    return "sleep 4"


def verify_nonblocking_shell_wait(
    exe,
    client_sock,
    tcp_server,
    discovery_server,
    observer_tcp,
    observer_discovery,
):
    observer_proc = start_server(exe, observer_tcp, observer_discovery, discovery_server)
    observer_sock = None
    previous_timeout = client_sock.gettimeout()
    try:
        observer_sock = wait_for_tcp(observer_tcp, observer_proc)
        observer_sock.settimeout(8.0)
        initialize(observer_sock)
        observed_server, observer_request_id = wait_for_online_peer(
            observer_sock, tcp_server, 2
        )

        started = call_tool(
            client_sock,
            20,
            "system.shell_start",
            {"command": blocking_command(), "timeout_ms": 5000},
        )
        ping_result = {}

        def ping_during_wait():
            try:
                time.sleep(0.25)
                proxy_ping(
                    observer_sock,
                    observer_request_id,
                    observed_server["server_id"],
                )
                ping_result["ok"] = True
            except Exception as exc:
                ping_result["error"] = exc

        ping_thread = threading.Thread(target=ping_during_wait, daemon=True)
        ping_thread.start()
        client_sock.settimeout(8.0)
        wait_started = time.monotonic()
        waited = call_tool(
            client_sock,
            21,
            "system.shell_wait",
            {"job_id": started["job_id"], "timeout_ms": 5000},
        )
        wait_elapsed = time.monotonic() - wait_started
        assert wait_elapsed < 1.0, (wait_elapsed, waited)
        assert waited["wait_result"] == "still_running", waited
        ping_thread.join(timeout=3)
        assert not ping_thread.is_alive(), ping_result
        assert ping_result.get("ok") is True, ping_result

        request_id = 22
        deadline = time.time() + 6
        while time.time() < deadline:
            job = call_tool(
                client_sock,
                request_id,
                "system.shell_poll",
                {"job_id": started["job_id"]},
            )
            request_id += 1
            if job["state"] == "exited":
                break
            time.sleep(0.1)
        assert job["state"] == "exited" and job["exit_code"] == 0, job
        observed_after, _ = wait_for_online_peer(
            observer_sock, tcp_server, observer_request_id + 1
        )
        assert observed_after["server_id"] == observed_server["server_id"], observed_after
    finally:
        client_sock.settimeout(previous_timeout)
        if observer_sock:
            observer_sock.close()
        observer_proc.terminate()
        try:
            observer_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            observer_proc.kill()
            observer_proc.wait(timeout=5)


def main():
    exe = sys.argv[1]
    tcp_server = int(sys.argv[2])
    discovery_server = int(sys.argv[3])
    fake_tcp_online = int(sys.argv[4])
    fake_tcp_timeout = int(sys.argv[5])
    fake_discovery = int(sys.argv[6])
    broadcast_tcp = int(sys.argv[7])
    broadcast_discovery = int(sys.argv[8])
    broadcast_target = int(sys.argv[9])
    file_transfer_mode = sys.argv[10]
    file_transfer_plugin = sys.argv[11]

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_sock.bind(("127.0.0.1", fake_discovery))
    udp_sock.settimeout(0.5)

    online_peer = FakePeer(
        fake_tcp_online,
        respond_to_ping=True,
        support_data_channel=file_transfer_mode != "n",
        duplicate_initialize_response=True,
        delay_tools_list=True,
    )
    timeout_peer = FakePeer(
        fake_tcp_timeout,
        respond_to_ping=True,
        delay_tools_list=True,
    )
    online_peer.start()
    timeout_peer.start()

    proc = start_server(exe, tcp_server, discovery_server, fake_discovery)
    broadcast_proc = None
    sock = None
    transfer_sock = None
    transfer_path = None
    contract_failures = []
    try:
        sock = wait_for_tcp(tcp_server, proc)
        sock.settimeout(3.0)
        initialize(sock)
        if file_transfer_mode == "m":
            loaded = call_tool(
                sock,
                19,
                "plugin_tools.insmod",
                {"package_path": file_transfer_plugin},
            )
            assert "server.send" in loaded["tools"], loaded

        first = wait_for_udp(udp_sock, time.time() + 5)
        second = wait_for_udp(udp_sock, time.time() + 12)
        assert first["event"] == "online", first
        assert second["event"] == "online", second

        udp_sock.sendto(udp_packet("fake-online", fake_tcp_online), ("127.0.0.1", discovery_server))
        deadline = time.time() + 5
        payload = None
        while time.time() < deadline:
            payload = call_tool(sock, 2, "server.list_servers", {"wait_ms": 100})
            peer = peer_entry(payload, fake_tcp_online)
            if peer and peer["state"] == "online" and len(online_peer.heartbeats) >= 2:
                break
            time.sleep(0.1)
        assert payload is not None, "missing online peer payload"
        peer = peer_entry(payload, fake_tcp_online)
        assert peer and peer["state"] == "online", payload
        assert len(online_peer.heartbeats) >= 2, online_peer.heartbeats
        assert online_peer.heartbeats[-1] - online_peer.heartbeats[0] >= 0.8, online_peer.heartbeats
        if not online_peer.tools_list_seen.wait(2):
            contract_failures.append("missing automatic tools/list after initialization")
        if online_peer.tools_list_requests != 1:
            contract_failures.append(
                f"expected one initialization tools/list, got {online_peer.tools_list_requests}"
            )
        contract_failures.extend(
            verify_automatic_tool_discovery(tcp_server, online_peer, peer["server_id"])
        )
        assert not contract_failures, "\n".join(contract_failures)

        udp_sock.sendto(
            udp_packet("fake-timeout", fake_tcp_timeout),
            ("127.0.0.1", discovery_server),
        )
        failure_peer, failure_request_id = wait_for_online_peer(
            sock, fake_tcp_timeout, 30
        )
        assert timeout_peer.tools_list_seen.wait(2), "missing delayed tools/list request"
        timed_out = proxy_tool_result(
            sock,
            failure_request_id,
            failure_peer["server_id"],
            "system.ping",
            proxy_timeout_ms=4500,
        )
        assert timed_out["isError"] is True, timed_out
        assert "tools discovery timed out" in timed_out["content"][0]["text"].lower(), timed_out

        timeout_peer.set_tools_list_behavior(delay=False, malformed=True)
        malformed = proxy_tool_result(
            sock,
            failure_request_id + 1,
            failure_peer["server_id"],
            "system.ping",
            proxy_timeout_ms=4500,
        )
        assert malformed["isError"] is True, malformed
        assert "invalid tools/list response" in malformed["content"][0]["text"].lower(), malformed
        timeout_peer.set_respond_to_ping(False)
        if os.name != "nt":
            verify_nonblocking_shell_wait(
                exe,
                sock,
                tcp_server,
                discovery_server,
                broadcast_tcp,
                broadcast_discovery,
            )
        if file_transfer_mode != "n":
            assert online_peer.hello_seen.wait(2), "missing MFT1 data-channel negotiation"
            online_peer.assert_initialized_before_hello()

            online_peer.close()
            online_peer = FakePeer(
                fake_tcp_online,
                respond_to_ping=True,
                support_data_channel=True,
                duplicate_initialize_response=True,
            )
            online_peer.start()
            udp_sock.sendto(
                udp_packet("fake-online-restarted", fake_tcp_online),
                ("127.0.0.1", discovery_server),
            )
            deadline = time.time() + 8
            while time.time() < deadline:
                payload = call_tool(sock, 23, "server.list_servers", {"wait_ms": 100})
                peer = peer_entry(payload, fake_tcp_online)
                if (
                    peer
                    and peer["state"] == "online"
                    and peer["tcp_connected"] is True
                    and online_peer.hello_seen.wait(0.1)
                ):
                    break
                time.sleep(0.1)
            assert peer and peer["state"] == "online" and peer["tcp_connected"] is True, payload
            assert online_peer.hello_seen.is_set(), "missing MFT1 negotiation after peer restart"
            online_peer.assert_initialized_before_hello()
            assert online_peer.tools_list_seen.wait(2), "missing tools/list after peer restart"
            assert online_peer.tools_list_requests == 1, online_peer.tools_list_requests
            proxy_ping(sock, 24, peer["server_id"])

            with tempfile.NamedTemporaryFile(delete=False) as transfer_file:
                transfer_file.write(b"d" * (1024 * 1024))
                transfer_path = transfer_file.name
            transfer_sock = wait_for_tcp(tcp_server, proc)
            transfer_sock.settimeout(10.0)
            initialize(transfer_sock)
            transfer = {}

            def send_file():
                try:
                    transfer["result"] = call_tool(
                        transfer_sock,
                        30,
                        "server.send",
                        {
                            "server_id": peer["server_id"],
                            "local_path": transfer_path,
                            "remote_path": ".",
                            "timeout_ms": 5000,
                        },
                    )
                except Exception as exc:
                    transfer["error"] = exc

            transfer_thread = threading.Thread(target=send_file, daemon=True)
            transfer_thread.start()
            assert online_peer.data_link_failed.wait(5), "DATA did not use the negotiated data link"
            time.sleep(0.5)
            assert online_peer.data_connections >= 1, online_peer.data_connections
            assert online_peer.control_data_frames == 0, online_peer.control_data_frames
            assert transfer_thread.is_alive(), transfer
            payload = call_tool(sock, 22, "server.list_servers", {"wait_ms": 100})
            peer = peer_entry(payload, fake_tcp_online)
            assert peer and peer["state"] == "online" and peer["tcp_connected"] is True, payload

        sock.settimeout(8.0)
        if os.name == "nt":
            blocked = call_tool(
                sock,
                20,
                "system.shell_exec",
                {"command": blocking_command(), "timeout_ms": 5000},
            )
            assert blocked["exit_code"] == 0 and blocked["timed_out"] is False, blocked
        payload = call_tool(sock, 21, "server.list_servers", {"wait_ms": 100})
        peer = peer_entry(payload, fake_tcp_online)
        assert peer and peer["state"] == "online" and peer["tcp_connected"] is True, payload
        sock.settimeout(3.0)

        deadline = time.time() + 7
        timeout_payload = None
        while time.time() < deadline:
            timeout_payload = call_tool(sock, 3, "server.list_servers", {"wait_ms": 100})
            peer = peer_entry(timeout_payload, fake_tcp_timeout)
            if peer and peer["state"] == "timeout":
                break
            time.sleep(0.2)
        assert timeout_payload is not None, "missing timeout payload"
        peer = peer_entry(timeout_payload, fake_tcp_timeout)
        assert peer and peer["state"] == "timeout", timeout_payload

        udp_sock.sendto(
            udp_packet("fake-offline", fake_tcp_timeout, event="offline"),
            ("127.0.0.1", discovery_server),
        )
        offline_payload = call_tool(sock, 4, "server.list_servers", {"wait_ms": 100})
        peer = peer_entry(offline_payload, fake_tcp_timeout)
        assert peer and peer["state"] == "offline", offline_payload

        udp_sock.close()
        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        udp_sock.bind(("127.0.0.1", broadcast_target))
        udp_sock.settimeout(0.5)

        broadcast_proc = start_broadcast_port_server(
            exe,
            broadcast_tcp,
            broadcast_discovery,
            broadcast_target,
        )
        wait_for_tcp(broadcast_tcp, broadcast_proc).close()
        packet = wait_for_udp(udp_sock, time.time() + 5)
        assert packet["event"] == "online", packet
        assert packet["tcp_port"] == broadcast_tcp, packet

        broadcast_proc.terminate()
        broadcast_proc.wait(timeout=5)
        broadcast_proc = None

        udp_sock.close()
        udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        udp_sock.bind(("127.0.0.1", broadcast_tcp))
        udp_sock.settimeout(0.5)

        broadcast_proc = start_default_broadcast_port_server(
            exe,
            broadcast_tcp,
            broadcast_discovery,
        )
        wait_for_tcp(broadcast_tcp, broadcast_proc).close()
        packet = wait_for_udp(udp_sock, time.time() + 5)
        assert packet["event"] == "online", packet
        assert packet["tcp_port"] == broadcast_tcp, packet
    finally:
        if sock:
            sock.close()
        if transfer_sock:
            transfer_sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        if broadcast_proc:
            broadcast_proc.terminate()
            try:
                broadcast_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                broadcast_proc.kill()
                broadcast_proc.wait(timeout=5)
        online_peer.close()
        timeout_peer.close()
        udp_sock.close()
        if transfer_path:
            try:
                os.unlink(transfer_path)
            except FileNotFoundError:
                pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
