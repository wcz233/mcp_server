import hashlib
import json
import os
from pathlib import Path
import socket
import ssl


TCP_SECURITY = os.environ.get("MCP_TEST_TCP_SECURITY", "mtls")
TLS_FIXTURES = Path(os.environ.get("MCP_TEST_TLS_DIR", ""))


def add_server_tls_env(env, identity="node-a"):
    env["MCP_TCP_SECURITY"] = TCP_SECURITY
    if TCP_SECURITY == "plaintext":
        return env

    config_path = TLS_FIXTURES / f"network_security_{identity}.json"
    config_path.write_text(
        json.dumps(
            {
                "version": 1,
                "enabled": True,
                "mtls": {
                    "ca_file": str(TLS_FIXTURES / "ca.cert.pem"),
                    "certificate_file": str(TLS_FIXTURES / f"{identity}.cert.pem"),
                    "private_key_file": str(TLS_FIXTURES / f"{identity}.key.pem"),
                    "server_name": "",
                },
            }
        ),
        encoding="utf-8",
    )
    env["MCP_NETWORK_SECURITY_CONFIG"] = str(config_path)
    return env


def certificate_fingerprint(identity):
    pem = (TLS_FIXTURES / f"{identity}.cert.pem").read_text(encoding="ascii")
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(pem)).hexdigest()


def client_context(identity="node-b", trusted=True, check_hostname=True):
    ca_file = TLS_FIXTURES / ("ca.cert.pem" if trusted else "untrusted-ca.cert.pem")
    context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=ca_file)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.check_hostname = check_hostname
    if identity:
        context.load_cert_chain(
            TLS_FIXTURES / f"{identity}.cert.pem",
            TLS_FIXTURES / f"{identity}.key.pem",
        )
    return context


def connect_tls(port, identity="node-b", trusted=True, timeout=0.5):
    raw = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    if TCP_SECURITY == "plaintext":
        return raw
    try:
        return client_context(identity, trusted).wrap_socket(raw, server_hostname="localhost")
    except Exception:
        raw.close()
        raise


def server_context(identity="node-b"):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.verify_mode = ssl.CERT_REQUIRED
    context.load_verify_locations(TLS_FIXTURES / "ca.cert.pem")
    context.load_cert_chain(
        TLS_FIXTURES / f"{identity}.cert.pem",
        TLS_FIXTURES / f"{identity}.key.pem",
    )
    return context
