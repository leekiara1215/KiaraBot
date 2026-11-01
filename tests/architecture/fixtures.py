"""Disposable test PKI and loopback ports. Never reads production credentials."""
import contextlib
import os
import socket
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

SERVICES = ('control', 'identity', 'runtime', 'plugin-host', 'scheduler', 'model')


@dataclass(frozen=True)
class Certificate:
    certificate: Path
    key: Path


class TestPki:
    """Private temporary CA, service leaves, trusted/untrusted/expired client leaves."""

    def __init__(self, openssl):
        self.openssl = str(Path(openssl).resolve(strict=True))
        self._temporary = None

    def run(self, *arguments, expected=0):
        result = subprocess.run([self.openssl, *map(str, arguments)], cwd=self.root,
                                capture_output=True, timeout=20)
        if result.returncode != expected:
            # No command stdout/stderr: OpenSSL may print PEM or private paths.
            raise RuntimeError(f'OpenSSL {arguments[0]} returned {result.returncode}, expected {expected}')
        return result

    def __enter__(self):
        self._temporary = tempfile.TemporaryDirectory(prefix='kiarabot-p02-pki-')
        self.root = Path(self._temporary.name)
        os.chmod(self.root, 0o700)
        try:
            self.version = self.run('version').stdout.decode().strip()
            self.ca = self._authority('trusted')
            self.foreign_ca = self._authority('untrusted')
            self.servers = {service: self.issue(service, server=True) for service in SERVICES}
            self.client = self.issue('acceptance-client')
            self.foreign_client = self.issue('foreign-client', authority=self.foreign_ca)
            self.expired_client = self.issue('expired-client', expired=True)
            self.expired_server = self.issue('expired-server', server=True, expired=True)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        if self._temporary is not None:
            self._temporary.cleanup()

    def _key(self, name):
        key = self.root / (name + '.key')
        self.run('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', key)
        key.chmod(0o600)
        return key

    def _authority(self, name):
        key = self._key(name)
        cert = self.root / (name + '.pem')
        self.run('req', '-new', '-x509', '-key', key, '-out', cert, '-days', '2',
                 '-subj', f'/CN=KiaraBot disposable {name}',
                 '-addext', 'basicConstraints=critical,CA:TRUE',
                 '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
        directory = self.root / name
        directory.mkdir(mode=0o700)
        (directory / 'index').touch()
        (directory / 'serial').write_text('1000\n')
        (directory / 'config').write_text(f'''[ca]
default_ca = test
[test]
database = {directory / 'index'}
serial = {directory / 'serial'}
new_certs_dir = {directory}
certificate = {cert}
private_key = {key}
default_md = sha256
default_days = 1
policy = names
[names]
commonName = supplied
[server]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost,IP:127.0.0.1
[client]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth
''')
        return Certificate(cert, key)

    def issue(self, name, server=False, expired=False, authority=None):
        authority = authority or self.ca
        key = self._key(name)
        request = self.root / (name + '.csr')
        cert = self.root / (name + '.pem')
        self.run('req', '-new', '-key', key, '-out', request, '-subj', '/CN=' + name)
        config = self.root / authority.certificate.stem / 'config'
        dates = ('-startdate', '20000101000000Z', '-enddate', '20000102000000Z') if expired else ()
        self.run('ca', '-batch', '-notext', '-config', config, '-extensions',
                 'server' if server else 'client', '-in', request, '-out', cert, *dates)
        return Certificate(cert, key)


@contextlib.contextmanager
def reserved_listener():
    """Bind only a kernel-selected loopback port; keep it held for conflict tests."""
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        yield listener


def free_endpoint():
    # The production child cannot inherit a listening socket under the shared CLI.
    # Reserve/release immediately; unexpected contention must fail, never retry a failing service.
    with reserved_listener() as listener:
        return f'127.0.0.1:{listener.getsockname()[1]}'
