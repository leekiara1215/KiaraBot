"""Launcher ownership boundaries; full managed message flow is an integration check."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
LAUNCHER = ROOT / 'scripts/local/start.py'
SHORT_TEMP = '/private/tmp' if Path('/private/tmp').is_dir() else '/tmp'


def invoke(state, command='status'):
    return subprocess.run([sys.executable, str(LAUNCHER), command, '--state-dir', str(state)],
                          capture_output=True, text=True, timeout=5)


def test_refuses_symlink_state_and_insecure_secret():
    with tempfile.TemporaryDirectory(prefix='kb-l-', dir=SHORT_TEMP) as folder:
        root = Path(folder)
        target = root / 'target'
        target.mkdir(mode=0o700)
        (root / 'link').symlink_to(target)
        assert invoke(root / 'link').returncode == 1
        assert invoke(target).returncode == 1  # Creates only the owned marker, not a service.
        secret = target / 'admin-password'
        secret.write_text('must-not-print-password')
        secret.chmod(0o644)
        result = invoke(target, 'init')
        assert result.returncode == 1 and 'must-not-print-password' not in result.stderr
        assert not (target / 'run.json').exists()


def test_foreign_pid_is_never_signalled():
    with tempfile.TemporaryDirectory(prefix='kb-l-', dir=SHORT_TEMP) as folder:
        state = Path(folder)
        invoke(state)
        foreign = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        try:
            identity = subprocess.check_output(
                ['ps', '-p', str(foreign.pid), '-o', 'uid=,lstart=,command='], text=True).strip()
            path = state / 'run.json'
            path.write_text(json.dumps({'pid': foreign.pid, 'identity': identity,
                                        'run_id': 'unrelated', 'state': 'running'}))
            path.chmod(0o600)
            assert invoke(state, 'stop').returncode == 1
            assert foreign.poll() is None
        finally:
            foreign.terminate()
            foreign.wait(timeout=3)


def test_symlink_lock_is_not_followed():
    with tempfile.TemporaryDirectory(prefix='kb-l-', dir=SHORT_TEMP) as folder:
        state = Path(folder)
        invoke(state)
        victim = state / 'victim'
        victim.write_text('unchanged')
        victim.chmod(0o600)
        (state / 'lock').symlink_to(victim)
        assert invoke(state, 'init').returncode == 1
        assert victim.read_text() == 'unchanged'
