"""Disposable server-verified TLS material for adapter target-bridge tests.

Only temporary test certificates are generated with the system OpenSSL binary; no production
CA, credential or private key is read, and the material is removed with the temporary directory.
"""
from __future__ import annotations

import subprocess
from pathlib import Path


def write_server_tls(directory: Path):
    """Create a self-signed server certificate whose SAN covers loopback test clients."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    certificate = directory / 'server.pem'
    private_key = directory / 'server.key'
    subprocess.run([
        'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
        '-keyout', str(private_key), '-out', str(certificate), '-days', '2',
        '-subj', '/CN=localhost',
        '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1',
    ], check=True, capture_output=True, timeout=60)
    private_key.chmod(0o600)
    return certificate, private_key


def write_trust_file(directory: Path, certificate: Path):
    """CA file handed to the adapter; the self-signed server certificate is the trust anchor."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    trust = directory / 'ca.pem'
    trust.write_bytes(Path(certificate).read_bytes())
    return trust


def write_bootstrap_secret(directory: Path, credential: str):
    """Deployment secret file: owner-only regular file, one credential plus a trailing LF."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    secret = directory / 'bootstrap-credential'
    secret.write_text(credential + '\n')
    secret.chmod(0o600)
    return secret
