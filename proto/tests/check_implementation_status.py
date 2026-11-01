#!/usr/bin/env python3
"""Check declared contract surfaces against their v1 implementation status.

The registry proto/tests/contract_status.json is the reviewed source of truth for
which payloads, fields, enum values, WebSocket channels/events, REST operations
and cross-contract semantic rules are implemented, not implemented, or
deprecated within a named execution scope. schema v2 preserves historical categories
as non-production reference facts; target production defaults to not_implemented and
requires explicit overrides backed by current source and target tests. Logical evidence
paths follow only the byte-verified core/reference manifest.

This gate runs without protoc/CMake and enforces that:
  1. every Protobuf message field and oneof payload is registered;
  2. every registered Protobuf item that is not implemented carries an inline
     ``CONTRACT_STATUS`` annotation in the .proto source;
  3. AsyncAPI channels/events and OpenAPI operationIds are fully registered;
  4. api/*.yaml top-level ``x-contract-status`` lists match the registry, and a
     channel marked unavailable is annotated on the channel itself;
  5. every evidence path referenced by the registry exists in the repository;
  6. OpenAPI schema enum values never advertise values outside the implemented
     set (contract claims must not exceed the implementation);
  7. doc/01-contracts.md lists every unavailable/deprecated contract item in its
     implementation-status table with the matching status;
  8. the frozen plugin capability/permission/subscription vocabulary appears
     both in doc/01-contracts.md and in the implementation source.

Run: python3 proto/tests/check_implementation_status.py
"""

import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from evidence_scopes import EvidenceResolver, effective_status, validate_scopes, TARGET_SOURCE_ROOTS

ROOT = Path(__file__).resolve().parents[2]
PROTO_DIR = ROOT / "proto" / "kiarabot" / "v1"
REGISTRY = ROOT / "proto" / "tests" / "contract_status.json"
DOC_CONTRACTS = ROOT / "doc" / "01-contracts.md"
OPENAPI = ROOT / "api" / "openapi.yaml"
ASYNCAPI = ROOT / "api" / "asyncapi.yaml"

STATUS_VALUES = {"implemented", "not_implemented", "deprecated"}
FIELD_PATTERN = re.compile(
    r"(?:repeated\s+|optional\s+)?(map<[^>]+>|[\w.]+)\s+(\w+)\s*=\s*(\d+)\s*;"
)
STATUS_ANNOTATION = re.compile(r"CONTRACT_STATUS:\s*(implemented|not_implemented|deprecated)")
NESTED_BLOCK = re.compile(r"\b(?:message|enum)\s+\w+\s*\{")
STATUS_SECTION_START = "<!-- contract-status:start -->"
STATUS_SECTION_END = "<!-- contract-status:end -->"
VOCABULARY_START = "<!-- vocabulary:start -->"
VOCABULARY_END = "<!-- vocabulary:end -->"
IMPLEMENTATION_SUFFIXES = {".cpp", ".cc", ".hpp", ".h", ".py"}
IMPLEMENTATION_ROOTS = ("core", "adapters", "sdk")
IMPLEMENTATION_SKIP_DIRS = {".git", ".venv", "build", "node_modules", "__pycache__", ".mypy_cache"}


def strip_comments(source: str) -> str:
    """Blank // comments while keeping character offsets stable for annotation lookup."""
    return re.sub(r"//[^\n]*", lambda match: " " * len(match.group(0)), source)


def block_end(source: str, open_brace: int) -> int:
    depth = 0
    for offset in range(open_brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return offset + 1
    raise ValueError("unbalanced braces in .proto source")


def blank_ranges(source: str, ranges: List[Tuple[int, int]]) -> str:
    characters = list(source)
    for start, end in ranges:
        for offset in range(start, end):
            if characters[offset] != "\n":
                characters[offset] = " "
    return "".join(characters)


def comments_before(source: str, offset: int) -> str:
    """Contiguous // comment lines immediately above the line containing offset."""
    line_start = source.rfind("\n", 0, offset) + 1
    position = line_start
    collected: List[str] = []
    while position > 0:
        previous_end = position - 1
        previous_start = source.rfind("\n", 0, previous_end) + 1
        line = source[previous_start:previous_end].strip()
        if not line.startswith("//"):
            break
        collected.append(line)
        position = previous_start
    return "\n".join(reversed(collected))


def status_tokens(comment: str) -> Set[str]:
    return set(STATUS_ANNOTATION.findall(comment))


class ProtoModel:
    def __init__(self) -> None:
        self.messages: Dict[str, str] = {}
        self.oneofs: Dict[str, Dict[str, Dict[str, str]]] = {}
        self.fields: Dict[str, str] = {}
        self.field_annotations: Dict[str, Set[str]] = {}
        self.enums: Dict[str, Dict[str, int]] = {}
        self.enum_annotations: Dict[str, Set[str]] = {}

    def message_names(self) -> Set[str]:
        return set(self.messages)


def parse_proto_files() -> ProtoModel:
    model = ProtoModel()
    for path in sorted(PROTO_DIR.glob("*.proto")):
        source = path.read_text(encoding="utf-8")
        cleaned = strip_comments(source)
        blocks = []
        for match in re.finditer(r"\b(message|enum)\s+(\w+)\s*\{", cleaned):
            kind, name = match.group(1), match.group(2)
            end = block_end(cleaned, match.end() - 1)
            blocks.append({"kind": kind, "name": name, "start": match.start(),
                           "open": match.end(), "end": end})
        # Resolve parent message chains without relying on parse order.
        for block in blocks:
            parents = []
            for candidate in blocks:
                if candidate["kind"] != "message":
                    continue
                if candidate["start"] < block["start"] and candidate["end"] > block["end"]:
                    parents.append(candidate)
            parents.sort(key=lambda item: item["start"])
            block["dotted"] = ".".join([item["name"] for item in parents] + [block["name"]])
            if block["kind"] == "message":
                if block["dotted"] in model.messages:
                    raise ValueError(f"duplicate message {block['dotted']} in {path.name}")
                model.messages[block["dotted"]] = path.name
        for block in blocks:
            if block["kind"] == "message":
                nested = []
                for candidate in blocks:
                    if candidate is block or candidate["kind"] not in ("message", "enum"):
                        continue
                    if candidate["start"] > block["start"] and candidate["end"] < block["end"]:
                        nested.append((candidate["start"] - block["open"],
                                       candidate["end"] - block["open"]))
                body = cleaned[block["open"]:block["end"] - 1]
                blanked = blank_ranges(body, nested)
                oneofs: Dict[str, Dict[str, str]] = {}
                for oneof in re.finditer(r"\boneof\s+(\w+)\s*\{", blanked):
                    oneof_end = block_end(blanked, oneof.end() - 1)
                    members: Dict[str, str] = {}
                    for member in re.finditer(
                        r"(?m)^\s*([\w.]+)\s+(\w+)\s*=\s*(\d+)\s*;",
                        blanked[oneof.end():oneof_end - 1],
                    ):
                        members[member.group(2)] = member.group(1)
                    oneofs[oneof.group(1)] = members
                model.oneofs[block["dotted"]] = oneofs
                message_comment = comments_before(source, block["start"])
                for field in FIELD_PATTERN.finditer(blanked):
                    field_name = field.group(2)
                    key = f"{block['dotted']}.{field_name}"
                    if key in model.fields:
                        raise ValueError(f"duplicate field {key} in {path.name}")
                    model.fields[key] = field.group(1)
                    absolute = block["open"] + field.start()
                    annotations = status_tokens(comments_before(source, absolute))
                    annotations |= status_tokens(message_comment)
                    model.field_annotations[key] = annotations
            else:
                enum_comment = comments_before(source, block["start"])
                body = cleaned[block["open"]:block["end"] - 1]
                values: Dict[str, int] = {}
                for value in re.finditer(
                    r"(?m)^\s*([A-Z][A-Z0-9_]*)\s*=\s*(-?\d+)\s*(?:\[[^\]]*\])?\s*;", body
                ):
                    name = value.group(1)
                    if name in values:
                        raise ValueError(f"duplicate enum value {block['dotted']}.{name}")
                    values[name] = int(value.group(2))
                    absolute = block["open"] + value.start(1)
                    annotations = status_tokens(comments_before(source, absolute))
                    annotations |= status_tokens(enum_comment)
                    model.enum_annotations[f"{block['dotted']}.{name}"] = annotations
                model.enums[block["dotted"]] = values
    if not model.messages or not model.enums:
        raise ValueError("no Protobuf definitions found")
    return model


def read_registry() -> dict:
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


def read_doc_section(path: Path, start: str, end: str) -> str:
    text = path.read_text(encoding="utf-8")
    if start not in text or end not in text:
        raise ValueError(f"missing section markers {start} / {end} in {path.relative_to(ROOT)}")
    return text.split(start, 1)[1].split(end, 1)[0]


def parse_yaml_list(block: str, key: str) -> List[str]:
    match = re.search(rf"(?m)^\s+{key}:\s*\[([^\]]*)\]", block)
    if not match:
        return []
    return [item.strip().strip("'\"") for item in match.group(1).split(",") if item.strip()]


def parse_contract_status_extension(path: Path, block: str) -> Tuple[List[str], List[str]]:
    not_implemented: List[str] = []
    deprecated: List[str] = []
    for match in re.finditer(r"(?m)^\s*([a-zA-Z_]+):\s*$", block):
        name = match.group(1)
        if name not in ("not_implemented", "deprecated"):
            continue
        section_start = match.end()
        following = re.search(r"(?m)^\s*[a-zA-Z_]+:\s*$", block[section_start:])
        section = block[section_start:section_start + following.start()] if following else block[section_start:]
        values = re.findall(r"(?m)^\s*-\s+([\w.\-]+)\s*$", section)
        if name == "not_implemented":
            not_implemented = values
        else:
            deprecated = values
    return not_implemented, deprecated


def parse_asyncapi(path: Path) -> Tuple[Set[str], Set[str], str]:
    source = path.read_text(encoding="utf-8")
    channel_section = source.split("\nchannels:\n", 1)[1].split("\ncomponents:\n", 1)[0]
    channels = set(re.findall(r"(?m)^  (\w+):\s*$", channel_section))
    events = set()
    for match in re.finditer(
        r"event_type:\s*\{\s*type:\s*string,\s*enum:\s*\[([^\]]+)\]\s*\}", source
    ):
        events.update(item.strip() for item in match.group(1).split(","))
    return channels, events, source


def parse_openapi(path: Path) -> Tuple[Set[str], str]:
    source = path.read_text(encoding="utf-8")
    operations = set(re.findall(r"(?m)^\s+operationId:\s*(\w+)\s*$", source))
    return operations, source


def parse_schema_enum(source: str, schema: str, field: str) -> Optional[List[str]]:
    marker = f"\n    {schema}:\n"
    if marker not in source:
        return None
    block = source.split(marker, 1)[1]
    following = re.search(r"(?m)^    \w+:\s*$", block)
    if following:
        block = block[:following.start()]
    match = re.search(
        rf"{field}:\s*\{{\s*type:\s*string,\s*enum:\s*\[([^\]]*)\][^}}]*\}}", block
    )
    if not match:
        return None
    return [item.strip().strip("'\"") for item in match.group(1).split(",") if item.strip()]


def implementation_text(scope="reference", root=ROOT) -> str:
    if scope not in {"reference", "target"}:
        raise ValueError("unknown implementation vocabulary scope")
    pieces: List[str] = []
    for root_name in IMPLEMENTATION_ROOTS:
        directory = root / root_name
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if path.suffix not in IMPLEMENTATION_SUFFIXES or not path.is_file():
                continue
            if any(part in IMPLEMENTATION_SKIP_DIRS for part in path.relative_to(root).parts):
                continue
            relative = path.relative_to(root).as_posix()
            if scope == "target" and not relative.startswith(TARGET_SOURCE_ROOTS):
                continue
            if (scope == "reference" and (root / "core/reference/manifest.json").is_file()
                    and relative.startswith("core/") and not relative.startswith("core/reference/")):
                continue
            if "tests" in path.relative_to(root).parts:
                continue
            pieces.append(path.read_text(encoding="utf-8", errors="ignore"))
    return "\n".join(pieces)


def validate_registry(registry: dict) -> None:
    if registry.get("schema_version") != 2:
        raise ValueError("contract_status.json schema_version must be 2 (explicit reference/target execution scopes)")
    if set(registry.get("status_values", [])) != STATUS_VALUES:
        raise ValueError("contract_status.json status_values must be implemented/not_implemented/deprecated")
    for category, entries in registry["categories"].items():
        for key, status in entries.items():
            if status not in STATUS_VALUES:
                raise ValueError(f"{category}:{key} has invalid status {status!r}")
    for key, status in registry["categories"]["proto_fields"].items():
        if not re.fullmatch(r"\w+\.\w+", key):
            raise ValueError(f"invalid proto field registry key {key}")


def oneof_type(proto: ProtoModel, message: str, oneof: str, member: str) -> str:
    try:
        return proto.oneofs[message][oneof][member]
    except KeyError as error:
        raise ValueError(f"missing oneof member {message}.{oneof}.{member}") from error


def build_covered_messages(registry: dict, proto: ProtoModel) -> Dict[str, str]:
    covered: Dict[str, str] = {}
    groups = (
        ("event_payloads", "EventEnvelope", "payload"),
        ("action_payloads", "ActionEnvelope", "payload"),
        ("host_operations", "HostRequest", "operation"),
        ("host_results", "HostResponse", "result"),
    )
    for category, message, oneof in groups:
        for key, status in registry["categories"][category].items():
            if status == "implemented":
                continue
            member = key.split(".", 1)[1]
            covered[oneof_type(proto, message, oneof, member)] = key
    return covered


def main() -> int:
    errors: List[str] = []

    def fail(message: str) -> None:
        errors.append(message)

    try:
        registry = read_registry()
        validate_registry(registry)
        evidence_resolver = EvidenceResolver(ROOT)
        validate_scopes(registry, evidence_resolver)
    except (ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"contract_status.json: {error}", file=sys.stderr)
        return 1

    proto = parse_proto_files()
    categories = registry["categories"]

    # 1. Protobuf coverage.
    if set(proto.messages) != set(categories["proto_messages"]):
        missing = sorted(set(proto.messages) - set(categories["proto_messages"]))
        extra = sorted(set(categories["proto_messages"]) - set(proto.messages))
        if missing:
            fail(f"unregistered Protobuf messages: {', '.join(missing)}")
        if extra:
            fail(f"registry cites nonexistent Protobuf messages: {', '.join(extra)}")
    if set(proto.fields) != set(categories["proto_fields"]):
        missing = sorted(set(proto.fields) - set(categories["proto_fields"]))
        extra = sorted(set(categories["proto_fields"]) - set(proto.fields))
        if missing:
            fail(f"unregistered Protobuf fields: {', '.join(missing)}")
        if extra:
            fail(f"registry cites nonexistent Protobuf fields: {', '.join(extra)}")
    expected_payloads = {
        "event_payloads": (proto.oneofs.get("EventEnvelope", {}).get("payload", {}), "EventEnvelope"),
        "action_payloads": (proto.oneofs.get("ActionEnvelope", {}).get("payload", {}), "ActionEnvelope"),
        "host_operations": (proto.oneofs.get("HostRequest", {}).get("operation", {}), "HostRequest"),
        "host_results": (proto.oneofs.get("HostResponse", {}).get("result", {}), "HostResponse"),
    }
    for category, (members, message) in expected_payloads.items():
        expected = {f"{message}.{name}" for name in members}
        observed = set(categories[category])
        if expected != observed:
            fail(
                f"{category} coverage differs: missing {sorted(expected - observed)}, "
                f"extra {sorted(observed - expected)}"
            )
        for key in expected & observed:
            field_key = key
            if categories[category][key] != categories["proto_fields"].get(field_key):
                fail(f"{category}:{key} disagrees with proto_fields:{field_key}")
            member = key.split(".", 1)[1]
            payload_type = oneof_type(proto, message, {
                "event_payloads": "payload",
                "action_payloads": "payload",
                "host_operations": "operation",
                "host_results": "result",
            }[category], member)
            payload_status = categories[category][key]
            message_status = categories["proto_messages"].get(payload_type)
            if message_status != payload_status:
                fail(
                    f"{category}:{key} is {payload_status} but its message "
                    f"{payload_type} is {message_status} in proto_messages"
                )

    # 2. Enum values coverage.
    enum_lookup: Dict[str, int] = {}
    for enum_name, values in proto.enums.items():
        for value_name, number in values.items():
            enum_lookup[f"{enum_name}.{value_name}"] = number
    for key in categories["enum_values"]:
        if key not in enum_lookup:
            fail(f"registry cites nonexistent enum value {key}")

    # 3. WebSocket/REST surface coverage.
    asyncapi_channels, asyncapi_events, asyncapi_source = parse_asyncapi(ASYNCAPI)
    expected_channels = {f"channels.{name}" for name in asyncapi_channels}
    if expected_channels != set(categories["channels"]):
        fail(
            f"channels coverage differs: missing {sorted(expected_channels - set(categories['channels']))}, "
            f"extra {sorted(set(categories['channels']) - expected_channels)}"
        )
    expected_events = {f"events.{name}" for name in asyncapi_events}
    if expected_events != set(categories["stream_events"]):
        fail(
            f"stream_events coverage differs: missing {sorted(expected_events - set(categories['stream_events']))}, "
            f"extra {sorted(set(categories['stream_events']) - expected_events)}"
        )
    openapi_operations, openapi_source = parse_openapi(OPENAPI)
    expected_operations = {f"operations.{name}" for name in openapi_operations}
    if expected_operations != set(categories["operations"]):
        fail(
            f"operations coverage differs: missing {sorted(expected_operations - set(categories['operations']))}, "
            f"extra {sorted(set(categories['operations']) - expected_operations)}"
        )

    covered_messages = build_covered_messages(registry, proto)
    doc_required: Set[str] = set()
    evidence_required: Set[str] = set()

    for category in ("event_payloads", "action_payloads", "host_operations", "host_results",
                     "enum_values", "channels", "stream_events", "operations", "semantic_rules"):
        for key, status in categories[category].items():
            if status == "implemented":
                continue
            doc_required.add(key)
            evidence_required.add(key)

    for key, status in categories["proto_fields"].items():
        if status == "implemented":
            continue
        message = key.split(".", 1)[0]
        if message in covered_messages:
            continue
        doc_required.add(key)
        evidence_required.add(key)

    # 4. Inline CONTRACT_STATUS annotations.
    for key, status in categories["proto_fields"].items():
        if status == "implemented":
            continue
        annotations = proto.field_annotations.get(key, set())
        if status not in annotations:
            fail(f"Protobuf field {key} is {status} but has no inline CONTRACT_STATUS: {status} comment")
    for key, status in categories["enum_values"].items():
        if status == "implemented":
            continue
        annotations = proto.enum_annotations.get(key, set())
        if status not in annotations:
            fail(f"enum value {key} is {status} but has no inline CONTRACT_STATUS: {status} comment")

    # 5. Evidence paths.
    evidence = registry.get("evidence", {})
    for key in sorted(evidence_required):
        paths = evidence.get(key)
        if not paths:
            fail(f"missing evidence for unavailable contract item {key}")
            continue
        for relative in paths:
            try:
                evidence_resolver.resolve(relative)
            except ValueError as error:
                fail(f"invalid evidence for {key}: {error}")
    for key, paths in evidence.items():
        for relative in paths:
            try:
                evidence_resolver.resolve(relative)
            except ValueError as error:
                fail(f"invalid evidence for {key}: {error}")
        known = (
            key in categories["proto_fields"]
            or any(key in categories[category] for category in categories)
        )
        if not known:
            fail(f"evidence cites unknown contract item {key}")

    # 6. api/*.yaml x-contract-status lists.
    try:
        openapi_block = openapi_source.split("\nx-contract-status:\n", 1)[1]
        openapi_block = openapi_block.split("\npaths:", 1)[0]
        openapi_not_implemented, openapi_deprecated = parse_contract_status_extension(
            OPENAPI, openapi_block)
    except IndexError:
        openapi_not_implemented, openapi_deprecated = [], []
    expected_openapi_not_implemented = sorted(
        key.split("operations.", 1)[1]
        for key, status in categories["operations"].items()
        if status == "not_implemented"
    )
    if sorted(openapi_not_implemented) != expected_openapi_not_implemented:
        fail(
            "OpenAPI x-contract-status.not_implemented differs from the registry: "
            f"expected {expected_openapi_not_implemented}, got {openapi_not_implemented}"
        )
    if openapi_deprecated:
        fail(f"OpenAPI x-contract-status.deprecated must stay empty, got {openapi_deprecated}")

    try:
        asyncapi_block = asyncapi_source.split("\nx-contract-status:\n", 1)[1]
        asyncapi_block = asyncapi_block.split("\nservers:", 1)[0]
        asyncapi_not_implemented, asyncapi_deprecated = parse_contract_status_extension(
            ASYNCAPI, asyncapi_block)
    except IndexError:
        asyncapi_not_implemented, asyncapi_deprecated = [], []
    expected_asyncapi_not_implemented: List[str] = []
    for key, status in categories["channels"].items():
        if status == "not_implemented":
            expected_asyncapi_not_implemented.append(key.split("channels.", 1)[1])
    for key, status in categories["stream_events"].items():
        if status == "not_implemented":
            expected_asyncapi_not_implemented.append(key.split("events.", 1)[1])
    if categories["semantic_rules"].get("semantic.stream.plugin_updated_health_state") != "implemented":
        expected_asyncapi_not_implemented.append("plugin.updated.health_state")
    if sorted(asyncapi_not_implemented) != sorted(expected_asyncapi_not_implemented):
        fail(
            "AsyncAPI x-contract-status.not_implemented differs from the registry: "
            f"expected {sorted(expected_asyncapi_not_implemented)}, got {asyncapi_not_implemented}"
        )
    if asyncapi_deprecated:
        fail(f"AsyncAPI x-contract-status.deprecated must stay empty, got {asyncapi_deprecated}")

    # Channel-level marker for unavailable channels.
    channel_section = asyncapi_source.split("\nchannels:\n", 1)[1].split("\ncomponents:\n", 1)[0]
    for key, status in categories["channels"].items():
        if status != "not_implemented":
            continue
        channel = key.split("channels.", 1)[1]
        block_match = re.search(rf"(?m)^  {channel}:\s*$", channel_section)
        if not block_match:
            fail(f"AsyncAPI channel {channel} missing")
            continue
        rest = channel_section[block_match.end():]
        following = re.search(r"(?m)^  \w+:\s*$", rest)
        block = rest[:following.start()] if following else rest
        if "x-contract-status: not_implemented" not in block:
            fail(f"AsyncAPI channel {channel} is not implemented but lacks x-contract-status: not_implemented")

    # 7. Contract claims must not exceed implemented schema enum values.
    for schema_field, implemented_values in registry.get("schema_enum_implemented_values", {}).items():
        schema, field = schema_field.split(".", 1)
        advertised = parse_schema_enum(openapi_source, schema, field)
        if advertised is None:
            fail(f"OpenAPI schema {schema}.{field} enum not found")
            continue
        outside = [value for value in advertised if value not in implemented_values]
        if outside:
            fail(
                f"OpenAPI {schema}.{field} advertises unimplemented values {outside}; "
                f"implemented set is {implemented_values}"
            )

    # 8. doc/01-contracts.md implementation-status table.
    table = read_doc_section(DOC_CONTRACTS, STATUS_SECTION_START, STATUS_SECTION_END)
    table_lines = table.splitlines()
    for key in sorted(doc_required):
        expected = categories["proto_fields"].get(key)
        if expected is None:
            for category in categories.values():
                if key in category:
                    expected = category[key]
                    break
        matching = [line for line in table_lines if key in line]
        if not matching:
            fail(f"doc/01-contracts.md status table omits {key} ({expected})")
            continue
        if not any(expected in line for line in matching):
            fail(f"doc/01-contracts.md status table does not mark {key} as {expected}")

    # 9. Frozen vocabulary must be documented and present in implementation sources.
    vocabulary = registry.get("vocabulary", {})
    vocabulary_section = read_doc_section(DOC_CONTRACTS, VOCABULARY_START, VOCABULARY_END)
    sources = implementation_text()
    for group in ("features", "subscriptions", "permissions", "adapter_capabilities"):
        for name in vocabulary.get(group, []):
            if name not in vocabulary_section:
                fail(f"doc/01-contracts.md vocabulary section omits {group} name {name}")
            if name not in sources:
                fail(f"implementation sources do not reference declared {group} name {name}")

    try:
        target_table = read_doc_section(DOC_CONTRACTS, "<!-- execution-scopes:start -->",
                                       "<!-- execution-scopes:end -->")
        for category, values in categories.items():
            for key in values:
                if effective_status(registry, "target", category, key) == "implemented":
                    if not any(f"`{key}`" in line and "| implemented |" in line
                               for line in target_table.splitlines()):
                        fail(f"target production status table omits {category}:{key}")
    except ValueError as error:
        fail(str(error))

    if errors:
        print("contract implementation status check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(
        "Contract implementation status, API extensions, evidence, documentation table "
        "and reference vocabulary are consistent (historical status is not target availability)"
    )
    target_items = [f"{category}:{key}" for category, values in categories.items() for key in values
                    if effective_status(registry, "target", category, key) == "implemented"]
    print("Target production implemented: " + (", ".join(target_items) or "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
