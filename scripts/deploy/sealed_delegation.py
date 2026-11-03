#!/usr/bin/env python3
"""P05b-6 sealed instance-parent delegation file: strict reader and atomic writer.

This module contains NO signing or authorization logic. The token is an Identity-signed parent
delegation obtained by an authenticated administrator; Runtime must still verify it online.
"""
from __future__ import annotations

import base64
import json
import os
import stat
import time

MAX_FILE_BYTES = 64 * 1024
MAX_TTL_SECONDS = 56
SCHEMA_VERSION = 1
AUDIENCE = 'runtime'
TASK_PERMISSIONS = frozenset({'task.schedule', 'task.read'})
MODEL_PERMISSIONS = frozenset({'model.chat', 'model.call.inspect'})
ALLOWED_PERMISSIONS = TASK_PERMISSIONS | MODEL_PERMISSIONS
ALLOWED_SCOPE_KINDS = frozenset({'RESOURCE_INSTANCE', 'RESOURCE_INSTALLATION',
                                 'RESOURCE_MODEL'})


class SealedDelegationError(ValueError):
    """Stable validation/IO failure; messages never contain token or file content."""


def _fail(message: str) -> None:
    raise SealedDelegationError(message)


def _valid_uuid_like(value) -> bool:
    return isinstance(value, str) and 0 < len(value) <= 128 and all(
        character.isalnum() or character in '-_' for character in value)


def _decode_segment(segment: str) -> dict:
    if not isinstance(segment, str) or not segment:
        _fail('delegation_proof is not a compact JWT')
    padding = '=' * (-len(segment) % 4)
    try:
        decoded = base64.urlsafe_b64decode(segment + padding)
        value = json.loads(decoded.decode('utf-8'))
    except (ValueError, UnicodeDecodeError) as error:
        raise SealedDelegationError('delegation_proof payload is not JSON') from error
    if not isinstance(value, dict):
        _fail('delegation_proof payload is not an object')
    return value


def decode_proof(token: str) -> dict:
    """Structural JWT decode only; it is never a signature verification."""
    if not isinstance(token, str) or token.count('.') != 2 or len(token) > 32768:
        _fail('delegation_proof is not a compact JWT')
    parts = token.split('.')
    if any(not part for part in parts):
        _fail('delegation_proof is not a compact JWT')
    payload = _decode_segment(parts[1])
    delegation = payload.get('delegation')
    if not isinstance(delegation, dict):
        _fail('delegation_proof has no nested delegation claims')
    return payload


def validate_document(document: dict, *, expected_instance: str | None = None,
                      expected_installation: str | None = None,
                      expected_model: str | None = None,
                      now: float | None = None, max_ttl: int = MAX_TTL_SECONDS) -> dict:
    if not isinstance(document, dict):
        _fail('document is not an object')
    required = {'schema_version', 'instance_id', 'installation_id', 'audience', 'permissions',
                'scope', 'issued_at', 'expires_at', 'ttl_seconds', 'delegation_proof'}
    if set(document) != required:
        _fail('document fields do not match schema')
    if document['schema_version'] != SCHEMA_VERSION:
        _fail('unsupported schema_version')
    if not _valid_uuid_like(document['instance_id']):
        _fail('invalid instance_id')
    if not _valid_uuid_like(document['installation_id']):
        _fail('invalid installation_id')
    if expected_instance is not None and document['instance_id'] != expected_instance:
        _fail('instance_id does not match requested instance')
    if expected_installation is not None and document['installation_id'] != expected_installation:
        _fail('installation_id does not match requested installation')
    if document['audience'] != AUDIENCE:
        _fail('audience must be runtime')
    permissions = document['permissions']
    if (not isinstance(permissions, list) or not permissions or len(permissions) > 8 or
            len(set(permissions)) != len(permissions) or
            any(not isinstance(item, str) or item not in ALLOWED_PERMISSIONS
                for item in permissions)):
        _fail('permissions are not whitelisted')
    task_permissions = {item for item in permissions if item in TASK_PERMISSIONS}
    model_permissions = {item for item in permissions if item in MODEL_PERMISSIONS}
    if task_permissions and model_permissions:
        _fail('task and model permissions must use separate sealed files')
    scope = document['scope']
    if not isinstance(scope, list) or not scope or len(scope) > 8:
        _fail('invalid scope')
    seen = set()
    kinds = set()
    for entry in scope:
        if (not isinstance(entry, dict) or set(entry) != {'kind', 'id'} or
                entry['kind'] not in ALLOWED_SCOPE_KINDS or
                not _valid_uuid_like(entry['id'])):
            _fail('invalid scope entry')
        key = (entry['kind'], entry['id'])
        if key in seen:
            _fail('duplicate scope entry')
        seen.add(key)
        kinds.add(entry['kind'])
        if entry['kind'] == 'RESOURCE_INSTANCE' and entry['id'] != document['instance_id']:
            _fail('scope contains a different instance')
    if task_permissions:
        if not task_permissions == set(permissions):
            _fail('task permissions must be the only permissions')
        if 'RESOURCE_INSTANCE' not in kinds or 'RESOURCE_MODEL' in kinds:
            _fail('task sealed file requires INSTANCE scope (and optional INSTALLATION)')
        if kinds - {'RESOURCE_INSTANCE', 'RESOURCE_INSTALLATION'}:
            _fail('task sealed file has an unrelated scope kind')
    elif model_permissions:
        if not model_permissions == set(permissions):
            _fail('model permissions must be the only permissions')
        model_entries = [entry for entry in scope if entry['kind'] == 'RESOURCE_MODEL']
        if len(model_entries) != 1 or len(scope) != 1:
            _fail('model sealed file requires exactly one MODEL scope and no other scope')
        if expected_model is not None and model_entries[0]['id'] != expected_model:
            _fail('scope model does not match requested model')
    else:
        _fail('permissions are not whitelisted')
    issued_at, expires_at = document['issued_at'], document['expires_at']
    if (isinstance(issued_at, bool) or isinstance(expires_at, bool) or
            not isinstance(issued_at, int) or not isinstance(expires_at, int)):
        _fail('issued_at/expires_at must be integer epoch seconds')
    ttl = expires_at - issued_at
    if document['ttl_seconds'] != ttl or ttl <= 0 or ttl > max_ttl:
        _fail('TTL must be positive and <=56s')
    claims = decode_proof(document['delegation_proof'])
    if claims.get('aud') != AUDIENCE:
        _fail('proof audience mismatch')
    if claims.get('iat') != issued_at or claims.get('exp') != expires_at:
        _fail('proof timestamps do not match envelope')
    if claims.get('sub') is None:
        _fail('proof subject is missing')
    current = int(time.time()) if now is None else int(now)
    if expires_at <= current:
        _fail('delegation file is expired')
    return document


def _check_existing_target(target: str, owner_uid: int) -> None:
    try:
        info = os.lstat(target)
    except FileNotFoundError:
        return
    except OSError as error:
        raise SealedDelegationError('cannot stat target') from error
    if stat.S_ISLNK(info.st_mode):
        _fail('target must not be a symbolic link')
    if not stat.S_ISREG(info.st_mode):
        _fail('target is not a regular file')
    if info.st_uid != owner_uid:
        _fail('target owner mismatch')
    if stat.S_IMODE(info.st_mode) != 0o600:
        _fail('target mode must be 0600')


def atomic_write_sealed(path: str, document: dict, *, owner_uid: int | None = None) -> None:
    """Validate and atomically replace the sealed file; never follows a symlink target."""
    validate_document(document)
    target = os.path.abspath(path)
    directory = os.path.dirname(target)
    if not directory:
        _fail('target has no parent directory')
    try:
        directory_info = os.lstat(directory)
    except OSError as error:
        raise SealedDelegationError('output directory is unavailable') from error
    if stat.S_ISLNK(directory_info.st_mode) or not stat.S_ISDIR(directory_info.st_mode):
        _fail('output directory must be a real directory')
    expected_owner = os.geteuid() if owner_uid is None else owner_uid
    if directory_info.st_uid != expected_owner:
        _fail('output directory owner mismatch')
    _check_existing_target(target, expected_owner)
    payload = json.dumps(document, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    if len(payload) > MAX_FILE_BYTES:
        _fail('sealed document exceeds size limit')
    temporary = os.path.join(directory, '.instance-delegation.' + os.urandom(12).hex() + '.tmp')
    descriptor = None
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o600)
        os.write(descriptor, payload)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = None
        os.replace(temporary, target)
        directory_descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    info = os.lstat(target)
    if (not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode) or
            info.st_uid != expected_owner or stat.S_IMODE(info.st_mode) != 0o600):
        _fail('sealed file post-write verification failed')


def read_sealed_delegation(path: str, *, expected_instance: str | None = None,
                           expected_installation: str | None = None,
                           expected_model: str | None = None,
                           expected_owner: int | None = None, now: float | None = None,
                           max_ttl: int = MAX_TTL_SECONDS) -> dict:
    """Strict Runtime-side reader. It validates the envelope; it never verifies the signature."""
    target = os.path.abspath(path)
    expected_owner = os.geteuid() if expected_owner is None else expected_owner
    try:
        info = os.lstat(target)
    except OSError as error:
        raise SealedDelegationError('sealed file is unavailable') from error
    if stat.S_ISLNK(info.st_mode):
        _fail('sealed file must not be a symbolic link')
    if not stat.S_ISREG(info.st_mode):
        _fail('sealed file is not a regular file')
    if info.st_uid != expected_owner:
        _fail('sealed file owner mismatch')
    if stat.S_IMODE(info.st_mode) != 0o600:
        _fail('sealed file mode must be 0600')
    if info.st_size <= 0 or info.st_size > MAX_FILE_BYTES:
        _fail('sealed file size is invalid')
    try:
        with open(target, 'r', encoding='utf-8') as handle:
            document = json.load(handle)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SealedDelegationError('sealed file is not valid JSON') from error
    return validate_document(document, expected_instance=expected_instance,
                             expected_installation=expected_installation,
                             expected_model=expected_model, now=now, max_ttl=max_ttl)


def file_name(instance_id: str, kind: str = 'task') -> str:
    suffix = '' if kind == 'task' else '-' + kind
    return 'instance-delegation-' + instance_id + suffix + '.json'
