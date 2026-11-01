# P04 Control evidence

## Configuration and endpoint ownership

Scope: Control outer transport, service configuration and the explicitly approved
minimal outer endpoint lifecycle. Input: validated ProcessConfig and optional
control_api listen/identity_endpoint. Output: exclusive endpoint start/check/stop;
configuration failures exit 2, local startup/runtime failures exit 1. No protocol,
API, inner application or database implementation change.

Documentation impact: doc/06 and doc/08 updated for listener configuration and
ownership. proto/, api/, doc/01 unchanged: existing wire/error semantics remain
binding. doc/18 is protected and untouched. No other module documentation needs
updating because ownership and behavior outside Control are unchanged.

Actual checks (full-locked uses -Werror):
- cmake --build build/full-locked --target kiarabot_service_config_driver kiarabot_service_composition_driver -j4
- PATH=<locked OpenSSL>:$PATH python3 core/tests/platform/service_config_test.py build/full-locked/kiarabot_service_config_driver: passed
- PATH=<locked OpenSSL>:$PATH python3 core/tests/platform/service_composition_test.py build/full-locked/kiarabot_service_composition_driver build/full-locked/kiarabot_service_rpc_client: passed
- locked clang-format 16 applied to changed C++.

The existing composition driver registration lacks its direct proto link and second
CTest argument on this source base; local temporary CMake links it for verification.
Build owner integration is required; no CMake modification is included in this commit.
Endpoint tests assert start failure, runtime failure and endpoint destruction before
application while dependencies remain alive. Configuration tests cover strict shape,
duplicate keys, unknown values, forbidden services, URL/DSN-like endpoints and ports.

## Identity mTLS authentication mapping and HTTPS endpoint

New outer interfaces: IdentityAuthClient::handle(AuthHttpRequest) -> AuthHttpResponse
and make_https_endpoint(ProcessConfig) -> unique_ptr<ServiceEndpoint>. No generated,
JSON or HTTP types enter application/ports. Control has no business DB or auth cache.
Login/Refresh keys pass through; protected requests revalidate, then List/Revoke use
exact Identity delegation with the original access proof and real certificate digest.

Actual -Werror build: cmake --build build/full-locked --target kiarabot-control
kiarabot_control_identity_fixture -j4. Temporary CMake registers the outer library,
fixture and Boost/OpenSSL dependencies; build owner receives exact registration.

Actual end-to-end command: PATH=<locked OpenSSL>:$PATH python3
control/api/tests/architecture/auth_https_test.py build/full-locked/bin/kiarabot-control
build/full-locked/kiarabot_control_identity_fixture. Passed real HTTPS -> mTLS gRPC
fixture Login/Refresh/Me/List/Revoke, unchanged retry keys, direct cert digest binding,
uint64 maximum, checked_at TTL floor, persisted timestamps, revoked/expired priority,
invalid/duplicate/unknown JSON and headers, query/UUID bounds, body/header/connection
limits, stable code mapping, no-detail INTERNAL, Identity partition and wrong CA,
2s upstream/queue budget, in-flight RPC and stalled TLS bounded shutdown, no secrets
in logs and no CORS. The fixture supplies deterministic wire responses, not business
policy or PG; real Identity application/bootstrap integration is still required.

clang-tidy 22 with clang-analyzer-* and warnings-as-errors on fixed full-locked
compile DB passed client/server/config/runner/composition driver (Apple SDK args).
Format16 applied; git diff --check passed. Full repo quality and link registration
are coordinated by build owner, not represented as checks performed here.
