#!/usr/bin/env python3
"""P08d managed-instance bootstrap credential deployment helper.

Runtime/CreateInstance and Runtime/BindAdapter return a one-time plugin/adapter bootstrap
credential in the create/bind response (plaintext is never stored by Runtime). This helper turns
that captured response into the Host credential file convention documented in doc/08:

    <credential_directory>/<instance_id>.bootstrap   mode 0600, regular file, no symlinks

It is deliberately a deployment-side file helper rather than a protocol feature: HostBridge has
no credential delivery frame and Runtime must not write the Host filesystem. The credential is
read from stdin (default) or a 0600 response file; it is never printed, logged, passed in argv or
persisted anywhere except the target file. Rotation/cleanup is explicit and manual (P14 file-mount
policy): rerun with --replace for a new credential, and run prune with the current active instance
list after deleting/stopping instances.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

INSTANCE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z")
TEMP_PREFIX = ".target-host-credential-"


class HelperError(Exception):
    def __init__(self, code: str, message: str, exit_code: int = 2):
        super().__init__(message)
        self.code = code
        self.exit_code = exit_code


def fail(code: str, message: str, exit_code: int = 2) -> "None":
    raise HelperError(code, message, exit_code)


def safe_directory(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        fail("INVALID_DIRECTORY", "credential directory must be an absolute path")
    try:
        information = path.lstat()
    except FileNotFoundError:
        fail("MISSING_DIRECTORY", "credential directory does not exist")
    if stat.S_ISLNK(information.st_mode) or not stat.S_ISDIR(information.st_mode):
        fail("INVALID_DIRECTORY", "credential directory must be a real directory")
    if information.st_mode & stat.S_IWOTH:
        fail("INVALID_DIRECTORY", "credential directory must not be world-writable")
    return path


def expected_owner(value: str | None) -> tuple[int, int] | None:
    if not value:
        return None
    parts = value.split(":", 1)
    try:
        import pwd
        import grp

        user = pwd.getpwnam(parts[0])
        group = grp.getgrnam(parts[1]) if len(parts) > 1 else None
    except (KeyError, ImportError):
        fail("INVALID_OWNER", "owner user/group is not resolvable")
    return user.pw_uid, group.gr_gid if group else user.pw_gid


def read_response(path: str | None) -> dict:
    if path in (None, "-"):
        raw = sys.stdin.read()
    else:
        candidate = Path(path)
        try:
            information = candidate.lstat()
        except FileNotFoundError:
            fail("MISSING_RESPONSE", "response file does not exist")
        if stat.S_ISLNK(information.st_mode) or not stat.S_ISREG(information.st_mode):
            fail("INVALID_RESPONSE", "response file must be a regular file")
        if stat.S_IMODE(information.st_mode) & 0o077:
            fail("INVALID_RESPONSE", "response file must be 0600 (contains the credential)")
        raw = candidate.read_text()
    try:
        document = json.loads(raw)
    except json.JSONDecodeError as error:
        fail("INVALID_RESPONSE", "response is not JSON: " + str(error))
    if not isinstance(document, dict):
        fail("INVALID_RESPONSE", "response must be a JSON object")
    return document


def extract(document: dict, override: str | None) -> tuple[str, str]:
    credential = document.get("bootstrap_credential")
    if not isinstance(credential, str) or not credential:
        fail("MISSING_CREDENTIAL", "response has no one-time bootstrap_credential")
    if len(credential) < 32 or len(credential) > 4096 or any(
        character in credential for character in ("\n", "\r", "\0")
    ):
        fail("INVALID_CREDENTIAL", "bootstrap credential is not a bounded opaque value")
    if override:
        instance = override
    else:
        instance = ""
        instance_record = document.get("instance")
        if isinstance(instance_record, dict) and isinstance(instance_record.get("instance_id"), str):
            instance = instance_record["instance_id"]
        binding = document.get("binding")
        if not instance and isinstance(binding, dict) and isinstance(
            binding.get("adapter_instance_id"), str
        ):
            instance = binding["adapter_instance_id"]
        if not instance:
            fail("MISSING_INSTANCE", "response has no instance/binding instance id")
    if not INSTANCE_ID.fullmatch(instance):
        fail("INVALID_INSTANCE", "instance id contains unsupported characters")
    return instance, credential


def target_path(directory: Path, instance: str) -> Path:
    return directory / (instance + ".bootstrap")


def inspect_existing(path: Path, owner: tuple[int, int] | None) -> str | None:
    """Returns the existing credential text or None when absent. Rejects unsafe files."""
    try:
        information = path.lstat()
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(information.st_mode) or not stat.S_ISREG(information.st_mode):
        fail("UNSAFE_TARGET", "existing credential path is not a regular file")
    if stat.S_IMODE(information.st_mode) != 0o600:
        fail("UNSAFE_TARGET", "existing credential file must be 0600")
    if owner is not None and (information.st_uid, information.st_gid) != owner:
        fail("UNSAFE_TARGET", "existing credential file owner does not match --owner")
    try:
        return path.read_text().rstrip("\n").rstrip("\r")
    except OSError:
        fail("UNREADABLE_TARGET", "existing credential file cannot be read")


def write_atomic(directory: Path, path: Path, credential: str, owner: tuple[int, int] | None) -> None:
    descriptor, temporary = tempfile.mkstemp(prefix=TEMP_PREFIX, dir=str(directory))
    if descriptor < 0:
        fail("WRITE_FAILED", "cannot create temporary credential file")
    try:
        try:
            if owner is not None:
                os.fchown(descriptor, owner[0], owner[1])
            os.fchmod(descriptor, 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                descriptor = -1
                stream.write(credential)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
            temporary = ""
            directory_descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
        finally:
            if descriptor >= 0:
                os.close(descriptor)
    except OSError:
        fail("WRITE_FAILED", "cannot write credential file")
    finally:
        if temporary:
            try:
                os.unlink(temporary)
            except OSError:
                pass


def store(args: argparse.Namespace) -> int:
    directory = safe_directory(args.directory)
    owner = expected_owner(args.owner)
    document = read_response(args.response)
    instance, credential = extract(document, args.instance_id)
    path = target_path(directory, instance)
    existing = inspect_existing(path, owner)
    if existing == credential:
        print(f"UNCHANGED {instance}")
        return 0
    if existing is not None and not args.replace:
        fail(
            "REFUSED_ROTATION",
            "a different credential already exists; rerun with --replace after stopping the instance",
            3,
        )
    if args.dry_run:
        print(f"{'ROTATE' if existing is not None else 'STORE'} {instance}")
        return 0
    write_atomic(directory, path, credential, owner)
    print(f"{'ROTATED' if existing is not None else 'STORED'} {instance}")
    return 0


def active_instances(path: str) -> set[str]:
    candidate = Path(path)
    try:
        raw = candidate.read_text()
    except OSError:
        fail("MISSING_ACTIVE", "active instance list cannot be read")
    try:
        document = json.loads(raw)
        values = document.get("instances") if isinstance(document, dict) else document
    except json.JSONDecodeError:
        values = [line.strip() for line in raw.splitlines() if line.strip()]
    if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
        fail("INVALID_ACTIVE", "active instance list must be a JSON string array or newline list")
    for value in values:
        if not INSTANCE_ID.fullmatch(value):
            fail("INVALID_ACTIVE", "active instance list contains an invalid instance id")
    return set(values)


def prune(args: argparse.Namespace) -> int:
    directory = safe_directory(args.directory)
    owner = expected_owner(args.owner)
    active = active_instances(args.active)
    removed = 0
    for entry in sorted(directory.iterdir(), key=lambda item: item.name):
        if entry.name.startswith(TEMP_PREFIX):
            if args.dry_run:
                print(f"REMOVE_TEMP {entry.name}")
            else:
                try:
                    entry.unlink()
                except OSError:
                    fail("PRUNE_FAILED", "stale temporary file cannot be removed")
            continue
        if not entry.name.endswith(".bootstrap"):
            continue
        instance = entry.name[: -len(".bootstrap")]
        if not INSTANCE_ID.fullmatch(instance):
            fail("UNSAFE_TARGET", "unexpected bootstrap file name in credential directory")
        inspect_existing(entry, owner)
        if instance in active:
            continue
        if args.dry_run:
            print(f"PRUNE {instance}")
        else:
            try:
                entry.unlink()
            except OSError:
                fail("PRUNE_FAILED", "inactive credential file cannot be removed")
            print(f"REMOVED {instance}")
        removed += 1
    print(f"PRUNED {removed}")
    return 0


def verify(args: argparse.Namespace) -> int:
    directory = safe_directory(args.directory)
    owner = expected_owner(args.owner)
    count = 0
    for entry in sorted(directory.iterdir(), key=lambda item: item.name):
        if not entry.name.endswith(".bootstrap"):
            continue
        instance = entry.name[: -len(".bootstrap")]
        if not INSTANCE_ID.fullmatch(instance):
            fail("UNSAFE_TARGET", "unexpected bootstrap file name in credential directory")
        inspect_existing(entry, owner)
        count += 1
    print(f"VERIFIED {count}")
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    store_command = commands.add_parser("store", help="write one captured create/bind response")
    store_command.add_argument("--directory", required=True)
    store_command.add_argument("--response", default="-", help="response JSON file (0600) or - for stdin")
    store_command.add_argument("--instance-id", default=None, help="override instance id")
    store_command.add_argument("--owner", default=None, help="Host user[:group] owner for the file")
    store_command.add_argument("--replace", action="store_true", help="rotate an existing credential")
    store_command.add_argument("--dry-run", action="store_true")
    store_command.set_defaults(handler=store)

    prune_command = commands.add_parser("prune", help="remove credentials for inactive instances")
    prune_command.add_argument("--directory", required=True)
    prune_command.add_argument("--active", required=True, help="JSON string array or newline list")
    prune_command.add_argument("--owner", default=None)
    prune_command.add_argument("--dry-run", action="store_true")
    prune_command.set_defaults(handler=prune)

    verify_command = commands.add_parser("verify", help="verify files are 0600, regular and owned")
    verify_command.add_argument("--directory", required=True)
    verify_command.add_argument("--owner", default=None)
    verify_command.set_defaults(handler=verify)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        return args.handler(args)
    except HelperError as error:
        print(f"ERROR {error.code}: {error}", file=sys.stderr)
        return error.exit_code


if __name__ == "__main__":
    sys.exit(main())
