"""Real gRPC transport evidence plus bounded property parsing; no identity authorization fake."""
from pathlib import Path
import subprocess
import sys
import tempfile

from service_config_test import certificates


def main(driver):
    with tempfile.TemporaryDirectory(prefix='p04-peer-evidence-') as folder:
        config = certificates(Path(folder), 'ec')
        tls = config['tls']
        fingerprint = subprocess.run(
            ['openssl', 'x509', '-in', tls['certificate_file'], '-noout', '-sha256', '-fingerprint'],
            capture_output=True, text=True, check=True, timeout=5
        ).stdout.strip().split('=')[1].replace(':', '').lower()
        result = subprocess.run(
            [driver, tls['certificate_file'], tls['ca_file'], tls['private_key_file'], fingerprint],
            capture_output=True, text=True, timeout=10)
        if result.returncode:
            raise RuntimeError('peer evidence fixture failed; transport diagnostics redacted')
        print('Real mTLS peer evidence, fingerprint, chain and rejection checks passed')


if __name__ == '__main__':
    main(sys.argv[1])
