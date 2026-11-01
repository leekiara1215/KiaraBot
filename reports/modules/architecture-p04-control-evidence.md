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

## Review correction: authoritative projection and failure facts

Set trace_id=request_id before every RPC. Successful ValidateSession projections now
require Active principal/version/binding, nonrevoked current session, canonical UUIDv4
references, positive/exact versions and valid ordered persisted/checked expiry times.
All times use the common pure time_point_from_parts range guard. Token pair order and
page snapshot identity/expiry/page-size/uniqueness/order/subject consistency are checked.
No local grant decision was added. Invalid successful wire data fails INTERNAL/500.

RpcFailure details are decoded before UNAVAILABLE/DEADLINE categories. Code/status and
correlation must match; full stable failure_context is projected without secret text.
Transport-only failure carries no invented acceptance. Typed PLUGIN_UNAVAILABLE and
TIMEOUT fixtures preserve ACCEPTANCE_UNKNOWN, operation/resource, maximum uint64 current
version, retry delay and retryable. Bad context/status, missing metadata, stale/disabled/
revoked current identity and malformed UUID/time/version/page/token responses are tested.
Media type and bearer scheme case-insensitivity/UTF-8 charset are also verified.

Actual revalidation: -Werror Control+fixture build and auth_https_test.py passed after
these corrections. ctest --test-dir build/full-locked -R
'^(p02_service_config|p03_service_composition|p02_control_application)$'
--output-on-failure passed 3/3 after applying reviewed build dependency 3a8d3a2.
Documentation local-link validation (doc/06, doc/08, this report), diff whitespace and
protected doc/18 unchanged checks passed. Common target transport codec integration and
real Identity application/PG/bootstrap remain pending external companion changes.

Final focused clang-tidy22 re-run on changed authentication client and Identity
fixture passed with -warnings-as-errors after the projection/error corrections;
format16 and module-boundary checks passed. Baseline difference: this implements the
approved independent Control -> Identity target (doc/19–23), while frozen doc/18
remains unchanged. control/api/README.md still describes historical same-process
SQLite/reference deployment; it was not edited under this task's restricted document
scope and is reported to the parent for coordinated documentation cleanup.
