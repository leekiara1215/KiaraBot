#!/usr/bin/env python3
"""Bootstrap credential deployment helper: legacy manual store plus controlled-recipient verify.

Two deployment shapes share the same file convention
(``<directory>/<peer_id>.bootstrap``: regular file, mode 0600, no symlinks):

* Legacy/manual: Runtime/CreateInstance and Runtime/BindAdapter return a one-time credential in the
  create/bind response; this helper turns that captured response into the Host file. The credential
  is read from stdin (default) or a 0600 response file; it is never printed, logged, passed in argv
  or persisted anywhere except the target file.
* Controlled BOOT-04 delivery: Control writes the file directly through its SecretDeliveryPort and
  keeps no-secret ``.delivery.<peer_id>.intent``/``.receipt`` metadata beside it. This directory is
  mounted read-only into the consumer. ``verify --recipients <registry> --managed`` cross-checks the
  fixed offline recipient registry, the 0600 secret file and the sink receipt digest/version;
  ``store`` refuses such managed directories so a manual response can never impersonate Control.

Recipient registry files are offline deployment configuration, never secrets: they map a fixed
recipient_id to an exact resource/peer/owner. The helper never accepts a path, URL or command from
a delivery request, and never prints a secret, digest or environment value.

Rotation stays explicit (P14 file-mount policy): a legacy ``store`` with different material needs
``--replace``; a controlled recipient is rotated only by an explicitly authorized Runtime reissue
and a new Control delivery. ``prune`` never removes a registered active recipient.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

INSTANCE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z")
TEMP_PREFIX = ".target-host-credential-"
DELIVERY_PREFIX = ".delivery."
DELIVERY_SUFFIXES = (".intent", ".receipt", ".lock")
RESOURCE_KINDS = ("account", "instance")
MAXIMUM_RECIPIENTS = 127
MAXIMUM_METADATA_BYTES = 4096


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
    if information.st_nlink != 1:
        fail("UNSAFE_TARGET", "credential file must not have hard links")
    if information.st_size < 1 or information.st_size > 4096:
        fail("UNSAFE_TARGET", "credential file size is out of bounds")
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
    if args.recipients:
        recipients = load_recipients(args.recipients)
        if any(recipient["peer_id"] == instance for recipient in recipients):
            fail(
                "CONTROLLED_RECIPIENT",
                "registered controlled recipient is delivered by Control; manual store is not allowed",
                3,
            )
    if has_delivery_metadata(directory, instance):
        fail(
            "MANAGED_RECIPIENT",
            "Control-managed delivery metadata exists; manual store cannot overwrite it",
            3,
        )
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
    if args.recipients is not None:
        return prune_managed(directory, owner, args)
    if not args.active:
        fail("MISSING_ACTIVE", "--active or --recipients is required")
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


def prune_managed(directory: Path, owner: tuple[int, int] | None,
                  args: argparse.Namespace) -> int:
    """Controlled recipient directories: never remove a registered recipient, fail closed on others."""
    recipients = load_recipients(args.recipients)
    mapped = {recipient["peer_id"]: recipient for recipient in recipients}
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
            removed += 1
            continue
        if entry.name.startswith(DELIVERY_PREFIX) or not entry.name.endswith(".bootstrap"):
            continue
        peer = entry.name[: -len(".bootstrap")]
        if not INSTANCE_ID.fullmatch(peer):
            fail("UNSAFE_TARGET", "unexpected bootstrap file name in credential directory")
        if peer not in mapped:
            fail(
                "UNEXPECTED_RECIPIENT",
                "installed credential does not belong to a registered recipient; refusing to delete",
            )
        recipient = mapped[peer]
        recipient_owner = (
            expected_owner(recipient["owner"]) if recipient["owner"] else owner
        )
        inspect_existing(entry, recipient_owner)
    print("PRUNED 0")
    return 0


def sha256_hex(material: str) -> str:
    return hashlib.sha256(material.encode("utf-8")).hexdigest()


def strict_secret_text(path: Path, owner: tuple[int, int] | None) -> str:
    text = inspect_existing(path, owner)
    if text is None:
        fail("MISSING_CREDENTIAL", "installed credential file is missing")
    if len(text) != 64 or any(
        character not in "0123456789abcdef" for character in text
    ):
        fail("INVALID_CREDENTIAL", "bootstrap credential file is not 64 lowercase hex characters")
    return text


def load_recipients(value: str) -> list[dict]:
    """Offline fixed-recipient registry. Never contains a secret or a request-supplied value."""
    path = Path(value)
    try:
        information = path.lstat()
    except FileNotFoundError:
        fail("MISSING_RECIPIENTS", "recipient registry file does not exist")
    if stat.S_ISLNK(information.st_mode) or not stat.S_ISREG(information.st_mode):
        fail("INVALID_RECIPIENTS", "recipient registry must be a regular file")
    if stat.S_IMODE(information.st_mode) & 0o022:
        fail("INVALID_RECIPIENTS", "recipient registry must not be group/world writable")
    try:
        document = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        fail("INVALID_RECIPIENTS", "recipient registry is not readable JSON")
    if (
        not isinstance(document, dict)
        or document.get("schema") != 1
        or not isinstance(document.get("recipients"), list)
        or not 1 <= len(document["recipients"]) <= MAXIMUM_RECIPIENTS
    ):
        fail("INVALID_RECIPIENTS", "recipient registry shape is invalid")
    result: list[dict] = []
    recipient_ids: set[str] = set()
    peers: set[str] = set()
    for item in document["recipients"]:
        if not isinstance(item, dict):
            fail("INVALID_RECIPIENTS", "recipient entry must be an object")
        recipient_id = item.get("recipient_id")
        resource_kind = item.get("resource_kind")
        resource_id = item.get("resource_id")
        peer_id = item.get("peer_id")
        if not isinstance(recipient_id, str) or not INSTANCE_ID.fullmatch(recipient_id):
            fail("INVALID_RECIPIENTS", "recipient id is invalid")
        if resource_kind not in RESOURCE_KINDS:
            fail("INVALID_RECIPIENTS", "recipient resource kind is invalid")
        if not isinstance(resource_id, str) or not INSTANCE_ID.fullmatch(resource_id):
            fail("INVALID_RECIPIENTS", "recipient resource id is invalid")
        if not isinstance(peer_id, str) or not INSTANCE_ID.fullmatch(peer_id):
            fail("INVALID_RECIPIENTS", "recipient peer id is invalid")
        if recipient_id in recipient_ids or peer_id in peers:
            fail("INVALID_RECIPIENTS", "recipient registry contains duplicates")
        recipient_ids.add(recipient_id)
        peers.add(peer_id)
        owner_value = item.get("owner")
        if owner_value is not None and not isinstance(owner_value, str):
            fail("INVALID_RECIPIENTS", "recipient owner must be a string")
        entry_directory = item.get("directory")
        if entry_directory is not None:
            if not isinstance(entry_directory, str) or not Path(entry_directory).is_absolute():
                fail("INVALID_RECIPIENTS", "recipient directory must be absolute")
            # The Control-side directory namespace differs from the consumer's read-only mount
            # path, so the helper verifies the directory it was pointed at rather than equating
            # the two path namespaces. Absolute-path syntax is still enforced above.
            del entry_directory
        result.append(
            {
                "recipient_id": recipient_id,
                "resource_kind": resource_kind,
                "resource_id": resource_id,
                "peer_id": peer_id,
                "owner": owner_value,
            }
        )
    return result


def read_delivery_metadata(directory: Path, peer: str, suffix: str) -> dict | None:
    """Read the sink's no-secret intent/receipt. Metadata stays Control-owned and may be absent."""
    target = directory / (DELIVERY_PREFIX + peer + suffix)
    try:
        information = target.lstat()
    except FileNotFoundError:
        return None
    except OSError:
        fail("UNREADABLE_METADATA", "delivery metadata cannot be inspected")
    if stat.S_ISLNK(information.st_mode) or not stat.S_ISREG(information.st_mode):
        fail("UNSAFE_METADATA", "delivery metadata must be a regular file")
    if stat.S_IMODE(information.st_mode) & 0o077:
        fail("UNSAFE_METADATA", "delivery metadata must be 0600")
    if information.st_size < 1 or information.st_size > MAXIMUM_METADATA_BYTES:
        fail("UNSAFE_METADATA", "delivery metadata size is out of bounds")
    try:
        document = json.loads(target.read_text())
    except (OSError, json.JSONDecodeError):
        fail("UNREADABLE_METADATA", "delivery metadata cannot be read")
    expected_keys = {
        "schema",
        "delivery_id",
        "resource_kind",
        "resource_id",
        "recipient_id",
        "credential_version",
        "digest",
        "receipt_id",
    }
    if not isinstance(document, dict) or set(document) != expected_keys:
        fail("INVALID_METADATA", "delivery metadata shape is invalid")
    if document["schema"] != 1 or document["resource_kind"] not in RESOURCE_KINDS:
        fail("INVALID_METADATA", "delivery metadata schema/kind is invalid")
    for key in ("delivery_id", "resource_id", "recipient_id"):
        if not isinstance(document[key], str) or not INSTANCE_ID.fullmatch(document[key]):
            fail("INVALID_METADATA", "delivery metadata identity is invalid")
    if (
        not isinstance(document["credential_version"], int)
        or isinstance(document["credential_version"], bool)
        or not 0 < document["credential_version"] < 2**64
    ):
        fail("INVALID_METADATA", "delivery metadata credential version is invalid")
    if not isinstance(document["digest"], str) or len(document["digest"]) != 64 or any(
        character not in "0123456789abcdef" for character in document["digest"]
    ):
        fail("INVALID_METADATA", "delivery metadata digest is invalid")
    if (
        not isinstance(document["receipt_id"], str)
        or len(document["receipt_id"]) > 128
        or (" " in document["receipt_id"])
    ):
        fail("INVALID_METADATA", "delivery metadata receipt is invalid")
    return document


def managed_evidence(directory: Path, recipient: dict, secret: str) -> tuple[int, str]:
    """Cross-check the installed version against the sink's no-secret receipt/intent."""
    receipt = read_delivery_metadata(directory, recipient["peer_id"], ".receipt")
    intent = read_delivery_metadata(directory, recipient["peer_id"], ".intent")
    if receipt is None and intent is None:
        fail("MISSING_RECEIPT", "managed recipient has no delivery intent or receipt")
    record = receipt if receipt is not None else intent
    assert record is not None
    if receipt is not None and intent is not None:
        if intent["credential_version"] > receipt["credential_version"]:
            fail("INVALID_METADATA", "delivery intent is newer than its receipt")
    if (
        record["recipient_id"] != recipient["recipient_id"]
        or record["resource_kind"] != recipient["resource_kind"]
        or record["resource_id"] != recipient["resource_id"]
    ):
        fail("RECIPIENT_MISMATCH", "installed delivery belongs to another registered recipient")
    if sha256_hex(secret) != record["digest"]:
        fail(
            "VERSION_MISMATCH",
            "installed credential is not the version recorded by the delivery receipt",
            3,
        )
    state = "delivered" if receipt is not None else "pending"
    return record["credential_version"], state


def directory_is_read_only(directory: Path) -> bool:
    try:
        if os.statvfs(directory).f_flag & getattr(os, "ST_RDONLY", 1 << 0):
            return True
    except OSError:
        pass
    return not os.access(directory, os.W_OK)


def has_delivery_metadata(directory: Path, peer: str) -> bool:
    for suffix in (".intent", ".receipt"):
        try:
            (directory / (DELIVERY_PREFIX + peer + suffix)).lstat()
            return True
        except FileNotFoundError:
            continue
        except OSError:
            return True
    return False


def verify(args: argparse.Namespace) -> int:
    directory = safe_directory(args.directory)
    if args.require_read_only and not directory_is_read_only(directory):
        fail("DIRECTORY_NOT_READ_ONLY", "consumer credential directory must be a read-only mount")
    base_owner = expected_owner(args.owner)
    if args.recipients is not None:
        return verify_managed(directory, base_owner, args)
    if args.managed:
        fail("INVALID_ARGUMENTS", "--managed requires --recipients")
    count = 0
    for entry in sorted(directory.iterdir(), key=lambda item: item.name):
        if not entry.name.endswith(".bootstrap"):
            continue
        instance = entry.name[: -len(".bootstrap")]
        if not INSTANCE_ID.fullmatch(instance):
            fail("UNSAFE_TARGET", "unexpected bootstrap file name in credential directory")
        inspect_existing(entry, base_owner)
        count += 1
    print(f"VERIFIED {count}")
    return 0


def verify_managed(directory: Path, base_owner: tuple[int, int] | None,
                   args: argparse.Namespace) -> int:
    recipients = load_recipients(args.recipients)
    mapped = {recipient["peer_id"]: recipient for recipient in recipients}
    seen: set[str] = set()
    verified = 0
    for entry in sorted(directory.iterdir(), key=lambda item: item.name):
        if entry.name.startswith(TEMP_PREFIX) or entry.name.startswith(DELIVERY_PREFIX):
            continue
        if not entry.name.endswith(".bootstrap"):
            fail("UNEXPECTED_FILE", "credential directory contains an unexpected file")
        peer = entry.name[: -len(".bootstrap")]
        if not INSTANCE_ID.fullmatch(peer):
            fail("UNSAFE_TARGET", "unexpected bootstrap file name in credential directory")
        if peer not in mapped:
            fail(
                "UNEXPECTED_RECIPIENT",
                "installed credential does not belong to a registered recipient",
            )
        seen.add(peer)
        recipient = mapped[peer]
        recipient_owner = (
            expected_owner(recipient["owner"]) if recipient["owner"] else base_owner
        )
        secret = strict_secret_text(entry, recipient_owner)
        version = 0
        state = "unverified"
        if args.managed:
            version, state = managed_evidence(directory, recipient, secret)
        print(f"RECIPIENT {peer} version={version} state={state}")
        verified += 1
    missing = sorted(set(mapped) - seen)
    if missing:
        fail(
            "MISSING_CREDENTIAL",
            "registered recipient has no installed credential: " + ",".join(missing),
        )
    print(f"VERIFIED {verified}")
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
    store_command.add_argument(
        "--recipients",
        default=None,
        help="offline recipient registry; refuses peers delivered by Control",
    )
    store_command.set_defaults(handler=store)

    prune_command = commands.add_parser("prune", help="remove credentials for inactive instances")
    prune_command.add_argument("--directory", required=True)
    prune_command.add_argument("--active", default=None, help="JSON string array or newline list")
    prune_command.add_argument("--owner", default=None)
    prune_command.add_argument("--dry-run", action="store_true")
    prune_command.add_argument(
        "--recipients",
        default=None,
        help="offline recipient registry; registered recipients are never pruned",
    )
    prune_command.set_defaults(handler=prune)

    verify_command = commands.add_parser("verify", help="verify files are 0600, regular and owned")
    verify_command.add_argument("--directory", required=True)
    verify_command.add_argument("--owner", default=None)
    verify_command.add_argument(
        "--recipients",
        default=None,
        help="offline recipient registry; strict fixed-recipient directory verification",
    )
    verify_command.add_argument(
        "--managed",
        action="store_true",
        help="also verify the sink receipt/intent digest and credential version",
    )
    verify_command.add_argument(
        "--require-read-only",
        action="store_true",
        help="require the consumer credential directory to be a read-only mount",
    )
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
