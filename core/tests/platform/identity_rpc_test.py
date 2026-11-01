"""Real mTLS Identity RPC boundary test with explicit pure-application/verifier fixtures."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from service_config_test import certificates


def main(driver):
    with tempfile.TemporaryDirectory(prefix='p04-identity-rpc-') as directory:
        config = certificates(Path(directory), 'ec')
        # The production certificate verifier uses X509_STRICT. Give the temporary CA
        # explicit signing KU and the leaf explicit non-CA/KU, rather than relaxing it.
        def openssl(*arguments):
            subprocess.run(['openssl', *arguments], cwd=directory, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        openssl('req', '-x509', '-key', 'ca.key', '-days', '1',
                '-subj', '/CN=P02-Test-CA', '-out', 'ca.pem',
                '-addext', 'basicConstraints=critical,CA:TRUE',
                '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
                '-addext', 'subjectKeyIdentifier=hash')
        (Path(directory) / 'strict-extensions').write_text(
            'subjectAltName=DNS:localhost,IP:127.0.0.1\n'
            'basicConstraints=critical,CA:FALSE\n'
            'keyUsage=critical,digitalSignature,keyEncipherment\n'
            'extendedKeyUsage=serverAuth,clientAuth\n'
            'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n')
        openssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem',
                '-CAkey', 'ca.key', '-CAcreateserial', '-days', '1',
                '-extfile', 'strict-extensions', '-out', 'server.pem')
        tls = config['tls']
        result = subprocess.run(
            [driver, tls['certificate_file'], tls['ca_file'], tls['private_key_file']],
            capture_output=True, text=True, timeout=30)
        if result.returncode:
            location = re.search(r'Identity RPC fixture assertion line [0-9]+; diagnostics redacted', result.stderr)
            raise RuntimeError(location.group(0) if location else 'Identity RPC fixture failed; diagnostics redacted')
        assert 'fixture app only' in result.stdout
        print(result.stdout.strip())


if __name__ == '__main__':
    main(sys.argv[1])
