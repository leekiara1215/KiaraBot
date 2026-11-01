"""Resolve historical evidence explicitly; target availability never inherits reference status."""
import hashlib
import json
import re
from pathlib import PurePosixPath

TARGET_SOURCE_ROOTS = ('core/src/runtime/platform/', 'core/src/application/target/',
                       'core/src/domain/target/', 'control/api/src/application/target/',
                       'control/api/app/')


class EvidenceFailure(ValueError):
    pass


def safe_path(root, name):
    if not isinstance(name, str):
        raise EvidenceFailure('evidence path must be a string')
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or path.as_posix() != name or not path.parts:
        raise EvidenceFailure('evidence path must be canonical and relative: ' + name)
    current = root
    for part in path.parts:
        current = current / part
        if current.is_symlink():
            raise EvidenceFailure('evidence path must not contain symlinks: ' + name)
    return current


class EvidenceResolver:
    def __init__(self, root):
        self.root = root
        self.mappings = {}
        manifest_path = safe_path(root, 'core/reference/manifest.json')
        if not manifest_path.exists():
            return  # Pre-isolation checkout: original logical evidence remains valid.
        manifest = json.loads(manifest_path.read_text())
        if not isinstance(manifest, dict) or set(manifest) != {'base', 'files'}:
            raise EvidenceFailure('unknown reference manifest structure')
        if not isinstance(manifest['base'], str) or not re.fullmatch('[0-9a-f]{40}', manifest['base']):
            raise EvidenceFailure('invalid reference manifest base')
        if not isinstance(manifest['files'], list) or not manifest['files']:
            raise EvidenceFailure('empty reference manifest')
        for entry in manifest['files']:
            if not isinstance(entry, dict) or set(entry) != {'source', 'reference', 'blob'}:
                raise EvidenceFailure('unknown reference mapping structure')
            logical, destination = entry['source'], entry['reference']
            original, actual = safe_path(root, logical), safe_path(root, destination)
            if not logical.startswith(('core/app/', 'core/include/', 'core/src/', 'core/tests/')):
                raise EvidenceFailure('unknown reference logical source: ' + logical)
            if destination != 'core/reference/' + logical.removeprefix('core/'):
                raise EvidenceFailure('incorrect reference mapping: ' + logical)
            if logical in self.mappings:
                raise EvidenceFailure('duplicate reference mapping: ' + logical)
            if original.exists():
                raise EvidenceFailure('reference source reintroduced: ' + logical)
            if not actual.is_file():
                raise EvidenceFailure('reference destination missing: ' + destination)
            content = actual.read_bytes()
            digest = hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()
            if not isinstance(entry['blob'], str) or digest != entry['blob']:
                raise EvidenceFailure('reference blob changed: ' + destination)
            self.mappings[logical] = actual

    def resolve(self, logical):
        original = safe_path(self.root, logical)
        actual = self.mappings.get(logical, original)
        if not actual.is_file():
            raise EvidenceFailure('evidence file missing: ' + logical)
        return actual

    def is_target_source(self, logical):
        actual = self.resolve(logical)
        relative = actual.relative_to(self.root).as_posix()
        return relative.startswith(TARGET_SOURCE_ROOTS) and actual.suffix in {'.cpp', '.cc'}

    def is_target_test(self, logical):
        actual = self.resolve(logical)
        relative = actual.relative_to(self.root).as_posix()
        return 'reference' not in actual.relative_to(self.root).parts and (
            relative.startswith(('tests/architecture/', 'core/tests/platform/', 'core/tests/architecture/'))
        ) and actual.suffix in {'.py', '.cpp', '.cc', '.cjs', '.rb'}


def effective_status(registry, scope, category, key):
    historical = registry['categories'][category][key]  # Reject unknown categories/items.
    if scope == 'reference':
        return historical
    if scope != 'target':
        raise EvidenceFailure('unknown execution scope: ' + scope)
    override = registry['execution_scopes']['target']['overrides'].get(category, {}).get(key)
    if override is not None:
        return override
    return 'deprecated' if historical == 'deprecated' else 'not_implemented'


def validate_scopes(registry, resolver):
    scopes = registry.get('execution_scopes', {})
    if not isinstance(scopes, dict) or set(scopes) != {'reference', 'target'}:
        raise EvidenceFailure('execution scopes must explicitly name reference and target')
    reference, target = scopes['reference'], scopes['target']
    if reference != {'status_source': 'categories', 'production': False}:
        raise EvidenceFailure('historical categories must remain non-production reference scope')
    if not isinstance(target, dict) or set(target) != {'default_status', 'overrides', 'evidence'} or target['default_status'] != 'not_implemented':
        raise EvidenceFailure('target defaults must fail closed to not_implemented')
    if not isinstance(target['overrides'], dict) or not isinstance(target['evidence'], dict):
        raise EvidenceFailure('target overrides/evidence must be explicit objects')
    expected_evidence = set()
    for category, values in target['overrides'].items():
        if category not in registry['categories'] or not isinstance(values, dict):
            raise EvidenceFailure('unknown target override category')
        for key, status in values.items():
            if key not in registry['categories'][category] or status not in {'implemented', 'not_implemented', 'deprecated'}:
                raise EvidenceFailure('unknown target override item/status')
            if registry['categories'][category][key] == 'deprecated' and status != 'deprecated':
                raise EvidenceFailure('target cannot resurrect deprecated declaration')
            if status == 'implemented':
                qualified = category + ':' + key
                expected_evidence.add(qualified)
                paths = target['evidence'].get(qualified)
                if not isinstance(paths, list) or not paths:
                    raise EvidenceFailure('target implemented item lacks scoped evidence: ' + qualified)
                if not any(resolver.is_target_source(path) for path in paths):
                    raise EvidenceFailure('target implementation requires a current production source: ' + qualified)
                if not any(resolver.is_target_test(path) for path in paths):
                    raise EvidenceFailure('target implementation requires a current target test: ' + qualified)
                for path in paths:
                    actual = resolver.resolve(path)
                    if 'reference' in actual.relative_to(resolver.root).parts:
                        raise EvidenceFailure('reference evidence cannot certify target production: ' + qualified)
    if set(target['evidence']) != expected_evidence:
        raise EvidenceFailure('target evidence must cover exactly target implemented overrides')
