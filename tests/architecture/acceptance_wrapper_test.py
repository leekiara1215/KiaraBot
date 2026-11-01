"""Test shell command routing with disposable executables; never a business-suite result."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2] / 'tests/acceptance/run.sh'


class WrapperRoutingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='kiarabot-wrapper-')
        self.addCleanup(self.temporary.cleanup)
        self.root = (Path(self.temporary.name) / 'checkout with spaces').resolve()
        self.script = self.root / 'tests/acceptance/run.sh'
        self.script.parent.mkdir(parents=True)
        shutil.copyfile(SOURCE, self.script)
        self.build = self.root / 'custom build'
        self.reference = self.build / 'reference'
        self.log = self.root / 'route.jsonl'
        self.commands = self.root / 'fixture commands'
        self.commands.mkdir()
        self.sdk = self.root / 'sdk/python/.venv/bin/python'
        self.control = self.root / 'control/api/tests/.venv/bin/python'
        self.adapter_dir = self.root / 'adapter with spaces'
        self.adapter = self.adapter_dir / '.venv/bin/python'
        self.core = self.reference / 'core/kiarabot-core'
        self.policy = self.build / 'kiarabot-control-application-test'
        for path, actor in [(self.sdk, 'sdk'), (self.control, 'control'),
                            (self.adapter, 'adapter'), (self.core, 'reference-core'),
                            (self.policy, 'root-policy'), (self.commands / 'ctest', 'ctest'),
                            (self.commands / 'ruby', 'ruby')]:
            self.executable(path, actor)
        self.environment = {k: v for k, v in os.environ.items()
                            if not k.startswith(('KIARABOT_', 'CORE_', 'SDK_SOURCE', 'WRAPPER_'))}
        self.environment.update(PATH=str(self.commands) + os.pathsep + os.environ['PATH'],
                                KIARABOT_ACCEPTANCE_BUILD=str(self.build),
                                KIARABOT_ACCEPTANCE_PYTHON=str(self.sdk),
                                KIARABOT_CONTROL_TEST_PYTHON=str(self.control),
                                KIARABOT_ADAPTER_DIR=str(self.adapter_dir),
                                WRAPPER_LOG=str(self.log))

    def executable(self, path, actor):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f'''#!{sys.executable}
import json, os, sys
from pathlib import Path
actor = {actor!r}
record = dict(actor=actor, argv=sys.argv[1:], cwd=str(Path.cwd()),
              core=os.environ.get('CORE_TEST_BINARY'),
              build=os.environ.get('KIARABOT_ACCEPTANCE_BUILD'),
              policy=os.environ.get('CONTROL_POLICY_BINARY'),
              sdk_source=os.environ.get('SDK_SOURCE'))
with open(os.environ['WRAPPER_LOG'], 'a') as log:
    log.write(json.dumps(record) + '\\n')
if actor == 'ruby' and not os.access(os.environ.get('CONTROL_POLICY_BINARY', ''), os.X_OK):
    sys.exit(2)
if actor == os.environ.get('WRAPPER_FAIL_ACTOR'):
    match = os.environ.get('WRAPPER_FAIL_MATCH', '')
    if match in ' '.join(sys.argv[1:]):
        sys.exit(7)
''')
        path.chmod(0o700)

    def run_wrapper(self, **changes):
        environment = dict(self.environment, **changes)
        result = subprocess.run(['bash', str(self.script)], cwd=self.temporary.name,
                                env=environment, capture_output=True, text=True, timeout=15)
        records = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, records

    def test_all_routes_keep_build_root_and_reference_separate(self):
        result, records = self.run_wrapper()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('reference acceptance passed', result.stdout)
        self.assertIn('not six-service business acceptance', result.stdout)
        self.assertEqual([r['actor'] for r in records], ['sdk', 'control', 'ctest', 'ruby', 'sdk', 'adapter', 'adapter'])
        for record in records:
            self.assertEqual(record['core'], str(self.core))
        sdk_generate, control, ctest, ruby, sdk_test, adapter_generate, adapter_test = records
        self.assertEqual(sdk_generate['argv'], ['sdk/python/tools/generate_proto.py'])
        self.assertEqual(control['build'], str(self.reference))
        self.assertEqual(control['argv'], ['-m', 'pytest', '-q', 'control/api/tests'])
        self.assertEqual(ctest['argv'], ['--test-dir', str(self.build), '-L', '^reference$',
                                        '--output-on-failure', '--no-tests=error'])
        self.assertEqual(ctest['build'], str(self.build))
        self.assertEqual(ruby['policy'], str(self.policy))
        self.assertEqual(ruby['argv'], ['proto/tests/api_contract_test.rb'])
        self.assertEqual(sdk_test['argv'], ['-m', 'pytest', '-q', 'sdk/python/tests'])
        self.assertEqual(sdk_test['build'], str(self.build))
        self.assertEqual(adapter_generate['argv'], ['tools/generate_proto.py', str(self.root / 'proto')])
        self.assertEqual(adapter_test['argv'], ['-m', 'pytest', '-q'])
        for record in (adapter_generate, adapter_test):
            self.assertEqual(record['cwd'], str(self.adapter_dir))
            self.assertEqual(record['sdk_source'], str(self.root / 'sdk/python/src'))

    def test_reference_subdirectory_cannot_replace_build_root_input(self):
        result, records = self.run_wrapper(KIARABOT_ACCEPTANCE_BUILD=str(self.reference))
        self.assertEqual(result.returncode, 2)
        self.assertIn('isolated reference Core', result.stderr)
        self.assertEqual(records, [])

    def test_stale_old_core_is_not_a_fallback(self):
        self.core.unlink()
        self.executable(self.build / 'core/kiarabot-core', 'stale-core')
        result, records = self.run_wrapper()
        self.assertEqual(result.returncode, 2)
        self.assertIn('isolated reference Core', result.stderr)
        self.assertEqual(records, [])
        self.assertNotIn('passed', result.stdout)

    def test_missing_sdk_control_or_enabled_adapter_environment_fails(self):
        for path, message in [(self.sdk, 'sdk/python'), (self.control, 'control/api/tests'),
                              (self.adapter, 'Adapter locked environment')]:
            with self.subTest(path=path):
                path.chmod(0o600)
                try:
                    if self.log.exists():
                        self.log.unlink()
                    result, _ = self.run_wrapper()
                    self.assertEqual(result.returncode, 2)
                    self.assertIn(message, result.stderr)
                    self.assertNotIn('passed', result.stdout)
                finally:
                    path.chmod(0o700)

    def test_root_policy_path_never_falls_back_to_reference(self):
        self.policy.unlink()
        self.executable(self.reference / 'kiarabot-control-application-test', 'wrong-policy')
        result, records = self.run_wrapper()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(records[-1]['actor'], 'ruby')
        self.assertEqual(records[-1]['policy'], str(self.policy))
        self.assertIn('Ruby/OpenAPI contract', result.stderr)
        self.assertNotIn('passed', result.stdout)

    def test_stage_failures_propagate_without_skipping_assertions(self):
        cases = [('sdk', 'generate_proto.py', 'generate SDK protocol', 1),
                 ('control', 'pytest', 'control API acceptance', 2),
                 ('ctest', '--test-dir', 'reference CTest', 3),
                 ('ruby', 'api_contract', 'Ruby/OpenAPI contract', 4),
                 ('sdk', 'pytest', 'SDK tests', 5),
                 ('adapter', 'generate_proto.py', 'adapter tests', 6),
                 ('adapter', 'pytest', 'adapter tests', 7)]
        for actor, match, stage, count in cases:
            with self.subTest(stage=stage):
                if self.log.exists():
                    self.log.unlink()
                result, records = self.run_wrapper(WRAPPER_FAIL_ACTOR=actor, WRAPPER_FAIL_MATCH=match)
                self.assertEqual(result.returncode, 7)
                self.assertIn('acceptance failed during: ' + stage, result.stderr)
                self.assertEqual(len(records), count)
                self.assertNotIn('passed', result.stdout)

    def test_existing_adapter_opt_in_and_python_overrides_are_preserved(self):
        environment_sdk = self.commands / 'explicit-sdk-python'
        environment_control = self.commands / 'explicit-control-python'
        self.executable(environment_sdk, 'sdk')
        self.executable(environment_control, 'control')
        result, records = self.run_wrapper(KIARABOT_ADAPTER_DIR='',
                                          KIARABOT_ACCEPTANCE_PYTHON=str(environment_sdk),
                                          KIARABOT_CONTROL_TEST_PYTHON=str(environment_control))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([r['actor'] for r in records], ['sdk', 'control', 'ctest', 'ruby', 'sdk'])


if __name__ == '__main__':
    unittest.main()
