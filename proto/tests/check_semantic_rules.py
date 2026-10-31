#!/usr/bin/env python3
"""Check cross-contract semantic rules that are easy to drift apart.

Covers the P0/P1 contract convergence items:
  * unified identifier syntax (doc table vs OpenAPI/AsyncAPI parameters);
  * Action deadline upper bound documentation and its contract status;
  * the gRPC vs in-band business-error correction;
  * reserved/deprecated rules in .proto sources and proto/README.md;
  * graceful shutdown/drain contract and its v1 not-implemented status;
  * REST Action retention/EXPIRED semantics and their divergence record;
  * explicit admin role list without wildcards and without message.read;
  * OpenAPI PluginManifest enums equal to the implemented value set.

Run: python3 proto/tests/check_semantic_rules.py
"""

import json
import re
import sys
from pathlib import Path
from typing import Dict, List

ROOT = Path(__file__).resolve().parents[2]
PROTO_DIR = ROOT / "proto" / "kiarabot" / "v1"
REGISTRY = ROOT / "proto" / "tests" / "contract_status.json"
DOC_CONTRACTS = ROOT / "doc" / "01-contracts.md"
DOC_RUNTIME = ROOT / "doc" / "01-runtime-semantics.md"
OPENAPI = ROOT / "api" / "openapi.yaml"
ASYNCAPI = ROOT / "api" / "asyncapi.yaml"
PROTO_README = ROOT / "proto" / "README.md"

ID_SECTION_START = "<!-- id-rules:start -->"
ID_SECTION_END = "<!-- id-rules:end -->"
NESTED_BLOCK = re.compile(r"\b(?:message|enum)\s+\w+\s*\{")


def section(path: Path, start: str, end: str) -> str:
    text = path.read_text(encoding="utf-8")
    if start not in text or end not in text:
        raise ValueError(f"missing markers {start}/{end} in {path.relative_to(ROOT)}")
    return text.split(start, 1)[1].split(end, 1)[0]


def block_end(source: str, open_brace: int) -> int:
    depth = 0
    for offset in range(open_brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return offset + 1
    raise ValueError("unbalanced braces")


def blank_nested(body: str) -> str:
    characters = list(body)
    for match in NESTED_BLOCK.finditer(body):
        end = block_end(body, match.end() - 1)
        for offset in range(match.start(), end):
            if characters[offset] != "\n":
                characters[offset] = " "
    return "".join(characters)


def proto_messages():
    for path in sorted(PROTO_DIR.glob("*.proto")):
        source = path.read_text(encoding="utf-8")
        cleaned = re.sub(r"//[^\n]*", "", source)
        for match in re.finditer(r"\bmessage\s+(\w+)\s*\{", cleaned):
            end = block_end(cleaned, match.end() - 1)
            body = blank_nested(cleaned[match.end():end - 1])
            yield path, match.group(1), body


def parse_openapi_parameter(source: str, name: str) -> Dict[str, str]:
    marker = f"\n    {name}:\n"
    if marker not in source:
        raise ValueError(f"OpenAPI parameter {name} missing")
    block = source.split(marker, 1)[1]
    following = re.search(r"(?m)^    \w+:\s*$", block)
    if following:
        block = block[:following.start()]
    fields: Dict[str, str] = {}
    for key in ("minLength", "maxLength"):
        match = re.search(rf"{key}:\s*(\d+)", block)
        if match:
            fields[key] = match.group(1)
    pattern = re.search(r"pattern:\s*'([^']*)'", block)
    if pattern:
        fields["pattern"] = pattern.group(1)
    return fields


def parse_roles(source: str) -> Dict[str, List[str]]:
    roles = {}
    for match in re.finditer(r"(?m)^    (admin|operator|viewer):\s*\[([^\]]*)\]", source):
        roles[match.group(1)] = [item.strip() for item in match.group(2).split(",")]
    return roles


def parse_action_results(source: str) -> Dict[str, str]:
    block_match = re.search(r"(?ms)^  action_results:\n(.*?)(?=^  [a-z_]+:)", source)
    if not block_match:
        return {}
    values: Dict[str, str] = {}
    for line in block_match.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        key, value = line.split(":", 1)
        values[key.strip()] = value.strip()
    return values


def main() -> int:
    errors: List[str] = []

    def fail(message: str) -> None:
        errors.append(message)

    try:
        registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
        semantic = registry["categories"]["semantic_rules"]
        doc = DOC_CONTRACTS.read_text(encoding="utf-8")
        runtime = DOC_RUNTIME.read_text(encoding="utf-8")
        openapi = OPENAPI.read_text(encoding="utf-8")
        asyncapi = ASYNCAPI.read_text(encoding="utf-8")
        proto_readme = PROTO_README.read_text(encoding="utf-8")
        id_rules = section(DOC_CONTRACTS, ID_SECTION_START, ID_SECTION_END)
    except (ValueError, KeyError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 1

    # 1. Identifier syntax: documented classes match the API parameters.
    required_phrases = (
        ("action_id", "128", "[A-Za-z0-9._-]"),
        ("adapter_id", "128", "[A-Za-z0-9._:@-]"),
        ("conversation_id", "256", "可见 ASCII"),
        ("插件 `id`", "64", "[a-z0-9.-]"),
        ("session_id", "32", "256"),
    )
    for identifier, length, charset in required_phrases:
        row = [line for line in id_rules.splitlines() if identifier in line]
        if not row:
            fail(f"identifier rules table omits {identifier}")
            continue
        if not any(length in line and charset in line for line in row):
            fail(f"identifier rules row for {identifier} must state length {length} and charset {charset}")
    try:
        action_id = parse_openapi_parameter(openapi, "ActionId")
        action_key = parse_openapi_parameter(openapi, "ActionLookupKey")
        session_id = parse_openapi_parameter(openapi, "SessionId")
        plugin_id = parse_openapi_parameter(openapi, "PluginId")
    except ValueError as error:
        fail(str(error))
        action_id = action_key = session_id = plugin_id = {}
    for name, parameter in (("ActionId", action_id), ("ActionLookupKey", action_key)):
        if parameter.get("maxLength") != "128":
            fail(f"OpenAPI {name} maxLength must be 128, got {parameter.get('maxLength')}")
        if parameter.get("pattern") != "^[A-Za-z0-9._-]+$":
            fail(f"OpenAPI {name} pattern must be ^[A-Za-z0-9._-]+$, got {parameter.get('pattern')}")
    if session_id.get("maxLength") != "256":
        fail(f"OpenAPI SessionId maxLength must be 256, got {session_id.get('maxLength')}")
    if plugin_id.get("pattern") != "^[a-z0-9][a-z0-9.-]{1,63}$":
        fail(f"OpenAPI PluginId pattern drift: {plugin_id.get('pattern')}")
    session_schema = re.search(
        r"session_id:\s*\{\s*type:\s*string,\s*minLength:\s*(\d+)", asyncapi
    )
    if not session_schema or session_schema.group(1) != "32":
        fail("AsyncAPI session_id must declare minLength 32")

    # 2. Action deadline upper bound and its status.
    if "300 秒" not in doc:
        fail("doc/01-contracts.md must document the 300 second Action deadline bound")
    if semantic.get("semantic.action.deadline_max_300s") != "not_implemented":
        fail("semantic.action.deadline_max_300s must stay not_implemented until enforced")
    if not any("300" in path.read_text(encoding="utf-8") for path in
               (PROTO_DIR / "actions.proto",)):
        fail("actions.proto must document the 300 second deadline bound")

    # 3. gRPC vs in-band error correction.
    if "in-band" not in doc:
        fail("doc/01-contracts.md must state that business errors use in-band Error frames")
    if "gRPC 状态映射为：" in doc:
        fail("doc/01-contracts.md still contains the deprecated normative gRPC mapping phrase")
    if semantic.get("semantic.grpc.business_error_mapping") != "deprecated":
        fail("semantic.grpc.business_error_mapping must be deprecated")
    for status in ("UNAUTHENTICATED", "PERMISSION_DENIED", "INVALID_ARGUMENT", "INTERNAL",
                   "CANCELLED", "DEADLINE_EXCEEDED"):
        if status not in doc:
            fail(f"doc/01-contracts.md must list the transport gRPC status {status}")

    # 4. reserved/deprecated rules are executable.
    for keyword in ("reserved", "deprecated", "编号永不复用"):
        if keyword not in proto_readme:
            fail(f"proto/README.md must document the {keyword!r} compatibility rule")
    for path, message, body in proto_messages():
        fields = {}
        for match in re.finditer(
            r"(?m)^\s*(?:repeated\s+|optional\s+)?(map<[^>]+>|[\w.]+)\s+(\w+)\s*=\s*(\d+)\s*;",
            body,
        ):
            fields[int(match.group(3))] = match.group(2)
        for reserved in re.finditer(r"\breserved\s+([^;]+);", body):
            for item in reserved.group(1).split(","):
                item = item.strip()
                if not item:
                    continue
                if " to " in item:
                    start_text = item.split(" to ")[0].strip()
                    end_text = item.split(" to ")[1].strip()
                    if start_text == "max":
                        start_number = max(fields) + 1 if fields else 1
                    else:
                        start_number = int(start_text)
                    end_number = 536870911 if end_text == "max" else int(end_text)
                else:
                    start_number = end_number = int(item)
                overlap = [number for number in fields if start_number <= number <= end_number]
                if overlap:
                    fail(
                        f"{path.name}:{message} reserved range {item} overlaps existing "
                        f"field numbers {overlap}"
                    )
    source_all = "\n".join(path.read_text(encoding="utf-8") for path in PROTO_DIR.glob("*.proto"))
    if re.search(r"\ballow_alias\s*=\s*true\b", source_all):
        fail("v1 must not use allow_alias; aliases make enum renumbering undetectable")

    # 5. Graceful shutdown / drain contract.
    if "ShutdownNotice" not in source_all:
        fail("common.proto must define ShutdownNotice for the drain contract")
    for frame in ("shutdown = 5;", "shutdown = 8;"):
        if frame not in source_all:
            fail(f"Bridge oneof is missing the drain frame field {frame}")
    if semantic.get("semantic.transport.graceful_shutdown") != "not_implemented":
        fail("semantic.transport.graceful_shutdown must stay not_implemented")
    if "优雅停机" not in doc or "ShutdownNotice" not in doc:
        fail("doc/01-contracts.md must contain the graceful shutdown/drain contract")
    if "未知结果" not in runtime:
        fail("doc/01-runtime-semantics.md must state that an unsignalled stream close is an unknown outcome")

    # 6. REST Action query retention/EXPIRED semantics and divergence.
    for phrase in ("604800", "EXPIRED", "TIMEOUT", "NOT_FOUND"):
        if phrase not in doc:
            fail(f"doc/01-contracts.md must state REST Action query semantic {phrase!r}")
    for phrase in ("168", "EXPIRED"):
        if phrase not in runtime:
            fail(f"doc/01-runtime-semantics.md must state the Action retention/EXPIRED rule {phrase!r}")
    action_results = parse_action_results(openapi)
    expected = {
        "terminal_retention_seconds": "604800",
        "missing_expired_invisible": "NOT_FOUND",
        "deadline_transition": "EXPIRED",
        "expired_error_code": "TIMEOUT",
    }
    for key, value in expected.items():
        if action_results.get(key) != value:
            fail(f"OpenAPI action_results.{key} must be {value}, got {action_results.get(key)!r}")
    if semantic.get("semantic.rest.action_query_terminal_retention") != "not_implemented":
        fail("semantic.rest.action_query_terminal_retention must stay not_implemented")
    if semantic.get("semantic.rest.action_query_deadline_expired") != "not_implemented":
        fail("semantic.rest.action_query_deadline_expired must stay not_implemented")

    # 7. Explicit admin role list, no wildcard, no default message.read.
    roles = parse_roles(openapi)
    if set(roles) != {"admin", "operator", "viewer"}:
        fail(f"OpenAPI roles must be admin/operator/viewer, got {sorted(roles)}")
    for role, grants in roles.items():
        if "*" in grants:
            fail(f"role {role} must not use wildcard grants")
        if "message.read" in grants:
            fail(f"role {role} must not grant message.read by default")
    if "message.read" in roles.get("admin", []):
        fail("admin must not receive message.read before the messages channel is implemented")
    for required in ("audit.read", "user.read"):
        if required not in roles.get("admin", []):
            fail(f"admin role must explicitly list {required}")
    if "No default role grants message.read" not in asyncapi:
        fail("AsyncAPI x-control-session-policy must state that no default role grants message.read")
    if semantic.get("semantic.rest.admin_role_wildcard") != "deprecated":
        fail("semantic.rest.admin_role_wildcard must be deprecated")

    # 8. PluginManifest enums equal the implemented value set.
    schema_enum_values = registry["schema_enum_implemented_values"]
    for schema_field, implemented in schema_enum_values.items():
        schema, field = schema_field.split(".", 1)
        marker = f"\n    {schema}:\n"
        if marker not in openapi:
            fail(f"OpenAPI schema {schema} missing")
            continue
        block = openapi.split(marker, 1)[1]
        following = re.search(r"(?m)^    \w+:\s*$", block)
        if following:
            block = block[:following.start()]
        match = re.search(rf"{field}:\s*\{{[^}}]*enum:\s*\[([^\]]*)\][^}}]*\}}", block)
        if not match:
            fail(f"OpenAPI {schema}.{field} enum missing")
            continue
        advertised = [item.strip() for item in match.group(1).split(",") if item.strip()]
        if sorted(advertised) != sorted(implemented):
            fail(
                f"OpenAPI {schema}.{field} advertises {advertised}, "
                f"implemented set is {implemented}"
            )

    if errors:
        print("contract semantic rules check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("Identifier, deadline, gRPC/in-band, reserved, drain, Action-query, role and manifest rules match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
