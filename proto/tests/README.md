# Protocol tests

`check_contracts.sh` performs static consistency checks and compiles every v1 `.proto` file into a descriptor set. It intentionally exits with code `2` when `protoc` is unavailable so CI cannot report a false green result.

Run it from the repository root:

```bash
./proto/tests/check_contracts.sh
```

CI can set `KIARABOT_BASELINE_DESCRIPTOR` to a descriptor from the previous released protocol. The script invokes the generated `kiarabot_descriptor_compatibility` tool (or the path in `KIARABOT_DESCRIPTOR_COMPATIBILITY_TOOL`) and checks the semantic API surface: old files, messages, fields, enum numbers and service method signatures must remain compatible. New fields and enum values are allowed. It no longer compares descriptor bytes directly, so compiler metadata and serialization ordering do not create false incompatibilities.

The test verifies that:

- Event and Action envelopes use `oneof` as their only discriminator;
- Protobuf and OpenAPI contain the same extended error codes;
- OpenAPI declares `Idempotency-Key`;
- AsyncAPI binds bearer authentication and references hello/resume/resync and `/ws/messages`;
- all Protobuf sources compile with imports into a descriptor.
- generated C++ messages serialize and parse with the same oneof discriminator.
