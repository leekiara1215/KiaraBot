#!/usr/bin/env python3
"""Exercise deployment failure handling and SQLite backup without Docker."""
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
DEPLOY = ROOT / 'scripts/deploy/deploy.sh'
BACKUP = ROOT / 'scripts/deploy/backup.sh'
MIGRATE = ROOT / 'scripts/deploy/migrate.sh'


class DeploymentTest(unittest.TestCase):
    def test_upgrade_failure_does_not_start_old_image(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            docker = path / 'docker'
            docker.write_text('#!/bin/bash\nprintf "%s\\n" "$*" >> "$CALL_LOG"\n'
                              'if [[ " $* " == *" run "* ]]; then exit 1; fi\n')
            docker.chmod(0o755)
            log = path / 'calls'
            env = dict(os.environ, PATH=f'{path}:{os.environ["PATH"]}',
                       CALL_LOG=str(log), KIARABOT_CORE_IMAGE_DIGEST='sha256:' + 'a' * 64,
                       ROLLBACK_IMAGE_DIGEST='sha256:' + 'b' * 64)
            result = subprocess.run([str(DEPLOY), 'upgrade'], env=env, text=True,
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn('UPGRADE_FAILED', result.stdout)
            calls = log.read_text().splitlines()
            self.assertTrue(any(' run ' in f' {line} ' for line in calls))
            self.assertFalse(any(' up ' in f' {line} ' for line in calls))

    def test_upgrade_start_failure_stops_partial_stack(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            docker = path / 'docker'
            docker.write_text('#!/bin/bash\nprintf "%s\\n" "$*" >> "$CALL_LOG"\n'
                              'if [[ " $* " == *" up "*" adapter "* ]]; then exit 1; fi\n')
            docker.chmod(0o755)
            log = path / 'calls'
            env = dict(os.environ, PATH=f'{path}:{os.environ["PATH"]}',
                       CALL_LOG=str(log), KIARABOT_CORE_IMAGE_DIGEST='sha256:' + 'a' * 64)
            result = subprocess.run([str(DEPLOY), 'upgrade'], env=env, text=True,
                                    capture_output=True, check=False)
            self.assertEqual(result.returncode, 1)
            calls = log.read_text().splitlines()
            self.assertTrue(any(' up ' in f' {line} ' and line.endswith(' core') for line in calls))
            self.assertFalse(any(' up ' in f' {line} ' and line.endswith(' panel') for line in calls))
            self.assertIn(' stop panel adapter core', calls[-1])

    def test_backup_restore_and_reject_tampered_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            database = path / 'data with spaces.db'
            with sqlite3.connect(database) as connection:
                connection.execute('CREATE TABLE sample (value TEXT)')
                connection.execute("INSERT INTO sample VALUES ('before')")
                connection.execute('PRAGMA user_version = 5')
            result = subprocess.run([str(BACKUP), 'backup', '--database', str(database),
                                     '--output', str(path / 'backups')], text=True,
                                    capture_output=True, check=True)
            snapshot = Path(result.stdout.splitlines()[-1])
            self.assertTrue(snapshot.is_file())
            self.assertIn('schema_version=5', Path(f'{snapshot}.metadata').read_text())
            with sqlite3.connect(database) as connection:
                connection.execute("UPDATE sample SET value='after'")
            restore = [str(BACKUP), 'restore', '--database', str(database), '--input', str(snapshot)]
            subprocess.run(restore, capture_output=True, check=True)
            with sqlite3.connect(database) as connection:
                self.assertEqual(connection.execute('SELECT value FROM sample').fetchone()[0], 'before')
            with snapshot.open('ab') as stream:
                stream.write(b'changed')
            failed = subprocess.run(restore, text=True, capture_output=True, check=False)
            self.assertEqual(failed.returncode, 1)
            self.assertIn('CHECKSUM_MISMATCH', failed.stdout)
            with sqlite3.connect(database) as connection:
                self.assertEqual(connection.execute('SELECT value FROM sample').fetchone()[0], 'before')

    def test_migration_failure_releases_lock_and_does_not_report_success(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            env = dict(os.environ, DATABASE_PATH=str(path / 'runtime.db'),
                       BACKUP_DIR=str(path / 'backups'), MIGRATION_LOCK_FILE=str(path / 'lock'),
                       MIGRATION_COMMAND='false')
            result = subprocess.run([str(MIGRATE)], env=env, text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn('MIGRATION_FAILED', result.stdout)
            self.assertFalse((path / 'lock').exists())

    @unittest.skipUnless(shutil.which('shasum'), 'shasum is unavailable')
    def test_backup_falls_back_to_shasum(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            bin_dir = path / 'bin'
            bin_dir.mkdir()
            for name in ('bash', 'dirname', 'awk', 'sqlite3', 'mkdir', 'date', 'mktemp',
                         'shasum', 'cat', 'rm'):
                executable = shutil.which(name)
                self.assertIsNotNone(executable, name)
                (bin_dir / name).symlink_to(executable)
            database = path / 'runtime.db'
            with sqlite3.connect(database) as connection:
                connection.execute('CREATE TABLE sample (value TEXT)')
            result = subprocess.run([str(BACKUP), 'backup', '--database', str(database),
                                     '--output', str(path / 'backups')],
                                    env=dict(os.environ, PATH=str(bin_dir)), text=True,
                                    capture_output=True, check=True)
            self.assertIn('sha256=', Path(result.stdout.splitlines()[-1] + '.metadata').read_text())


if __name__ == '__main__':
    unittest.main()
