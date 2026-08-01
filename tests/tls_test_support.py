import hashlib
import os
from pathlib import Path
import socket
import ssl


TLS_FIXTURES = Path(os.environ["MCP_TEST_TLS_DIR"])


def add_server_tls_env(env, identity="node-a"):
    env["MCP_TLS_CA_FILE"] = str(TLS_FIXTURES / "ca.cert.pem")
    env["MCP_TLS_CERT_FILE"] = str(TLS_FIXTURES / f"{identity}.cert.pem")
    env["MCP_TLS_KEY_FILE"] = str(TLS_FIXTURES / f"{identity}.key.pem")
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
