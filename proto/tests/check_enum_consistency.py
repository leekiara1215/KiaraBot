#!/usr/bin/env python3
"""Check every v1 enum: frozen numbers, ordering rules, and API vocabularies.

The gate intentionally does not need protoc: it parses the .proto sources and the
reviewed proto/tests/enum_baseline.json fixture. Renumbering, renaming, dropping
a published value, or advertising a value the API cannot express all fail here.
"""

import json
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
PROTO_DIR = ROOT / "proto" / "kiarabot" / "v1"
BASELINE = ROOT / "proto" / "tests" / "enum_baseline.json"
API_FILES = ("api/openapi.yaml", "api/asyncapi.yaml")

# Enums shared with the REST/WebSocket contracts. Their string vocabularies must
# stay name-for-name and order-for-order identical to the Protobuf definition.
SHARED_WITH_API = ("ErrorCode", "HealthState", "PluginState", "ActionStatus")

ENUM_BLOCK = re.compile(r"\b(enum|message)\s+(\w+)\s*\{")
ENUM_VALUE = re.compile(
    r"(?m)^\s*([A-Z][A-Z0-9_]*)\s*=\s*(-?\d+)\s*(?:\[[^\]]*\])?\s*;"
)


def strip_comments(source: str) -> str:
    return re.sub(r"//[^\n]*", "", source)


def find_block_end(source: str, open_brace: int) -> int:
    depth = 0
    for offset in range(open_brace, len(source)):
        character = source[offset]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return offset + 1
    raise ValueError("unbalanced braces in .proto source")


def collect_blocks(source: str, start: int = 0, end: Optional[int] = None,
                   parents: Tuple[str, ...] = ()):
    """Yield (kind, dotted_name, body, parent_names) for messages and enums."""
    end = len(source) if end is None else end
    position = start
    while position < end:
        match = ENUM_BLOCK.search(source, position, end)
        if not match:
            return
        kind, name = match.group(1), match.group(2)
        body_end = find_block_end(source, match.end() - 1)
        body = source[match.end():body_end - 1]
        yield kind, ".".join(parents + (name,)), body, parents
        if kind == "message":
            yield from collect_blocks(source, match.end(), body_end - 1, parents + (name,))
        position = body_end


def parse_proto_enums() -> Dict[str, List[Tuple[str, int]]]:
    enums: Dict[str, List[Tuple[str, int]]] = {}
    for path in sorted(PROTO_DIR.glob("*.proto")):
        source = strip_comments(path.read_text(encoding="utf-8"))
        for kind, dotted, body, parents in collect_blocks(source):
            if kind != "enum":
                continue
            if parents:
                full_name = "kiarabot.v1." + dotted
            else:
                full_name = "kiarabot.v1." + dotted
            if full_name in enums:
                raise ValueError(f"duplicate enum {full_name} in {path.name}")
            values = [(match.group(1), int(match.group(2))) for match in ENUM_VALUE.finditer(body)]
            if not values:
                raise ValueError(f"enum {full_name} in {path.name} has no values")
            if re.search(r"\boption\s+allow_alias\s*=\s*true\s*;", body):
                raise ValueError(
                    f"{full_name} uses allow_alias; v1 forbids alias values because renumbering "
                    "would become ambiguous (add a distinct value instead)"
                )
            enums[full_name] = values
    if not enums:
        raise ValueError("no Protobuf enums found")
    return enums


def load_baseline() -> Dict[str, List[Tuple[str, int]]]:
    document = json.loads(BASELINE.read_text(encoding="utf-8"))
    baseline: Dict[str, List[Tuple[str, int]]] = {}
    for name, values in document["enums"].items():
        baseline[name] = [(value_name, number) for value_name, number in values.items()]
    return baseline


def check_structure(name: str, values: List[Tuple[str, int]]) -> None:
    numbers: Dict[int, str] = {}
    for value_name, number in values:
        if number in numbers:
            raise ValueError(
                f"{name} repeats number {number} for {numbers[number]} and {value_name}"
            )
        numbers[number] = value_name
    if values[0][1] != 0 or not values[0][0].endswith("_UNSPECIFIED"):
        raise ValueError(
            f"{name} must start with a zero *_UNSPECIFIED value, got {values[0][0]}={values[0][1]}"
        )
    previous = values[0][1]
    for _, number in values[1:]:
        if number <= previous:
            raise ValueError(
                f"{name} numbers must increase from zero without aliases; "
                f"{number} follows {previous}"
            )
        previous = number


def check_against_baseline(
    actual: Dict[str, List[Tuple[str, int]]],
    baseline: Dict[str, List[Tuple[str, int]]],
) -> None:
    missing = sorted(set(baseline) - set(actual))
    if missing:
        raise ValueError(f"published enums disappeared or were renamed: {', '.join(missing)}")
    added = sorted(set(actual) - set(baseline))
    if added:
        raise ValueError(
            "new enums must be added to proto/tests/enum_baseline.json in the same change: "
            + ", ".join(added)
        )
    for name, expected in baseline.items():
        observed = actual[name]
        expected_names = [value_name for value_name, _ in expected]
        observed_names = [value_name for value_name, _ in observed]
        if observed_names != expected_names:
            raise ValueError(
                f"{name} value names/order changed: expected {expected_names}, got {observed_names}"
            )
        expected_numbers = dict(expected)
        observed_numbers = dict(observed)
        for value_name in expected_names:
            if expected_numbers[value_name] != observed_numbers[value_name]:
                raise ValueError(
                    f"{name}.{value_name} was renumbered: "
                    f"{expected_numbers[value_name]} -> {observed_numbers[value_name]}"
                )


def api_values(path: Path, name: str) -> List[str]:
    source = path.read_text(encoding="utf-8")
    match = re.search(
        rf"(?m)^    {name}:\s*\n\s+type:\s*string\s*\n\s+enum:\s*\[([^\]]+)\]",
        source,
    )
    if not match:
        raise ValueError(f"missing {name} schema in {path.relative_to(ROOT)}")
    return [value.strip() for value in match.group(1).split(",")]


def main() -> int:
    try:
        actual = parse_proto_enums()
        for name, values in actual.items():
            check_structure(name, values)
        baseline = load_baseline()
        check_against_baseline(actual, baseline)
        for simple_name in SHARED_WITH_API:
            full_name = f"kiarabot.v1.{simple_name}"
            if full_name not in actual:
                raise ValueError(f"missing shared enum {full_name}")
            expected = [value_name for value_name, _ in actual[full_name]]
            for relative_path in API_FILES:
                observed = api_values(ROOT / relative_path, simple_name)
                if observed != expected:
                    raise ValueError(
                        f"{simple_name} differs in {relative_path}: expected {expected}, got {observed}"
                    )
        print(
            "All Protobuf enums match the frozen name/number baseline; "
            "OpenAPI and AsyncAPI vocabularies match"
        )
        return 0
    except (ValueError, KeyError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
