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
