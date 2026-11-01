"""Real listener lifecycle with an outer dependency; no database business substitute."""
import http.client
import json
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile

from service_config_test import certificates


def main(driver):
    with tempfile.TemporaryDirectory(prefix='p03-composition-') as folder:
        root = Path(folder)
        config = certificates(root, 'ec')
        path = root / 'config.json'
        path.write_text(json.dumps(config))
        for mode, arguments, expected, called in [
            ('okay', ['--help'], 0, False), ('okay', [], 2, False),
            ('okay', ['--config', str(root / 'missing')], 2, False),
            *[(mode, ['--config', str(path)], 1, True)
              for mode in ['null', 'error', 'wrong-owner', 'throw', 'unknown-throw']]
        ]:
            result = subprocess.run([driver, mode, *arguments], capture_output=True, text=True, timeout=5)
            assert result.returncode == expected, (mode, result)
            assert ('composer_called' in result.stderr) == called, result.stderr
            assert 'never-print-composition-secret' not in result.stderr
            assert 'probe_port=' not in result.stdout
        for mode in ['okay', 'lost']:
            child = subprocess.Popen([driver, mode, '--config', str(path)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                assert select.select([child.stdout], [], [], 10)[0], 'startup timed out'
                state = dict(item.split('=', 1) for item in child.stdout.readline().strip().split())
                assert state.get('ready') == 'false', state
                connection = http.client.HTTPConnection('127.0.0.1', int(state['probe_port']), timeout=2)
                try:
                    connection.request('GET', '/health/live')
                    response = connection.getresponse()
                    assert response.status == 200
                    assert json.loads(response.read()) == {'state': 'HEALTHY'}
                finally:
                    connection.close()
                if mode == 'okay':
                    child.send_signal(signal.SIGTERM)
                stdout, stderr = child.communicate(timeout=5)
                assert child.returncode == (0 if mode == 'okay' else 1), stderr
                assert 'application_destroyed dependencies_alive=1' in stderr, stderr
                assert ('reason=dependency_lost' in stderr) == (mode == 'lost'), stderr
            finally:
                if child.poll() is None:
                    child.kill()
                    child.communicate(timeout=5)
        print('Composition guards, signals, dependency loss and resource lifetime passed')


if __name__ == '__main__':
    main(sys.argv[1])
