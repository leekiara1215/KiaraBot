#!/usr/bin/env python3
"""Real mTLS HostManagement RPC boundary test with typed application/authorizer fixtures."""
import hashlib
import re
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path


def openssl(directory, *arguments):
    result = subprocess.run(['openssl', *arguments], cwd=directory, capture_output=True,
                            timeout=10)
    assert result.returncode == 0, 'fixture certificate generation failed'


def certificates(root):
    (root / 'ca.ext').write_text(
        'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n'
        'subjectKeyIdentifier=hash\n')
    (root / 'leaf.ext').write_text(
        'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature,keyEncipherment\n'
        'extendedKeyUsage=serverAuth,clientAuth\nsubjectKeyIdentifier=hash\n'
        'authorityKeyIdentifier=keyid:always\n')
    (root / 'client.ext').write_text(
        'subjectAltName=URI:spiffe://kiarabot/control-test\nbasicConstraints=critical,CA:FALSE\n'
        'keyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n'
        'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')
    openssl(root, 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
            '-nodes', '-days', '1', '-subj', '/CN=P08-Test-CA', '-keyout', 'ca.key',
            '-out', 'ca.pem', '-addext', 'basicConstraints=critical,CA:TRUE',
            '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
            '-addext', 'subjectKeyIdentifier=hash')
    for name, cert, key, extension, subject in (
            ('server', 'server.pem', 'server.key', 'leaf.ext', '/CN=localhost'),
            ('client', 'client.pem', 'client.key', 'client.ext', '/CN=control-test')):
        openssl(root, 'req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
                '-subj', subject, '-keyout', key, '-out', f'{name}.csr')
        openssl(root, 'x509', '-req', '-in', f'{name}.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
                '-CAcreateserial', '-days', '1', '-extfile', extension, '-out', cert)
    (root / 'client.key').chmod(0o600)
    der = ssl.PEM_cert_to_DER_cert((root / 'client.pem').read_text())
    return {
        'server_certificate': str(root / 'server.pem'),
        'server_key': str(root / 'server.key'),
        'ca': str(root / 'ca.pem'),
        'client_certificate': str(root / 'client.pem'),
        'client_key': str(root / 'client.key'),
        'client_fingerprint': hashlib.sha256(der).hexdigest(),
    }


def main(driver):
    with tempfile.TemporaryDirectory(prefix='p08-host-management-rpc-') as directory:
        artifacts = certificates(Path(directory))
        arguments = [driver, artifacts['server_certificate'], artifacts['server_key'],
                     artifacts['ca'], artifacts['client_certificate'], artifacts['client_key'],
                     artifacts['client_fingerprint']]
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=60)
        if result.returncode:
            location = re.search(
                r'HostManagement RPC fixture assertion line [0-9]+; diagnostics redacted',
                result.stderr)
            raise RuntimeError(location.group(0) if location else
                               'HostManagement RPC fixture failed; diagnostics redacted')
        assert 'fixtures passed' in result.stdout
        print(result.stdout.strip())


if __name__ == '__main__':
    main(sys.argv[1])
