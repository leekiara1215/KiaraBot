# Protocol tests

For the Action feature negotiation and legacy Python reader test, generate the SDK types, then run `PYTHONPATH=sdk/python/build/generated sdk/python/.venv/bin/python proto/tests/action_contract_test.py` from the root. This verifies legacy readers preserve unknown fields, legacy servers default to no enabled features, query selectors and unknown status values. The C++ roundtrip test also covers the new frames and unchanged field numbers. This wire test does not certify that a service implements Action dispatch.

`check_contracts.sh` uses Python 3 for exact enum vocabulary checks, performs static consistency checks, and compiles every v1 `.proto` file into a descriptor set. It intentionally exits with code `2` when `protoc` is unavailable so CI cannot report a false green result.

Run it from the repository root:

```bash
./proto/tests/check_contracts.sh
```

CI can set `KIARABOT_BASELINE_DESCRIPTOR` to a descriptor from the previous released protocol. The script invokes the generated `kiarabot_descriptor_compatibility` tool (or the path in `KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL`) and checks the semantic API surface: old files, messages, fields, oneofs, nested types, reserved ranges, enum values, services and method signatures/options must remain compatible. New fields and enum values are allowed. It compares the descriptor model rather than descriptor bytes, so compiler metadata and serialization ordering do not create false incompatibilities.

After building the proto preset, run `./proto/tests/descriptor_compatibility_test.sh` to exercise compatible additions and incompatible field, enum, oneof and service changes against generated descriptors. This fixture test does not replace comparison with a released baseline descriptor.

Run `ruby proto/tests/api_contract_test.rb` to verify channel envelope/payload references, session handshakes, unique message IDs, Action lookup error responses and terminal result requirements. This test uses Ruby's standard YAML library.

The test verifies that:

- Event and Action envelopes use `oneof` as their only discriminator;
- Protobuf, OpenAPI and AsyncAPI expose the same error, health, plugin and action status vocabularies;
- OpenAPI declares `Idempotency-Key`;
- AsyncAPI binds bearer authentication and references hello/resume/resync and `/ws/messages`;
- all Protobuf sources compile with imports into a descriptor.
- generated C++ messages serialize and parse with the same oneof discriminator.
